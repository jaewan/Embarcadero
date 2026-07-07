// rdma_transport/rdma_sequencer.cc — W5 sequencer role (spec §3.3 "Poll / read", identical
// mechanism in both variants except the Blog READ's endpoint).
//
// Loop:
//   1. RDMA READ the WHOLE packed sentinel array (N brokers x 8B) from the memserver in ONE
//      contiguous op (spec E2 — a gather READ over the per-broker PBR *rings* is impossible; an
//      RDMA READ's SGE scatters into LOCAL buffers only, so this dedicated contiguous array is
//      the fix).
//   2. For each broker whose sentinel value advanced since the last poll: RDMA READ that broker's
//      current PBR slot (full 128B header, more conservative than the spec's literal "[0,80)" —
//      we re-verify HeaderPublishCommitted() using the header's OWN embedded sentinel field,
//      matching BatchHeaderPublishCommitted's torn-window discipline exactly rather than trusting
//      the sentinel-array value alone). Then RDMA READ the Blog payload at [log_idx,
//      log_idx+total_size) — broker-DIRECT (W5-A, this is the isolated leg-2-relevant hop) or from
//      the memserver (W5-B). Then advance the local GOI mirror + RDMA WRITE ControlBlock/CV/GOI.
//   3. Repeat until the run duration elapses.
//
// This is NOT wired into Track 01's real embarlet/topic.cc control plane (per the spec's boundary
// rule — Track 05 never edits topic.{cc,h}); it is a standalone, protocol-faithful measurement
// harness exercising the same wire layout, sentinel discipline, and RC same-QP ordering, so the
// bandwidth/latency/failure-detection numbers it produces are transport-real, not simulated.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rdma_transport/rdma_common.h"
#include "rdma_transport/rdma_wire.h"

using namespace embarcadero::rdma;
using namespace embarcadero::rdma_variant;

#ifndef BLOG_PLACEMENT_DRAM
#define BLOG_PLACEMENT_DRAM 0
#endif

namespace {

const char* GetArg(int argc, char** argv, const char* key, const char* def) {
  size_t klen = strlen(key);
  for (int i = 1; i < argc; ++i)
    if (!strncmp(argv[i], key, klen) && argv[i][klen] == '=') return argv[i] + klen + 1;
  return def;
}

uint64_t NowNsLocal() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct LatencyStats {
  std::vector<uint64_t> s;
  void Add(uint64_t ns) { s.push_back(ns); }
  uint64_t Pct(double p) {
    if (s.empty()) return 0;
    std::vector<uint64_t> t = s;
    std::sort(t.begin(), t.end());
    return t[static_cast<size_t>(p * (t.size() - 1))];
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string memserver_ip = GetArg(argc, argv, "--memserver-ip", "");
  int memserver_port = atoi(GetArg(argc, argv, "--memserver-port", "18600"));
  int num_brokers = atoi(GetArg(argc, argv, "--num-brokers", "2"));
  int pbr_slots = atoi(GetArg(argc, argv, "--pbr-slots", "4096"));
  int duration_secs = atoi(GetArg(argc, argv, "--duration", "10"));
  std::string broker_ips_csv = GetArg(argc, argv, "--broker-ips", "");  // W5-A only; CSV, index = broker_id
  int blog_port = atoi(GetArg(argc, argv, "--blog-port", "18700"));
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));

  fprintf(stderr, "[sequencer] placement=%s num_brokers=%d duration=%ds\n",
          BLOG_PLACEMENT_DRAM ? "DRAM(W5-A)" : "MEMSERVER(W5-B)", num_brokers, duration_secs);

  std::vector<std::string> broker_ips;
#if BLOG_PLACEMENT_DRAM
  {
    size_t pos = 0;
    std::string s = broker_ips_csv;
    while (!s.empty()) {
      size_t comma = s.find(',');
      broker_ips.push_back(comma == std::string::npos ? s : s.substr(0, comma));
      if (comma == std::string::npos) break;
      s = s.substr(comma + 1);
    }
    if (static_cast<int>(broker_ips.size()) != num_brokers) {
      fprintf(stderr, "[sequencer] FATAL: --broker-ips must list exactly num_brokers=%d IPs "
                      "(got %zu) for W5-A broker-direct Blog reads\n", num_brokers, broker_ips.size());
      return 1;
    }
  }
#endif

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;

  // ---- Connect to the memserver (metadata always; Blog too in W5-B) ----
  RcQp meta_qp{};
  if (!CreateRcQp(&d, 4096, 512, /*psn_seed=*/0x6000, &meta_qp)) return 1;
  HandshakeBlob local{}, remote{};
  local.role = 1;
  local.broker_id = -1;
  local.client_ep = LocalEndpoint(meta_qp);
  if (!OobClientExchange(memserver_ip, memserver_port, &local, &remote, sizeof(HandshakeBlob))) {
    fprintf(stderr, "[sequencer] OOB exchange with memserver failed\n"); return 1;
  }
  if (!ConnectRcQp(&meta_qp, remote.server_ep)) return 1;
  fprintf(stderr, "[sequencer] connected to memserver qpn=%u host_blog=%u\n", remote.server_ep.qpn,
          remote.host_blog);

  // ---- W5-A only: connect DIRECTLY to each broker for Blog reads ----
#if BLOG_PLACEMENT_DRAM
  std::vector<RcQp> blog_qps(num_brokers);
  std::vector<RegionDesc> blog_regions(num_brokers);
  for (int b = 0; b < num_brokers; ++b) {
    if (!CreateRcQp(&d, 64, 64, /*psn_seed=*/0x7000 + b, &blog_qps[b])) return 1;
    BlogHandoffBlob hlocal{}, hremote{};
    hlocal.ep = LocalEndpoint(blog_qps[b]);
    if (!OobClientExchange(broker_ips[b], blog_port, &hlocal, &hremote, sizeof(BlogHandoffBlob))) {
      fprintf(stderr, "[sequencer] Blog handoff with broker %d (%s) failed\n", b, broker_ips[b].c_str());
      return 1;
    }
    if (!ConnectRcQp(&blog_qps[b], hremote.ep)) return 1;
    blog_regions[b] = hremote.blog;
    fprintf(stderr, "[sequencer] broker-direct Blog connection to broker %d (%s) qpn=%u\n",
            b, broker_ips[b].c_str(), hremote.ep.qpn);
  }
#endif

  // ---- Local staging buffers, registered once ----
  std::vector<uint64_t> sentinel_local(num_brokers, kPublishUncommitted);
  Mr sentinel_local_mr{};
  if (!RegisterMr(d.pd, sentinel_local.data(), sentinel_local.size() * sizeof(uint64_t),
                  &sentinel_local_mr)) return 1;

  BatchHeaderMirror header_local{};
  Mr header_local_mr{};
  if (!RegisterMr(d.pd, &header_local, sizeof(header_local), &header_local_mr)) return 1;

  constexpr size_t kMaxPayload = 1 << 16;  // 64 KiB staging for Blog payload reads
  std::vector<uint8_t> payload_local(kMaxPayload);
  Mr payload_local_mr{};
  if (!RegisterMr(d.pd, payload_local.data(), payload_local.size(), &payload_local_mr)) return 1;

  ControlBlockMirror cb_local{};
  Mr cb_local_mr{};
  if (!RegisterMr(d.pd, &cb_local, sizeof(cb_local), &cb_local_mr)) return 1;

  CompletionVectorEntryMirror cv_local{};
  Mr cv_local_mr{};
  if (!RegisterMr(d.pd, &cv_local, sizeof(cv_local), &cv_local_mr)) return 1;

  GoiEntryMirror goi_local{};
  Mr goi_local_mr{};
  if (!RegisterMr(d.pd, &goi_local, sizeof(goi_local), &goi_local_mr)) return 1;

  std::vector<uint64_t> last_seen(num_brokers, kPublishUncommitted);
  std::vector<uint64_t> detect_ts(num_brokers, 0);
  uint64_t committed_seq = 0;
  uint64_t batches = 0, blog_bytes_read = 0, sentinel_polls = 0, stale_misses = 0;
  LatencyStats detect_to_commit;
  ibv_wc wc[16];
  int bad = 0;

  const auto t_start = std::chrono::steady_clock::now();
  const auto deadline = t_start + std::chrono::seconds(duration_secs);

  auto blocking_read = [&](RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey,
                           uint32_t len) -> bool {
    PostRead(q, laddr, lkey, raddr, rkey, len, /*wr_id=*/0);
    int n = PollCq(q->cq, wc, 16, &bad);
    return n > 0;
  };

  while (std::chrono::steady_clock::now() < deadline) {
    // Step 1: ONE contiguous READ of the whole sentinel array.
    if (!blocking_read(&meta_qp, sentinel_local.data(), sentinel_local_mr.lkey(), remote.sentinel.addr,
                        remote.sentinel.rkey, static_cast<uint32_t>(num_brokers * sizeof(uint64_t)))) {
      fprintf(stderr, "[sequencer] sentinel-array READ failed\n"); break;
    }
    ++sentinel_polls;
    const uint64_t poll_ts = NowNsLocal();

    for (int b = 0; b < num_brokers; ++b) {
      const uint64_t sv = sentinel_local[b];
      if (sv == kPublishUncommitted || sv == last_seen[b]) continue;

      // The sentinel array holds a single overwriting FRONTIER value per broker (spec E2), not a
      // per-batch log — seeing it advance from X to Y means "everything up to Y is now visible",
      // not "read the slot at Y". We must SCAN every seq in (last_seen[b], sv] in ring order,
      // exactly like BrokerScannerWorker5 scans the PBR ring bounded by a frontier, rather than
      // jump straight to the newest slot. Jumping would silently skip every batch the broker
      // produced between two of our polls — an undetected loss bug, not a measurement artifact.
      uint64_t range_start = (last_seen[b] == kPublishUncommitted) ? sv : last_seen[b] + 1;
      // Safety cap: if we fell behind by more than the whole ring, everything older than one lap
      // back is unrecoverable (already overwritten) — bound the scan instead of redoing lost work.
      if (sv - range_start >= static_cast<uint64_t>(pbr_slots)) {
        const uint64_t unrecoverable = (sv - range_start) - static_cast<uint64_t>(pbr_slots) + 1;
        stale_misses += unrecoverable;
        range_start = sv - static_cast<uint64_t>(pbr_slots) + 1;
      }
      last_seen[b] = sv;

      for (uint64_t seq = range_start; seq <= sv; ++seq) {
        // Step 2a: full 128B header READ, torn-window-safe re-verify (own embedded sentinel).
        const uint64_t slot = seq % static_cast<uint64_t>(pbr_slots);
        const uint64_t slot_addr = remote.pbr.addr +
            static_cast<uint64_t>(b) * remote.pbr_ring_bytes_per_broker + slot * sizeof(BatchHeaderMirror);
        if (!blocking_read(&meta_qp, &header_local, header_local_mr.lkey(), slot_addr, remote.pbr.rkey,
                            sizeof(BatchHeaderMirror))) {
          fprintf(stderr, "[sequencer] header READ failed (broker %d)\n", b); continue;
        }
        // pbr_absolute_index != seq means the broker already wrapped this slot again with a
        // NEWER batch before we got here — seq's own data is genuinely gone (a real leg-2/ring-
        // contention loss, not a false alarm), not merely "not yet committed".
        if (!HeaderPublishCommitted(header_local) || header_local.pbr_absolute_index != seq) {
          ++stale_misses;
          continue;
        }

        // Step 2b: Blog payload READ — broker-direct (W5-A) or memserver (W5-B). Isolated leg-2 hop.
        const uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(header_local.total_size, kMaxPayload));
#if BLOG_PLACEMENT_DRAM
        if (!blocking_read(&blog_qps[b], payload_local.data(), payload_local_mr.lkey(),
                           blog_regions[b].addr + header_local.log_idx, blog_regions[b].rkey, len)) {
          fprintf(stderr, "[sequencer] Blog READ failed (broker %d, broker-direct)\n", b); continue;
        }
#else
        const uint64_t blog_base = remote.blog.addr + static_cast<uint64_t>(b) * remote.blog_bytes_per_broker;
        if (!blocking_read(&meta_qp, payload_local.data(), payload_local_mr.lkey(),
                           blog_base + (header_local.log_idx % remote.blog_bytes_per_broker),
                           remote.blog.rkey, len)) {
          fprintf(stderr, "[sequencer] Blog READ failed (broker %d, memserver)\n", b); continue;
        }
#endif
        blog_bytes_read += header_local.total_size;
        ++batches;

        // Step 2c: advance GOI / ControlBlock / CV (local mirror, then flush to memserver).
        ++committed_seq;
        goi_local.total_order = committed_seq;
        goi_local.batch_seq = header_local.batch_seq;
        goi_local.client_id = header_local.client_id;
        goi_local.broker_id = header_local.broker_id;
        goi_local.log_idx = header_local.log_idx;
        goi_local.total_size = header_local.total_size;
        const uint64_t goi_addr = remote.goi.addr +
            (committed_seq % static_cast<uint64_t>(remote.goi_entries)) * sizeof(GoiEntryMirror);
        PostWrite(&meta_qp, &goi_local, goi_local_mr.lkey(), goi_addr, remote.goi.rkey,
                  sizeof(GoiEntryMirror), /*wr_id=*/1);

        cv_local.ack_offset = seq + 1;
        const uint64_t cv_addr = remote.cv.addr + static_cast<uint64_t>(b) * sizeof(CompletionVectorEntryMirror);
        PostWrite(&meta_qp, &cv_local, cv_local_mr.lkey(), cv_addr, remote.cv.rkey,
                  sizeof(CompletionVectorEntryMirror), /*wr_id=*/2);

        cb_local.committed_seq = committed_seq;
        PostWrite(&meta_qp, &cb_local, cb_local_mr.lkey(), remote.control.addr, remote.control.rkey,
                  sizeof(ControlBlockMirror), /*wr_id=*/3);
        // Drain the 3 writes above (small control-plane traffic; not the funnel's bottleneck).
        int drained = 0;
        while (drained < 3) {
          int n = PollCq(meta_qp.cq, wc, 16, &bad);
          if (n < 0) { fprintf(stderr, "[sequencer] CV/GOI/CB write WC error status=%d\n", bad); break; }
          drained += n;
        }

        detect_to_commit.Add(NowNsLocal() - poll_ts);
      }
    }
  }

  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
  fprintf(stderr,
          "[sequencer] RESULT batches=%lu blog_bytes=%lu secs=%.3f throughput=%.3f Mmsg/s "
          "%.3f Gb/s sentinel_polls=%lu poll_rate=%.1f/s stale_misses=%lu "
          "detect_to_commit_p50_us=%.1f p99_us=%.1f p999_us=%.1f\n",
          (unsigned long)batches, (unsigned long)blog_bytes_read, secs, batches / secs / 1e6,
          (blog_bytes_read * 8.0) / secs / 1e9, (unsigned long)sentinel_polls,
          sentinel_polls / secs, (unsigned long)stale_misses,
          detect_to_commit.Pct(0.50) / 1e3, detect_to_commit.Pct(0.99) / 1e3,
          detect_to_commit.Pct(0.999) / 1e3);

  DeregisterMr(&sentinel_local_mr); DeregisterMr(&header_local_mr); DeregisterMr(&payload_local_mr);
  DeregisterMr(&cb_local_mr); DeregisterMr(&cv_local_mr); DeregisterMr(&goi_local_mr);
  DestroyRcQp(&meta_qp);
#if BLOG_PLACEMENT_DRAM
  for (auto& q : blog_qps) DestroyRcQp(&q);
#endif
  CloseDevice(&d);
  return 0;
}
