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
#include <unordered_map>
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
  std::string blog_ports_csv = GetArg(argc, argv, "--blog-ports", "");  // optional per-broker override
                                                                          // (CSV, index = broker_id) —
                                                                          // needed when >1 broker is
                                                                          // co-located on one host and
                                                                          // a single shared port would
                                                                          // conflict; falls back to
                                                                          // --blog-port for every
                                                                          // broker if not given.
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));

  fprintf(stderr, "[sequencer] placement=%s num_brokers=%d duration=%ds\n",
          BLOG_PLACEMENT_DRAM ? "DRAM(W5-A)" : "MEMSERVER(W5-B)", num_brokers, duration_secs);

  std::vector<std::string> broker_ips;
  std::vector<int> blog_ports;
#if BLOG_PLACEMENT_DRAM
  {
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
    if (blog_ports_csv.empty()) {
      blog_ports.assign(num_brokers, blog_port);
    } else {
      std::string p = blog_ports_csv;
      while (!p.empty()) {
        size_t comma = p.find(',');
        blog_ports.push_back(atoi((comma == std::string::npos ? p : p.substr(0, comma)).c_str()));
        if (comma == std::string::npos) break;
        p = p.substr(comma + 1);
      }
      if (static_cast<int>(blog_ports.size()) != num_brokers) {
        fprintf(stderr, "[sequencer] FATAL: --blog-ports must list exactly num_brokers=%d ports "
                        "(got %zu)\n", num_brokers, blog_ports.size());
        return 1;
      }
    }
  }
#endif

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;

  // [[PIPELINE]] Windowed pipeline depth, shared by every pipelined_ops() call below (header
  // reads, Blog reads, GOI writes) AND by the per-broker Blog QPs' send-queue sizing in W5-A —
  // declared here (not at its point of use further down) so the QP creation below can size
  // against it.
  constexpr int kWindow = 64;

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
    // [[BUG FOUND]] max_send_wr==kWindow left ZERO margin: the pipeline can post up to kWindow
    // outstanding reads to ONE broker's QP if a poll cycle's work happens to concentrate on that
    // broker (e.g. a burst from one broker while others are quiet). Sized with headroom so a
    // full-window burst never hits ibv_post_send's ENOMEM/queue-full path.
    if (!CreateRcQpOnSharedCq(&d, blog_shared_cq, 2 * kWindow, /*psn_seed=*/0x7000 + b, &blog_qps[b])) return 1;
    BlogHandoffBlob hlocal{}, hremote{};
    hlocal.ep = LocalEndpoint(blog_qps[b]);
    if (!OobClientExchange(broker_ips[b], blog_ports[b], &hlocal, &hremote, sizeof(BlogHandoffBlob))) {
      fprintf(stderr, "[sequencer] Blog handoff with broker %d (%s) failed\n", b, broker_ips[b].c_str());
      return 1;
    }
    if (!ConnectRcQp(&blog_qps[b], hremote.ep)) return 1;
    blog_regions[b] = hremote.blog;
    fprintf(stderr, "[sequencer] broker-direct Blog connection to broker %d (%s) qpn=%u\n",
            b, broker_ips[b].c_str(), hremote.ep.qpn);
  }
  // Maps a completion's LOCAL qp_num (blog_qps[b].qp->qp_num — the number ibv_poll_cq reports for
  // a WC on that QP) back to the broker it belongs to, so the fast-path error handler in Step 4
  // knows WHICH broker just errored out.
  std::unordered_map<uint32_t, int> qpnum_to_broker;
  for (int b = 0; b < num_brokers; ++b) qpnum_to_broker[blog_qps[b].qp->qp_num] = b;
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
  // `post_one(i)` (wr_id MUST equal i, MUST return bool) and invoking `on_complete(i)` as each
  // lands — standard sliding-window pattern, the fix for the fully-serial post/block/repeat
  // bottleneck above.
  //
  // [[BUG FOUND]] `post_one`'s return value used to be discarded and `posted` incremented
  // unconditionally. If ibv_post_send ever fails (queue full, or — the case that matters for
  // Phase 2 — the peer is dead and the QP has entered an error state), that op is silently never
  // sent, yet `posted` advances as if it had been; `completed` then can never reach `count` for
  // that phantom op, so `while (completed < count)` spins forever. Fixed: check the return value
  // and abort the whole pipelined_ops call immediately on a post failure instead of pretending it
  // succeeded. A post/poll failure on a per-broker Blog QP (W5-A) is exactly the fast-path
  // `kPeerDown` signal Phase 2's failure-detection race depends on — silently hanging on it would
  // have made that measurement impossible, not just slow.
  // [[BUG FOUND]] The original fast-path plan was "a failed ibv_post_send on a dead peer's QP is
  // kPeerDown" — WRONG. post_send() only enqueues a WR on the LOCAL send queue; it does not touch
  // the network and succeeds regardless of whether the peer is alive. A dead peer only surfaces as
  // a BAD COMPLETION STATUS later (RC retry/timeout exhaustion -> IBV_WC_RETRY_EXC_ERR, or
  // IBV_WC_WR_FLUSH_ERR for anything queued after the QP already entered the error state) —
  // discovered via POLLING, not posting. Verified by a kill sanity test: broker0 was killed, but
  // `!post_one(...)` never fired even once; detection only happened via the 500ms lease backstop.
  // Fixed: pipelined_ops now polls inline (instead of delegating to the opaque PollCq helper) so it
  // can extract the erroring completion's qp_num and hand it to an optional `on_error` hook —
  // that's the real fast path. Good completions retrieved in the SAME poll batch as a bad one are
  // still delivered to on_complete before reporting the error (ibv_poll_cq can return a mixed
  // batch; only the bad entry and anything after it in that batch is lost, to be picked up by the
  // caller's retry logic).
  auto pipelined_ops = [&](RcQp* q, size_t count, auto&& post_one, auto&& on_complete,
                           auto&& on_error) -> bool {
    size_t posted = 0, completed = 0;
    while (completed < count) {
      while (posted < count && (posted - completed) < static_cast<size_t>(kWindow)) {
        if (!post_one(posted)) {
          fprintf(stderr, "[sequencer] pipelined op post failed at index %zu\n", posted);
          return false;
        }
        ++posted;
      }
      int n = ibv_poll_cq(q->cq, 16, wc);
      if (n < 0) { fprintf(stderr, "[sequencer] pipelined op poll_cq error\n"); return false; }
      if (n == 0) continue;
      bool had_error = false;
      for (int i = 0; i < n; ++i) {
        if (wc[i].status != IBV_WC_SUCCESS) {
          fprintf(stderr,
                  "[sequencer] pipelined op WC error status=%d(%s) qp_num=%u wr_id=%llu\n",
                  wc[i].status, ibv_wc_status_str(wc[i].status), wc[i].qp_num,
                  static_cast<unsigned long long>(wc[i].wr_id));
          on_error(wc[i].qp_num);
          had_error = true;
          break;  // stop processing this batch; anything after (incl. this entry) is lost, the
                  // caller's retry logic picks up whatever didn't complete
        }
        on_complete(static_cast<size_t>(wc[i].wr_id));
        ++completed;
      }
      if (had_error) return false;
    }
    return true;
  };

  std::vector<uint64_t> last_seen(num_brokers, kPublishUncommitted);
  uint64_t committed_seq = 0;
  uint64_t batches = 0, blog_bytes_read = 0, sentinel_polls = 0, stale_misses = 0;
  uint64_t unrecoverable_bytes = 0;
  LatencyStats detect_to_commit;

  const auto t_start = std::chrono::steady_clock::now();
  const auto deadline = t_start + std::chrono::seconds(duration_secs);

  // ---- W5-A leg-1 failure detection: FAST path (RDMA op to a dead broker errors, kPeerDown)
  // vs BACKSTOP path (lease timeout) — Phase 2 measures which fires first. `last_alive_ts[b]` is
  // the "lease": renewed implicitly by every successful Blog read from broker b (there is no
  // separate heartbeat message in this harness — the Blog read traffic itself IS the liveness
  // signal, matching the spec's "single-writer ownership... lease only for cold-path membership").
  // Per the spec's M2 guidance, the RDMA lease timeout must NOT reuse CXL's tight constants —
  // inflated here to cover RDMA RTT tail + polling-cycle granularity, not CXL's sub-microsecond
  // coherence latency.
  constexpr uint64_t kLeaseTimeoutNs = 500'000'000ull;  // 500ms backstop; RDMA RTT here is ~us-scale
  std::vector<bool> broker_dead(num_brokers, false);
  std::vector<uint64_t> broker_dead_detect_ts(num_brokers, 0);
  std::vector<std::string> broker_dead_via(num_brokers, "");
  std::vector<uint64_t> last_alive_ts(num_brokers, NowNsLocal());

  auto blocking_read = [&](RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey,
                           uint32_t len) -> bool {
    if (!PostRead(q, laddr, lkey, raddr, rkey, len, /*wr_id=*/0)) return false;
    int n = PollCq(q->cq, wc, 16, &bad);
    return n > 0;
  };

  std::vector<uint64_t> cv_dirty_since_flush(num_brokers, kPublishUncommitted);  // latest ack_offset pending a WRITE, per broker

  while (std::chrono::steady_clock::now() < deadline) {
    // Step 1: ONE contiguous READ of the whole sentinel array.
    if (!blocking_read(&meta_qp, sentinel_local.data(), sentinel_local_mr.lkey(), remote.sentinel.addr,
                        remote.sentinel.rkey, static_cast<uint32_t>(num_brokers * sizeof(uint64_t)))) {
      fprintf(stderr, "[sequencer] sentinel-array READ failed\n"); break;
    }
    ++sentinel_polls;
    const uint64_t poll_ts = NowNsLocal();

#if BLOG_PLACEMENT_DRAM
    // Lease-timeout BACKSTOP check (Phase 2 §7 "failure detection via BOTH paths"): if a broker
    // hasn't produced a successful Blog read in kLeaseTimeoutNs and the FAST path (RDMA op error,
    // checked in Step 4 below) hasn't already caught it, declare it dead here. Checked every poll
    // cycle so the backstop's own detection latency is bounded by the poll rate, not the run
    // duration.
    for (int b = 0; b < num_brokers; ++b) {
      if (broker_dead[b]) continue;
      if (poll_ts - last_alive_ts[b] > kLeaseTimeoutNs) {
        broker_dead[b] = true;
        broker_dead_detect_ts[b] = poll_ts;
        broker_dead_via[b] = "lease_timeout";
        fprintf(stderr, "[sequencer] broker %d declared dead via LEASE_TIMEOUT at t=%.3fs\n", b,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count());
      }
    }
#endif

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
          return PostRead(&meta_qp, &header_buf[i % kWindow], header_buf_mr.lkey(), slot_addr,
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
        },
        [](uint32_t) {});  // no per-broker QP here — meta_qp targets c3, never killed in Phase 2

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
    //
    // W5-A dead-broker handling: pipelined_ops aborts the WHOLE call on the first post failure
    // (see the fix above — no more silent-hang-on-failed-post), which would otherwise starve
    // OTHER, still-alive brokers' items queued in the SAME window behind a dead broker's item.
    // Retry loop: after an abort, drop items whose broker just got marked dead (their payload is
    // now permanently unrecoverable — counted below) and re-run the pipeline on whatever's left,
    // bounded at num_brokers+1 attempts (at most one NEW broker can die per attempt).
    std::vector<size_t> committed;  // indices into `work` that read their Blog payload successfully
    committed.reserve(valid_idx.size());
    std::vector<size_t> pending = valid_idx;
    for (int attempt = 0; attempt <= num_brokers && !pending.empty(); ++attempt) {
      const size_t committed_before = committed.size();
      bool ok = pipelined_ops(
#if BLOG_PLACEMENT_DRAM
          &blog_qps[0],  // all blog_qps share one CQ (blog_shared_cq) — any element's .cq works
#else
          &meta_qp,
#endif
          pending.size(),
          [&](size_t j) -> bool {
            const WorkItem& w = work[pending[j]];
            const uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(w.total_size, kMaxPayload));
            uint8_t* laddr = payload_buf.data() + (j % kWindow) * kMaxPayload;
#if BLOG_PLACEMENT_DRAM
            if (broker_dead[w.broker]) return false;
            bool posted = PostRead(&blog_qps[w.broker], laddr, payload_buf_mr.lkey(),
                                   blog_regions[w.broker].addr + w.log_idx, blog_regions[w.broker].rkey,
                                   len, /*wr_id=*/j);
            if (!posted && !broker_dead[w.broker]) {
              // Defensive fallback only — a LOCAL post_send() failure (e.g. this QP already in
              // the error state from an earlier bad completion THIS window, so ibv_post_send
              // itself rejects it) can still land here. The REAL fast path for a dead PEER is the
              // on_error(qp_num) hook below, fired from a bad completion status, not from post
              // failing (post_send succeeds locally regardless of whether the peer is alive).
              broker_dead[w.broker] = true;
              broker_dead_detect_ts[w.broker] = NowNsLocal();
              broker_dead_via[w.broker] = "rdma_error";
              fprintf(stderr, "[sequencer] broker %d declared dead via RDMA_ERROR (kPeerDown) at t=%.3fs\n",
                      w.broker, std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count());
            }
            return posted;
#else
            const uint64_t blog_base = remote.blog.addr +
                static_cast<uint64_t>(w.broker) * remote.blog_bytes_per_broker;
            return PostRead(&meta_qp, laddr, payload_buf_mr.lkey(),
                            blog_base + (w.log_idx % remote.blog_bytes_per_broker), remote.blog.rkey, len,
                            /*wr_id=*/j);
#endif
          },
          [&](size_t j) {
            const WorkItem& w = work[pending[j]];
            blog_bytes_read += w.total_size;
            ++batches;
#if BLOG_PLACEMENT_DRAM
            last_alive_ts[w.broker] = NowNsLocal();  // renew the lease — this read IS the heartbeat
#endif
            // [[BUG FOUND]] was `cv_dirty_since_flush[w.broker] = w.seq + 1;` (last-writer wins).
            // Harmless under normal single-QP-per-broker completion ordering, but not defensively
            // correct once a retry pass (above) can re-attempt items out of their original window
            // order — take the max so a late-arriving lower seq can never regress a broker's
            // already-recorded ack_offset.
            if (cv_dirty_since_flush[w.broker] == kPublishUncommitted ||
                w.seq + 1 > cv_dirty_since_flush[w.broker]) {
              cv_dirty_since_flush[w.broker] = w.seq + 1;
            }
            committed.push_back(pending[j]);
          }
#if BLOG_PLACEMENT_DRAM
          ,
          [&](uint32_t qp_num) {
            // THE actual fast path: a completion for this QP came back with a bad status (RC
            // retry/timeout exhaustion against a dead peer, or a flush error for anything queued
            // after the QP already entered the error state). Map qp_num back to broker_id and
            // declare it dead here — this is what kPeerDown detection really looks like, not a
            // failed post_send (see the bug note on pipelined_ops above).
            auto it = qpnum_to_broker.find(qp_num);
            if (it == qpnum_to_broker.end()) return;
            int b = it->second;
            if (broker_dead[b]) return;
            broker_dead[b] = true;
            broker_dead_detect_ts[b] = NowNsLocal();
            broker_dead_via[b] = "rdma_error";
            fprintf(stderr, "[sequencer] broker %d declared dead via RDMA_ERROR (kPeerDown) at t=%.3fs\n",
                    b, std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count());
          }
#else
          ,
          [](uint32_t) {}  // meta_qp targets c3, never killed in Phase 2
#endif
          );
      if (ok) { pending.clear(); break; }
      std::vector<size_t> committed_this_attempt(committed.begin() + committed_before, committed.end());
      std::vector<size_t> next_pending;
      next_pending.reserve(pending.size());
      for (size_t idx : pending) {
        if (std::find(committed_this_attempt.begin(), committed_this_attempt.end(), idx) !=
            committed_this_attempt.end()) {
          continue;  // completed this attempt
        }
        const WorkItem& w = work[idx];
        if (broker_dead[w.broker]) {
          unrecoverable_bytes += w.total_size;  // that broker's DRAM Blog is gone; this batch's
                                                 // payload is lost forever (header was committed,
                                                 // sentinel proves it, but the bytes are unreachable)
          continue;
        }
        next_pending.push_back(idx);  // still-alive broker, just not reached yet this attempt
      }
      pending = std::move(next_pending);
    }
    for (size_t idx : pending) unrecoverable_bytes += work[idx].total_size;  // exhausted retries
    if (committed.empty()) continue;

    // [[BUG FOUND]] `committed` is populated in on_complete's ARRIVAL order, which is completion
    // order, not post order — for W5-A those are the SAME per-broker (RC preserves per-QP
    // completion order) but can DIFFER ACROSS brokers sharing one CQ (a fast/local broker's read
    // can complete before a slower/farther broker's read that was posted earlier), and the retry
    // loop above appends recovered items even later. GOI's total_order must reflect a canonical,
    // reproducible order, not real-time network arrival — sort back into `work`-index order
    // (== valid_idx's original broker-then-seq order) before assigning committed_seq below.
    std::sort(committed.begin(), committed.end());

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
          return PostWrite(&meta_qp, &g, goi_buf_mr.lkey(), goi_addr, remote.goi.rkey,
                           sizeof(GoiEntryMirror), /*wr_id=*/k);
        },
        [&](size_t /*k*/) {},
        [](uint32_t) {});  // meta_qp targets c3, never killed in Phase 2

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

    // [[BUG FOUND]] `detect_to_commit` used to be sampled at Step 4's blog-read completion, before
    // the GOI write (Step 4b) and the CV/ControlBlock write (Step 5) even happen — understating
    // true detect-to-COMMIT latency by excluding the two writes that actually make a batch visible
    // to a real consumer. Sample here, after the CV/CB write is confirmed landed, for every item
    // this poll cycle actually committed (all share this poll's poll_ts and this now-confirmed
    // commit instant — coarser than per-item, but no longer systematically too fast).
    {
      const uint64_t commit_ts = NowNsLocal();
      for (size_t i = 0; i < committed.size(); ++i) detect_to_commit.Add(commit_ts - poll_ts);
    }
  }

  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
  fprintf(stderr,
          "[sequencer] RESULT batches=%lu blog_bytes=%lu secs=%.3f throughput=%.3f Mmsg/s "
          "%.3f Gb/s sentinel_polls=%lu poll_rate=%.1f/s stale_misses=%lu unrecoverable_bytes=%lu "
          "detect_to_commit_p50_us=%.1f p99_us=%.1f p999_us=%.1f\n",
          (unsigned long)batches, (unsigned long)blog_bytes_read, secs, batches / secs / 1e6,
          (blog_bytes_read * 8.0) / secs / 1e9, (unsigned long)sentinel_polls,
          sentinel_polls / secs, (unsigned long)stale_misses, (unsigned long)unrecoverable_bytes,
          detect_to_commit.Pct(0.50) / 1e3, detect_to_commit.Pct(0.99) / 1e3,
          detect_to_commit.Pct(0.999) / 1e3);
#if BLOG_PLACEMENT_DRAM
  for (int b = 0; b < num_brokers; ++b) {
    if (!broker_dead[b]) continue;
    fprintf(stderr,
            "[sequencer] BROKER_DEAD broker=%d via=%s detect_t=%.3fs\n", b,
            broker_dead_via[b].c_str(),
            (broker_dead_detect_ts[b] - static_cast<uint64_t>(
                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                     t_start.time_since_epoch()).count())) / 1e9);
  }
#endif

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
