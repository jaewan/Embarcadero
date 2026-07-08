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
//                 [--pbr-slots=N --seam-final-seq=N]
//                 [--dump-file=<path>] | [--verify-only=<path> --bytes=<N> ...verify flags...]
//
// --verify-slot-bytes/--verify-slots (Sub-phase 3B leg-1 item 2): brokers write
// `global_seq & 0xFF` into every byte of a batch's per-slot Blog region (rdma_broker.cc's Step 1).
// This is a STRUCTURAL / low-8-bit sequential-pattern check, NOT full byte-for-byte identity
// verification: it catches torn writes (non-uniform bytes within one slot), zeroed/dropped/
// duplicated/stale slots (broken +1 sequencing), but two different full 64-bit global_seq values
// that happen to share the same low byte at the same ring offset would NOT be distinguished by
// this check alone. It is a real, useful integrity check on the recovered data, not a claim of
// full identity verification.
//
// --pbr-slots/--seam-final-seq (Sub-phase 3B review fix, replacing an earlier "1 mismatch, PASS by
// hand" escape hatch): under sustained load the ring wraps many times before a kill, so the
// checked window can straddle the boundary between the ring's newest generation and the
// still-resident previous generation ("the seam") -- exactly one real, EXPECTED discontinuity, not
// data corruption. Rather than eyeballing this, the seam's physical slot index is derived
// PROGRAMMATICALLY from the actual final published sequence number for the source broker
// (--seam-final-seq, i.e. the value the memserver's end-of-run dump prints as
// "sentinel[broker=N]=<value>" for that broker -- the same number the surviving replica-holder
// last received from it before it died) modulo --pbr-slots. Only the single sequential-delta check
// spanning that exact boundary is skipped; every other slot (including the seam's own two
// neighbors) still gets the full uniform-byte + sequential-delta check, and any OTHER mismatch --
// off-by-anything-but-the-known-seam -- still fails the run. When --seam-final-seq/--pbr-slots
// are not given, verification is strict everywhere (no exclusion), matching the pre-review tool's
// behavior for callers that don't have a wraparound to account for.
//
// --dump-file (write-only) / --verify-only (read-only, no RDMA): a live kill trial's recovery step
// runs the READ+VERIFY+WRITE sequence before the memserver's own end-of-run sentinel dump exists
// (the memserver only prints it at its own exit, after the recovery write already completed) -- so
// --seam-final-seq isn't known yet at that point in time. --dump-file lets that first pass save the
// just-read bytes to a local file for later, offline re-scoring once the sentinel value is known
// (via a second, --verify-only invocation that does no RDMA at all); this splits "move the bytes"
// (timed, on the critical path) from "score the bytes" (can happen after the fact) without
// re-reading over RDMA a second time.

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

// Structural / low-8-bit sequential-pattern check (see file header). Returns true iff
// bad_slots == 0. Prints the VERIFY line itself (checked_slots/bad_slots/excluded_seam/PASS-FAIL).
bool VerifyStagingBuffer(const std::vector<uint8_t>& staging, size_t bytes, size_t verify_slot_bytes,
                          size_t verify_slots, int64_t seam_slot) {
  size_t checked_slots = 0, bad_slots = 0;
  int prev_value = -1;
  for (size_t k = 0; k < verify_slots && (k + 1) * verify_slot_bytes <= bytes; ++k) {
    const uint8_t* slot = staging.data() + k * verify_slot_bytes;
    const uint8_t value = slot[0];
    bool uniform = true;
    for (size_t b = 1; b < verify_slot_bytes; ++b) {
      if (slot[b] != value) { uniform = false; break; }
    }
    // The one known/expected discontinuity: the transition INTO the slot right after the seam
    // (seam_slot -> seam_slot+1) crosses from the ring's newest generation into the still-resident
    // previous generation, so the usual "+1 mod 256" invariant does not apply there. Every other
    // transition, including any OTHER slot at or adjacent to the seam, is still checked normally.
    bool is_seam_transition = (seam_slot >= 0) && (k > 0) && (static_cast<int64_t>(k - 1) == seam_slot);
    bool sequential = (prev_value < 0) || is_seam_transition ||
                       (value == static_cast<uint8_t>(prev_value + 1));
    ++checked_slots;
    if (!uniform || !sequential) ++bad_slots;
    prev_value = value;
  }
  fprintf(stderr,
          "[recovery] VERIFY checked_slots=%zu bad_slots=%zu excluded_seam_slot=%lld %s\n",
          checked_slots, bad_slots, static_cast<long long>(seam_slot),
          bad_slots == 0 ? "PASS" : "FAIL");
  return bad_slots == 0;
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
  size_t pbr_slots = atoll(GetArg(argc, argv, "--pbr-slots", "0"));
  long long seam_final_seq = atoll(GetArg(argc, argv, "--seam-final-seq", "-1"));
  std::string dump_file = GetArg(argc, argv, "--dump-file", "");
  std::string verify_only_file = GetArg(argc, argv, "--verify-only", "");
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));

  if (bytes == 0) { fprintf(stderr, "[recovery] FATAL: --bytes must be > 0\n"); return 1; }

  int64_t seam_slot = (pbr_slots > 0 && seam_final_seq >= 0)
                           ? static_cast<int64_t>(static_cast<uint64_t>(seam_final_seq) % pbr_slots)
                           : -1;

  // ---- Offline verify-only path: no RDMA at all, just re-score a --dump-file'd capture now that
  // the seam is known (see file header). ----
  if (!verify_only_file.empty()) {
    std::vector<uint8_t> staging(bytes);
    FILE* f = fopen(verify_only_file.c_str(), "rb");
    if (!f) { fprintf(stderr, "[recovery] FATAL: cannot open --verify-only file %s\n",
                       verify_only_file.c_str()); return 1; }
    size_t got = fread(staging.data(), 1, bytes, f);
    fclose(f);
    if (got != bytes) {
      fprintf(stderr, "[recovery] FATAL: --verify-only file too short (got %zu, want %zu)\n", got, bytes);
      return 1;
    }
    if (verify_slot_bytes == 0 || verify_slots == 0) {
      fprintf(stderr, "[recovery] FATAL: --verify-only requires --verify-slot-bytes/--verify-slots\n");
      return 1;
    }
    bool ok = VerifyStagingBuffer(staging, bytes, verify_slot_bytes, verify_slots, seam_slot);
    return ok ? 0 : 1;
  }

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

  if (!dump_file.empty()) {
    FILE* f = fopen(dump_file.c_str(), "wb");
    if (!f || fwrite(staging.data(), 1, bytes, f) != bytes) {
      fprintf(stderr, "[recovery] WARNING: --dump-file write to %s failed\n", dump_file.c_str());
    }
    if (f) fclose(f);
  }

  if (verify_slot_bytes > 0 && verify_slots > 0) {
    if (!VerifyStagingBuffer(staging, bytes, verify_slot_bytes, verify_slots, seam_slot)) {
      fprintf(stderr, "[recovery] FATAL: checksum verification failed\n");
      return 1;
    }
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
