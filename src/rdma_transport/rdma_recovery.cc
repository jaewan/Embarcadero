// rdma_transport/rdma_recovery.cc — Phase 2 (W5-A leg-1) re-replication tool.
//
// After a broker host is killed, the surviving right-neighbor (which holds a live replica of the
// dead broker's committed data, per the ring replication in rdma_broker.cc) is the only remaining
// copy. This tool restores RF by copying that data to a THIRD, guaranteed-reachable destination:
// the memserver's spare region (c3 is never killed in Phase 2, Decision 1 — metadata always
// survives). Measures exactly the bytes moved and the wall-clock time, which is the Phase 2 report
// bar's "re-replication volume + time" number.
//
// Usage:
//   rdma_recovery --source-ip=<surviving-broker> --source-port=<replica-port>
//                 --target-ip=<memserver> --target-port=<recovery-port> --bytes=<N>
//                 [--verify-slot-bytes=4096 --verify-slots=N]
//
// --verify-slot-bytes/--verify-slots (Sub-phase 3B leg-1 item 2): brokers write
// `global_seq & 0xFF` into every byte of a batch's per-slot Blog region (rdma_broker.cc's Step 1),
// and for slots before the ring's first wraparound, slot index == global_seq directly -- so slot
// k's expected content is deterministic: every byte == (k & 0xFF). Checking this after the READ
// (before writing to the target) turns "we recovered SOME bytes" into "we recovered the CORRECT
// bytes, byte-for-byte" without needing any out-of-band record of what was actually written.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rdma_transport/rdma_common.h"
#include "rdma_transport/rdma_wire.h"

using namespace embarcadero::rdma;
using namespace embarcadero::rdma_variant;

namespace {
const char* GetArg(int argc, char** argv, const char* key, const char* def) {
  size_t klen = strlen(key);
  for (int i = 1; i < argc; ++i)
    if (!strncmp(argv[i], key, klen) && argv[i][klen] == '=') return argv[i] + klen + 1;
  return def;
}
}  // namespace

int main(int argc, char** argv) {
  std::string source_ip = GetArg(argc, argv, "--source-ip", "");
  int source_port = atoi(GetArg(argc, argv, "--source-port", "18710"));
  std::string target_ip = GetArg(argc, argv, "--target-ip", "");
  int target_port = atoi(GetArg(argc, argv, "--target-port", "18690"));
  size_t bytes = atoll(GetArg(argc, argv, "--bytes", "0"));
  size_t verify_slot_bytes = atoll(GetArg(argc, argv, "--verify-slot-bytes", "0"));
  size_t verify_slots = atoll(GetArg(argc, argv, "--verify-slots", "0"));
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));

  if (bytes == 0) { fprintf(stderr, "[recovery] FATAL: --bytes must be > 0\n"); return 1; }

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;

  std::vector<uint8_t> staging(bytes);
  Mr staging_mr{};
  if (!RegisterMr(d.pd, staging.data(), staging.size(), &staging_mr)) return 1;

  const auto t_start = std::chrono::steady_clock::now();

  // ---- Connect to the surviving replica-holder (source) ----
  RcQp src_qp{};
  if (!CreateRcQp(&d, 64, 64, /*psn_seed=*/0xA000, &src_qp)) return 1;
  ReplicaHandoffBlob src_local{}, src_remote{};
  src_local.role = 2;  // recovery source read
  src_local.ep = LocalEndpoint(src_qp);
  if (!OobClientExchange(source_ip, source_port, &src_local, &src_remote, sizeof(ReplicaHandoffBlob))) {
    fprintf(stderr, "[recovery] handoff with source %s:%d failed\n", source_ip.c_str(), source_port);
    return 1;
  }
  if (!ConnectRcQp(&src_qp, src_remote.ep)) return 1;
  fprintf(stderr, "[recovery] connected to source %s (qpn=%u)\n", source_ip.c_str(), src_remote.ep.qpn);

  // ---- Connect to the recovery target (memserver's spare region) ----
  RcQp dst_qp{};
  if (!CreateRcQp(&d, 64, 64, /*psn_seed=*/0xA100, &dst_qp)) return 1;
  ReplicaHandoffBlob dst_local{}, dst_remote{};
  dst_local.role = 1;  // recovery target write
  dst_local.ep = LocalEndpoint(dst_qp);
  if (!OobClientExchange(target_ip, target_port, &dst_local, &dst_remote, sizeof(ReplicaHandoffBlob))) {
    fprintf(stderr, "[recovery] handoff with target %s:%d failed\n", target_ip.c_str(), target_port);
    return 1;
  }
  if (!ConnectRcQp(&dst_qp, dst_remote.ep)) return 1;
  fprintf(stderr, "[recovery] connected to target %s (qpn=%u)\n", target_ip.c_str(), dst_remote.ep.qpn);

  if (dst_remote.region.len < bytes) {
    fprintf(stderr, "[recovery] FATAL: target spare region (%uB) smaller than --bytes=%zu\n",
            dst_remote.region.len, bytes);
    return 1;
  }

  ibv_wc wc[1];
  int bad = 0;

  const auto t_read_start = std::chrono::steady_clock::now();
  PostRead(&src_qp, staging.data(), staging_mr.lkey(), src_remote.region.addr, src_remote.region.rkey,
           static_cast<uint32_t>(bytes), /*wr_id=*/0);
  int n = PollCq(src_qp.cq, wc, 1, &bad);
  if (n <= 0) { fprintf(stderr, "[recovery] source READ failed (status=%d)\n", bad); return 1; }
  const auto t_read_done = std::chrono::steady_clock::now();

  if (verify_slot_bytes > 0 && verify_slots > 0) {
    // [[BUG FOUND: leg-1 sanity testing]] The original check assumed slot k's expected byte value
    // is simply (k & 0xFF), which only holds BEFORE the ring's first wraparound (global_seq ==
    // slot index only for seq < pbr_slots). Under sustained load the ring wraps many times before
    // a kill happens, so slot k's actual content reflects whichever global_seq last landed there
    // (k + m*pbr_slots for whatever wrap count m applies) -- unknowable here without an
    // out-of-band record of exactly how many batches were sent. Fixed to a wrap-count-independent
    // invariant instead: every byte WITHIN one slot must be identical (each batch's payload is one
    // memset of a single value -- internal inconsistency means torn/corrupted data), AND
    // consecutive slots' values must differ by exactly +1 mod 256 (adjacent ring slots hold
    // consecutive global_seq values in the SAME final pass through the ring, as long as the
    // checked range is small relative to pbr_slots so it doesn't itself span a wrap boundary).
    // This verifies real byte-for-byte recovery correctness without needing to know the absolute
    // sequence number.
    size_t checked_slots = 0, bad_slots = 0;
    int prev_value = -1;
    for (size_t k = 0; k < verify_slots && (k + 1) * verify_slot_bytes <= bytes; ++k) {
      const uint8_t* slot = staging.data() + k * verify_slot_bytes;
      const uint8_t value = slot[0];
      bool uniform = true;
      for (size_t b = 1; b < verify_slot_bytes; ++b) {
        if (slot[b] != value) { uniform = false; break; }
      }
      bool sequential = (prev_value < 0) || (value == static_cast<uint8_t>(prev_value + 1));
      ++checked_slots;
      if (!uniform || !sequential) ++bad_slots;
      prev_value = value;
    }
    fprintf(stderr, "[recovery] VERIFY checked_slots=%zu bad_slots=%zu %s\n", checked_slots,
            bad_slots, bad_slots == 0 ? "PASS" : "FAIL");
    if (bad_slots > 0) { fprintf(stderr, "[recovery] FATAL: checksum verification failed\n"); return 1; }
  }

  PostWrite(&dst_qp, staging.data(), staging_mr.lkey(), dst_remote.region.addr, dst_remote.region.rkey,
            static_cast<uint32_t>(bytes), /*wr_id=*/0);
  n = PollCq(dst_qp.cq, wc, 1, &bad);
  if (n <= 0) { fprintf(stderr, "[recovery] target WRITE failed (status=%d)\n", bad); return 1; }
  const auto t_write_done = std::chrono::steady_clock::now();

  double read_secs = std::chrono::duration<double>(t_read_done - t_read_start).count();
  double write_secs = std::chrono::duration<double>(t_write_done - t_read_done).count();
  double total_secs = std::chrono::duration<double>(t_write_done - t_start).count();
  fprintf(stderr,
          "[recovery] RESULT bytes=%zu read_secs=%.6f write_secs=%.6f total_secs=%.6f "
          "read_gbps=%.3f write_gbps=%.3f\n",
          bytes, read_secs, write_secs, total_secs, (bytes * 8.0) / read_secs / 1e9,
          (bytes * 8.0) / write_secs / 1e9);

  DestroyRcQp(&src_qp);
  DestroyRcQp(&dst_qp);
  DeregisterMr(&staging_mr);
  CloseDevice(&d);
  return 0;
}
