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
  // All per-broker Blog QPs share ONE CQ so the pipelined poller (rdma_sequencer's main loop)
  // can watch every broker's completions with a single ibv_poll_cq call — necessary because the
  // pipeline posts many outstanding reads across DIFFERENT brokers' QPs and correlates them by
  // wr_id, not by which QP they landed on.
  ibv_cq* blog_shared_cq = ibv_create_cq(d.ctx, 4096, nullptr, nullptr, 0);
  if (!blog_shared_cq) { fprintf(stderr, "[sequencer] failed to create shared Blog CQ\n"); return 1; }
  std::vector<RcQp> blog_qps(num_brokers);
  std::vector<RegionDesc> blog_regions(num_brokers);
  for (int b = 0; b < num_brokers; ++b) {
    if (!CreateRcQpOnSharedCq(&d, blog_shared_cq, 64, /*psn_seed=*/0x7000 + b, &blog_qps[b])) return 1;
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

  // [[PIPELINE]] A fully-serial one-op-at-a-time sequencer (post, block, repeat) tops out at
  // roughly 1/(ops-per-batch * RTT) ~ 10-13K batches/s on this fabric (measured: a first funnel-
  // sweep attempt showed the SEQUENCER, not the NIC, as the bottleneck even at 10% of the
  // intended offered load — 33% loss to ring-wrap because the software chain couldn't keep up).
  // Fix: pipeline header reads and Blog reads in windows (post many outstanding, poll as they
  // land), and batch CV/ControlBlock updates to once per broker per poll cycle instead of once
  // per batch (matching the real system's per-epoch-not-per-message commit cadence) — GOI still
  // gets one WRITE per batch (it is inherently a per-record log) but pipelined, not blocked on.
  constexpr int kWindow = 64;
  std::vector<BatchHeaderMirror> header_buf(kWindow);
  Mr header_buf_mr{};
  if (!RegisterMr(d.pd, header_buf.data(), header_buf.size() * sizeof(BatchHeaderMirror),
                  &header_buf_mr)) return 1;

  constexpr size_t kMaxPayload = 1 << 16;  // 64 KiB staging per window slot
  std::vector<uint8_t> payload_buf(static_cast<size_t>(kWindow) * kMaxPayload);
  Mr payload_buf_mr{};
  if (!RegisterMr(d.pd, payload_buf.data(), payload_buf.size(), &payload_buf_mr)) return 1;

  std::vector<GoiEntryMirror> goi_buf(kWindow);
  Mr goi_buf_mr{};
  if (!RegisterMr(d.pd, goi_buf.data(), goi_buf.size() * sizeof(GoiEntryMirror), &goi_buf_mr)) return 1;

  ControlBlockMirror cb_local{};
  Mr cb_local_mr{};
  if (!RegisterMr(d.pd, &cb_local, sizeof(cb_local), &cb_local_mr)) return 1;

  std::vector<CompletionVectorEntryMirror> cv_buf(num_brokers);
  Mr cv_buf_mr{};
  if (!RegisterMr(d.pd, cv_buf.data(), cv_buf.size() * sizeof(CompletionVectorEntryMirror),
                  &cv_buf_mr)) return 1;

  struct WorkItem { int broker; uint64_t seq; bool header_valid = false; uint64_t log_idx = 0;
                     uint64_t total_size = 0; uint32_t client_id = 0; uint64_t batch_seq = 0; };

  ibv_wc wc[16];
  int bad = 0;

  // Windowed pipeline: keep up to kWindow ops outstanding on `q`, posting item i's op via
  // `post_one(i)` (wr_id MUST equal i) and invoking `on_complete(i)` as each lands — standard
  // sliding-window pattern, the fix for the fully-serial post/block/repeat bottleneck above.
  auto pipelined_ops = [&](RcQp* q, size_t count, auto&& post_one, auto&& on_complete) -> bool {
    size_t posted = 0, completed = 0;
    while (completed < count) {
      while (posted < count && (posted - completed) < static_cast<size_t>(kWindow)) {
        post_one(posted);
        ++posted;
      }
      int n = PollCq(q->cq, wc, 16, &bad);
      if (n < 0) { fprintf(stderr, "[sequencer] pipelined op WC error status=%d\n", bad); return false; }
      for (int i = 0; i < n; ++i) on_complete(static_cast<size_t>(wc[i].wr_id));
      completed += static_cast<size_t>(n);
    }
    return true;
  };

  std::vector<uint64_t> last_seen(num_brokers, kPublishUncommitted);
  std::vector<uint64_t> detect_ts(num_brokers, 0);
  uint64_t committed_seq = 0;
  uint64_t batches = 0, blog_bytes_read = 0, sentinel_polls = 0, stale_misses = 0;
  LatencyStats detect_to_commit;

  const auto t_start = std::chrono::steady_clock::now();
  const auto deadline = t_start + std::chrono::seconds(duration_secs);

  auto blocking_read = [&](RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey,
                           uint32_t len) -> bool {
    PostRead(q, laddr, lkey, raddr, rkey, len, /*wr_id=*/0);
    int n = PollCq(q->cq, wc, 16, &bad);
    return n > 0;
  };

  std::vector<uint64_t> cv_dirty_since_flush(num_brokers, kPublishUncommitted);  // latest ack_offset pending a WRITE, per broker
  std::vector<uint64_t> item_detect_ts;  // parallel to worklist, for detect_to_commit sampling

  while (std::chrono::steady_clock::now() < deadline) {
    // Step 1: ONE contiguous READ of the whole sentinel array.
    if (!blocking_read(&meta_qp, sentinel_local.data(), sentinel_local_mr.lkey(), remote.sentinel.addr,
                        remote.sentinel.rkey, static_cast<uint32_t>(num_brokers * sizeof(uint64_t)))) {
      fprintf(stderr, "[sequencer] sentinel-array READ failed\n"); break;
    }
    ++sentinel_polls;
    const uint64_t poll_ts = NowNsLocal();

    // Step 2: flatten every newly-visible (broker,seq) across ALL brokers into one worklist for
    // this poll cycle — the sentinel array holds a single overwriting FRONTIER per broker (spec
    // E2), not a per-batch log, so "advanced from X to Y" means "everything up to Y is now
    // visible", not "read the slot at Y"; we scan the full range (as BrokerScannerWorker5 would),
    // never jump straight to the newest slot (that would silently skip every batch produced
    // between two polls — an undetected loss, not a measurement artifact).
    std::vector<WorkItem> work;
    for (int b = 0; b < num_brokers; ++b) {
      const uint64_t sv = sentinel_local[b];
      if (sv == kPublishUncommitted || sv == last_seen[b]) continue;
      uint64_t range_start = (last_seen[b] == kPublishUncommitted) ? sv : last_seen[b] + 1;
      if (sv - range_start >= static_cast<uint64_t>(pbr_slots)) {
        const uint64_t unrecoverable = (sv - range_start) - static_cast<uint64_t>(pbr_slots) + 1;
        stale_misses += unrecoverable;
        range_start = sv - static_cast<uint64_t>(pbr_slots) + 1;
      }
      last_seen[b] = sv;
      for (uint64_t seq = range_start; seq <= sv; ++seq) work.push_back({b, seq});
    }
    if (work.empty()) continue;

    // Step 3: PIPELINED header reads — up to kWindow outstanding, torn-window-safe re-verify on
    // each landed header using its OWN embedded sentinel (never trust the sentinel-array value
    // alone).
    pipelined_ops(
        &meta_qp, work.size(),
        [&](size_t i) {
          const WorkItem& w = work[i];
          const uint64_t slot = w.seq % static_cast<uint64_t>(pbr_slots);
          const uint64_t slot_addr = remote.pbr.addr +
              static_cast<uint64_t>(w.broker) * remote.pbr_ring_bytes_per_broker +
              slot * sizeof(BatchHeaderMirror);
          PostRead(&meta_qp, &header_buf[i % kWindow], header_buf_mr.lkey(), slot_addr,
                   remote.pbr.rkey, sizeof(BatchHeaderMirror), /*wr_id=*/i);
        },
        [&](size_t i) {
          WorkItem& w = work[i];
          const BatchHeaderMirror& h = header_buf[i % kWindow];
          // pbr_absolute_index != seq means the broker already wrapped this slot with a NEWER
          // batch before we got here — seq's own data is genuinely gone (a real leg-2/ring-
          // contention loss, not a false alarm), not merely "not yet committed".
          if (!HeaderPublishCommitted(h) || h.pbr_absolute_index != w.seq) {
            ++stale_misses;
            return;
          }
          w.header_valid = true;
          w.log_idx = h.log_idx;
          w.total_size = h.total_size;
          w.client_id = h.client_id;
          w.batch_seq = h.batch_seq;
        });

    std::vector<size_t> valid_idx;
    for (size_t i = 0; i < work.size(); ++i) if (work[i].header_valid) valid_idx.push_back(i);
    if (valid_idx.empty()) continue;

    // Step 4: PIPELINED Blog payload reads — broker-direct (W5-A, isolated leg-2 hop) or
    // memserver (W5-B). W5-A needs a per-broker QP; those QPs share ONE CQ (blog_shared_cq, set
    // up at connect time) precisely so this single poll loop sees every broker's completions.
    // IMPORTANT: on_complete only records results here — it does NOT post the GOI WRITE inline.
    // Posting a new op to meta_qp while THIS pipeline is still polling meta_qp's own CQ for READ
    // completions would let a GOI-write completion's wr_id (a different index space) show up in
    // the same poll batch and be misread as a blog-read work-item index (an out-of-bounds bug
    // caught before any real run). The GOI writes get their OWN pipelined pass below instead.
    std::vector<size_t> committed;  // indices into `work` that read their Blog payload successfully
    committed.reserve(valid_idx.size());
    pipelined_ops(
#if BLOG_PLACEMENT_DRAM
        &blog_qps[0],  // all blog_qps share one CQ (blog_shared_cq) — any element's .cq works
#else
        &meta_qp,
#endif
        valid_idx.size(),
        [&](size_t j) {
          const WorkItem& w = work[valid_idx[j]];
          const uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(w.total_size, kMaxPayload));
          uint8_t* laddr = payload_buf.data() + (j % kWindow) * kMaxPayload;
#if BLOG_PLACEMENT_DRAM
          PostRead(&blog_qps[w.broker], laddr, payload_buf_mr.lkey(),
                   blog_regions[w.broker].addr + w.log_idx, blog_regions[w.broker].rkey, len,
                   /*wr_id=*/j);
#else
          const uint64_t blog_base = remote.blog.addr +
              static_cast<uint64_t>(w.broker) * remote.blog_bytes_per_broker;
          PostRead(&meta_qp, laddr, payload_buf_mr.lkey(),
                   blog_base + (w.log_idx % remote.blog_bytes_per_broker), remote.blog.rkey, len,
                   /*wr_id=*/j);
#endif
        },
        [&](size_t j) {
          const WorkItem& w = work[valid_idx[j]];
          blog_bytes_read += w.total_size;
          ++batches;
          cv_dirty_since_flush[w.broker] = w.seq + 1;
          detect_to_commit.Add(NowNsLocal() - poll_ts);
          committed.push_back(valid_idx[j]);
        });
    if (committed.empty()) continue;

    // Step 4b: SEPARATE pipelined pass for the GOI writes (one per committed batch — GOI is
    // inherently a per-record log, unlike CV/ControlBlock below). Runs after Step 4 fully drains,
    // so this pass's wr_ids (0..committed.size()-1) never collide with the read pipeline's.
    pipelined_ops(
        &meta_qp, committed.size(),
        [&](size_t k) {
          const WorkItem& w = work[committed[k]];
          ++committed_seq;
          GoiEntryMirror& g = goi_buf[k % kWindow];
          g.total_order = committed_seq;
          g.batch_seq = w.batch_seq;
          g.client_id = w.client_id;
          g.broker_id = static_cast<uint32_t>(w.broker);
          g.log_idx = w.log_idx;
          g.total_size = w.total_size;
          const uint64_t goi_addr = remote.goi.addr +
              (committed_seq % static_cast<uint64_t>(remote.goi_entries)) * sizeof(GoiEntryMirror);
          PostWrite(&meta_qp, &g, goi_buf_mr.lkey(), goi_addr, remote.goi.rkey,
                    sizeof(GoiEntryMirror), /*wr_id=*/k);
        },
        [&](size_t /*k*/) {});

    // Step 5: CV + ControlBlock — ONCE per broker (CV) / once total (ControlBlock) per poll
    // cycle with the LATEST value, not once per batch. This matches the real system's per-epoch
    // (not per-message) commit cadence and removes 2 of the 3 per-batch writes from the hot path.
    //
    // [[BUG FOUND + FIXED]] The drain here MUST block until it actually collects the EXACT number
    // of completions posted below — an earlier version used an upper-bound estimate and bailed on
    // the first empty poll (ibv_poll_cq legitimately returns 0 right after posting; completions
    // take microseconds to land). That left real CV/CB completions sitting UNDRAINED in meta_qp's
    // CQ, where the NEXT loop iteration's naive "any completion => my just-posted sentinel-array
    // read must be done" check (blocking_read) would wrongly consume THOSE leftover completions
    // instead of the read it just posted — proceeding with a STALE/garbage sentinel_local buffer.
    // Symptom actually observed: a nonsensical ~570K sentinel-polls/sec (far above any real RTT)
    // and ~98% of a W5-A run's work items rejected as stale. Fix: an EXACT posted-count + PollCq's
    // own blocking loop (which correctly spins on n==0 rather than treating it as "done").
    int cv_posted = 0;
    for (int b = 0; b < num_brokers; ++b) {
      if (cv_dirty_since_flush[b] == kPublishUncommitted) continue;
      cv_buf[b].ack_offset = cv_dirty_since_flush[b];
      cv_dirty_since_flush[b] = kPublishUncommitted;
      const uint64_t cv_addr = remote.cv.addr + static_cast<uint64_t>(b) * sizeof(CompletionVectorEntryMirror);
      PostWrite(&meta_qp, &cv_buf[b], cv_buf_mr.lkey(), cv_addr, remote.cv.rkey,
                sizeof(CompletionVectorEntryMirror), /*wr_id=*/1);
      ++cv_posted;
    }
    cb_local.committed_seq = committed_seq;
    PostWrite(&meta_qp, &cb_local, cb_local_mr.lkey(), remote.control.addr, remote.control.rkey,
              sizeof(ControlBlockMirror), /*wr_id=*/2);
    {
      int drained = 0;
      const int expected = cv_posted + 1;  // EXACT count now, not an upper bound
      while (drained < expected) {
        int n = PollCq(meta_qp.cq, wc, 16, &bad);  // blocks/spins until >=1 completion or error
        if (n < 0) { fprintf(stderr, "[sequencer] CV/CB write WC error status=%d\n", bad); break; }
        drained += n;
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

  DeregisterMr(&sentinel_local_mr); DeregisterMr(&header_buf_mr); DeregisterMr(&payload_buf_mr);
  DeregisterMr(&cb_local_mr); DeregisterMr(&cv_buf_mr); DeregisterMr(&goi_buf_mr);
  DestroyRcQp(&meta_qp);
#if BLOG_PLACEMENT_DRAM
  for (auto& q : blog_qps) DestroyRcQp(&q);
  ibv_destroy_cq(blog_shared_cq);
#endif
  CloseDevice(&d);
  return 0;
}
