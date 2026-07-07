// rdma_transport/rdma_broker.cc — W5 broker/producer role. ONE translation unit, TWO targets
// (spec §4, Decision 1): `-DBLOG_PLACEMENT_DRAM=1` builds the W5-A broker (payload Blog in this
// process's own DRAM, no network for the payload write); `-DBLOG_PLACEMENT_DRAM=0` builds the
// W5-B broker (payload Blog RDMA-WRITEd to the memserver). Everything else — control-plane
// connection to the memserver, the C2 producer WRITE sequence, metadata placement — is identical
// between the two; only the Blog write's endpoint differs (the isolated leg-2 variable).
//
// Producer sequence on ONE RC QP to the memserver (spec §3.3, C2), per batch:
//   1. Blog payload write, 64B-aligned.
//        BLOG_PLACEMENT_DRAM=1: local CPU store into this process's own registered Blog MR.
//        BLOG_PLACEMENT_DRAM=0: RDMA WRITE into the memserver's Blog MR slice for this broker.
//   2. ONE contiguous RDMA WRITE of the BatchHeader body [0,80) to this broker's PBR slot on the
//      memserver (includes batch_complete=1, pbr_absolute_index, log_idx; leaves [80,96) alone).
//   3. A SEPARATE, later RDMA WRITE of publish_commit@96 (8B) to the SAME PBR slot, plus a WRITE
//      of this broker's sentinel-array slot (E2) — both posted after step 2, same QP. NO
//      IBV_SEND_FENCE (E3) — RC same-QP ordering already orders these WRITEs relative to step 2.
//
// In BLOG_PLACEMENT_DRAM=1 mode this process ALSO runs a tiny OOB+RC listener so the sequencer can
// connect DIRECTLY (bypassing the memserver) to RDMA-READ this broker's Blog — the broker-direct
// path that is the whole point of W5-A (each broker's own NIC serves its own payload).

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "rdma_transport/rdma_common.h"
#include "rdma_transport/rdma_wire.h"

namespace { inline void CpuPause() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
} }  // namespace

using namespace embarcadero::rdma;
using namespace embarcadero::rdma_variant;

#ifndef BLOG_PLACEMENT_DRAM
#define BLOG_PLACEMENT_DRAM 0  // default to W5-B if not specified at compile time
#endif

namespace {

const char* GetArg(int argc, char** argv, const char* key, const char* def) {
  size_t klen = strlen(key);
  for (int i = 1; i < argc; ++i)
    if (!strncmp(argv[i], key, klen) && argv[i][klen] == '=') return argv[i] + klen + 1;
  return def;
}

void* AllocHugeOrFallback(size_t bytes) {
  size_t aligned = (bytes + (1ull << 21) - 1) & ~((1ull << 21) - 1);
  void* p = mmap(nullptr, aligned, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (p != MAP_FAILED) return p;
  fprintf(stderr, "[broker] MAP_HUGETLB failed (%s), falling back\n", strerror(errno));
  p = mmap(nullptr, aligned, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { perror("mmap fallback"); exit(1); }
  return p;
}

struct LatencyStats {
  std::vector<uint64_t> samples_ns;
  void Add(uint64_t ns) { samples_ns.push_back(ns); }
  // Percentile over the collected round-trip samples (write-post -> CQ completion).
  uint64_t Pct(double p) {
    if (samples_ns.empty()) return 0;
    std::vector<uint64_t> s = samples_ns;
    std::sort(s.begin(), s.end());
    size_t idx = static_cast<size_t>(p * (s.size() - 1));
    return s[idx];
  }
};

}  // namespace

int main(int argc, char** argv) {
  int broker_id = atoi(GetArg(argc, argv, "--broker-id", "0"));
  std::string memserver_ip = GetArg(argc, argv, "--memserver-ip", "");
  int memserver_port = atoi(GetArg(argc, argv, "--memserver-port", "18600"));
  int pbr_slots = atoi(GetArg(argc, argv, "--pbr-slots", "4096"));
  uint32_t message_size = atoi(GetArg(argc, argv, "--message-size", "1024"));
  int target_mbps = atoi(GetArg(argc, argv, "--target-mbps", "0"));  // 0 = unthrottled (max rate)
  int duration_secs = atoi(GetArg(argc, argv, "--duration", "10"));
  int inflight_cap = atoi(GetArg(argc, argv, "--inflight", "32"));
  int blog_port = atoi(GetArg(argc, argv, "--blog-port", "18700"));  // DRAM mode only
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));
  // Phase 2 (W5-A leg-1) RF>=2 ring replication: this broker's OWN replica-of-left-neighbor
  // acceptor port, and (if set) the right neighbor to replicate TO. Ring topology (broker i ->
  // broker (i+1 mod N)) is wired up by the orchestration script passing explicit IPs/ports, not
  // computed from broker_id here, so the binary doesn't need to know the full topology.
  int replica_port = atoi(GetArg(argc, argv, "--replica-port", "18710"));
  std::string right_neighbor_ip = GetArg(argc, argv, "--right-neighbor-ip", "");
  int right_neighbor_replica_port = atoi(GetArg(argc, argv, "--right-neighbor-replica-port", "18710"));
  // Normally 1 (just our left neighbor's ongoing replication feed). The broker designated as the
  // post-kill recovery SOURCE for a given trial needs 2: the left-neighbor feed plus the recovery
  // tool's later read-only connection.
  int replica_max_accepts = atoi(GetArg(argc, argv, "--replica-max-accepts", "1"));

  fprintf(stderr, "[broker %d] placement=%s message_size=%u target_mbps=%d duration=%ds inflight=%d\n",
          broker_id, BLOG_PLACEMENT_DRAM ? "DRAM(W5-A)" : "MEMSERVER(W5-B)", message_size,
          target_mbps, duration_secs, inflight_cap);

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;

  // ---- Blog allocation: local (W5-A) or none-yet-known-size-locally (W5-B writes remote) ----
  constexpr size_t kBlogBytesPerBroker = 1ull << 30;  // 1 GiB circular Blog per broker
  void* local_blog_mem = nullptr;
  Mr local_blog_mr{};
#if BLOG_PLACEMENT_DRAM
  local_blog_mem = AllocHugeOrFallback(kBlogBytesPerBroker);
  memset(local_blog_mem, 0, 1 << 20);  // touch first page; payload content doesn't need full zero
  if (!RegisterMr(d.pd, local_blog_mem, kBlogBytesPerBroker, &local_blog_mr)) return 1;
  fprintf(stderr, "[broker %d] local Blog registered: %zu bytes\n", broker_id, kBlogBytesPerBroker);

  // ---- Phase 2 RF>=2: replica-of-left-neighbor region (same size as our own Blog, so it can
  // hold a full 1:1 mirror of whichever broker replicates into us) ----
  void* replica_mem = AllocHugeOrFallback(kBlogBytesPerBroker);
  memset(replica_mem, 0, 1 << 20);
  Mr replica_mr{};
  if (!RegisterMr(d.pd, replica_mem, kBlogBytesPerBroker, &replica_mr)) return 1;
#endif

  // ---- Connect to the memserver (control plane; also the Blog target in W5-B) ----
  RcQp meta_qp{};
  if (!CreateRcQp(&d, 4096, 2048, /*psn_seed=*/0x4000 + broker_id, &meta_qp)) return 1;
  HandshakeBlob local{}, remote{};
  local.role = 0;
  local.broker_id = broker_id;
  local.client_ep = LocalEndpoint(meta_qp);
  if (!OobClientExchange(memserver_ip, memserver_port, &local, &remote, sizeof(HandshakeBlob))) {
    fprintf(stderr, "[broker %d] OOB exchange with memserver failed\n", broker_id); return 1;
  }
  if (!ConnectRcQp(&meta_qp, remote.server_ep)) return 1;
  fprintf(stderr, "[broker %d] connected to memserver qpn=%u host_blog=%u\n", broker_id,
          remote.server_ep.qpn, remote.host_blog);

#if !BLOG_PLACEMENT_DRAM
  if (!remote.host_blog) {
    fprintf(stderr, "[broker %d] FATAL: built for W5-B (memserver Blog) but memserver was not "
                    "started with --host-blog\n", broker_id);
    return 1;
  }
#endif

  const uint32_t pbr_ring_bytes_per_broker = remote.pbr_ring_bytes_per_broker;
  const uint64_t my_pbr_base = remote.pbr.addr + static_cast<uint64_t>(broker_id) * pbr_ring_bytes_per_broker;
  const uint32_t my_pbr_rkey = remote.pbr.rkey;
  const uint64_t sentinel_slot_addr = remote.sentinel.addr + static_cast<uint64_t>(broker_id) * sizeof(SentinelSlot);
  const uint32_t sentinel_rkey = remote.sentinel.rkey;
#if !BLOG_PLACEMENT_DRAM
  const uint64_t remote_blog_base = remote.blog.addr + static_cast<uint64_t>(broker_id) * remote.blog_bytes_per_broker;
  const uint32_t remote_blog_rkey = remote.blog.rkey;
  const uint64_t remote_blog_bytes = remote.blog_bytes_per_broker;
#endif

#if BLOG_PLACEMENT_DRAM
  // ---- W5-A only: accept the sequencer's DIRECT connection for broker-hosted Blog reads ----
  std::thread blog_server_thread([&]() {
    BlogHandoffBlob hlocal{}, hremote{};
    RcQp blog_qp{};
    if (!CreateRcQp(&d, 64, 64, /*psn_seed=*/0x5000 + broker_id, &blog_qp)) {
      fprintf(stderr, "[broker %d] blog-server CreateRcQp failed\n", broker_id); return;
    }
    hlocal.ep = LocalEndpoint(blog_qp);
    hlocal.blog = {reinterpret_cast<uint64_t>(local_blog_mr.addr), local_blog_mr.rkey(),
                   static_cast<uint32_t>(local_blog_mr.len)};
    if (!OobServerExchange(blog_port, &hlocal, &hremote, sizeof(BlogHandoffBlob))) {
      fprintf(stderr, "[broker %d] blog-server OOB exchange failed (sequencer not connecting?)\n",
              broker_id);
      return;
    }
    if (!ConnectRcQp(&blog_qp, hremote.ep)) {
      fprintf(stderr, "[broker %d] blog-server ConnectRcQp failed\n", broker_id); return;
    }
    fprintf(stderr, "[broker %d] sequencer connected direct for Blog reads (qpn=%u)\n",
            broker_id, hremote.ep.qpn);
    // Keep the QP alive for the run's duration; the sequencer drives all reads one-sided.
    std::this_thread::sleep_for(std::chrono::seconds(duration_secs + 5));
    DestroyRcQp(&blog_qp);
  });

  // ---- Phase 2 RF>=2: accept our LEFT neighbor's replication connection (they WRITE into our
  // replica_mem) — one accept, separate port/listener from the sequencer's blog handoff above so
  // the two concerns (payload reads, peer replication) don't share failure modes. ----
  std::thread replica_acceptor_thread([&, replica_max_accepts]() {
    // Accepts UP TO replica_max_accepts connections over the run's lifetime, not just one: the
    // FIRST is normally our left neighbor's ongoing replication feed (role=1); on whichever broker
    // the orchestration script designates as the post-kill recovery SOURCE, a SECOND, later
    // connection from the recovery tool (role=2, read-only) needs to land too — both get the SAME
    // replica region descriptor (concurrent RDMA READs of one region are safe; only the recovery
    // tool ever reads it, the left neighbor only ever writes it).
    int rfd = OobServerListen(replica_port, replica_max_accepts);
    if (rfd < 0) { fprintf(stderr, "[broker %d] replica-acceptor listen failed\n", broker_id); return; }
    std::vector<RcQp> rqps;
    for (int i = 0; i < replica_max_accepts; ++i) {
      RcQp rqp{};
      if (!CreateRcQp(&d, 4096, 2048, /*psn_seed=*/0x5500 + broker_id * 8 + i, &rqp)) {
        fprintf(stderr, "[broker %d] replica-acceptor CreateRcQp failed\n", broker_id); break;
      }
      ReplicaHandoffBlob hlocal{}, hremote{};
      hlocal.role = 1;
      hlocal.ep = LocalEndpoint(rqp);
      hlocal.region = {reinterpret_cast<uint64_t>(replica_mr.addr), replica_mr.rkey(),
                       static_cast<uint32_t>(replica_mr.len)};
      if (!OobServerAccept(rfd, &hlocal, &hremote, sizeof(ReplicaHandoffBlob))) {
        fprintf(stderr, "[broker %d] replica-acceptor: accept %d/%d timed out (RF=1 or no "
                        "recovery this run)\n", broker_id, i + 1, replica_max_accepts);
        break;
      }
      if (!ConnectRcQp(&rqp, hremote.ep)) {
        fprintf(stderr, "[broker %d] replica-acceptor ConnectRcQp failed\n", broker_id); break;
      }
      fprintf(stderr, "[broker %d] replica-acceptor: connection %d/%d accepted role=%d (qpn=%u)\n",
              broker_id, i + 1, replica_max_accepts, hremote.role, hremote.ep.qpn);
      rqps.push_back(rqp);
    }
    OobServerClose(rfd);
    std::this_thread::sleep_for(std::chrono::seconds(duration_secs + 15));  // outlive recovery window
    for (auto& q : rqps) DestroyRcQp(&q);
  });

  // ---- Phase 2 RF>=2: connect to our RIGHT neighbor as a replication CLIENT (we WRITE into
  // their replica region after every local store) ----
  RcQp replica_client_qp{};
  RegionDesc right_replica_region{};
  bool has_right_neighbor = !right_neighbor_ip.empty();
  if (has_right_neighbor) {
    if (!CreateRcQp(&d, 4096, 2048, /*psn_seed=*/0x5600 + broker_id, &replica_client_qp)) return 1;
    ReplicaHandoffBlob hlocal{}, hremote{};
    hlocal.role = 1;
    hlocal.ep = LocalEndpoint(replica_client_qp);
    if (!OobClientExchange(right_neighbor_ip, right_neighbor_replica_port, &hlocal, &hremote,
                           sizeof(ReplicaHandoffBlob))) {
      fprintf(stderr, "[broker %d] replication handoff with right neighbor (%s) failed\n",
              broker_id, right_neighbor_ip.c_str());
      return 1;
    }
    if (!ConnectRcQp(&replica_client_qp, hremote.ep)) return 1;
    right_replica_region = hremote.region;
    fprintf(stderr, "[broker %d] replicating to right neighbor %s (qpn=%u)\n", broker_id,
            right_neighbor_ip.c_str(), hremote.ep.qpn);
  }
#endif

  // ---- Producer loop: fixed-size "batches" of message_size bytes, PBR ring of pbr_slots ----
  const size_t blog_capacity_slots = static_cast<size_t>(pbr_slots);  // Blog ring sized 1:1 with PBR ring
  const size_t per_slot_blog_bytes = ((message_size + 63) / 64) * 64;  // 64B-aligned (spec invariant)
#if BLOG_PLACEMENT_DRAM
  std::vector<uint8_t> stage;  // unused in DRAM mode (we write directly into local_blog_mem)
#else
  std::vector<uint8_t> stage(per_slot_blog_bytes, 0xAB);  // local staging buffer for remote Blog WRITE
  Mr stage_mr{};
  if (!RegisterMr(d.pd, stage.data(), stage.size(), &stage_mr)) return 1;
#endif

  ibv_wc wc[64];
  int bad = 0;
  uint64_t posted_wrs = 0, completed_wrs = 0;
  uint64_t batches = 0, bytes_posted = 0;
  LatencyStats lat;
  // Per-outstanding-batch post timestamp, indexed by a ring of wr_id -> ts (bounded by inflight_cap).
  std::vector<std::pair<uint64_t, uint64_t>> pending_ts;  // (wr_id of the batch's LAST wr, post_ts)
  pending_ts.reserve(inflight_cap + 8);

  const double bytes_per_sec_target = target_mbps > 0 ? target_mbps * 1e6 : 0.0;
  const auto t_start = std::chrono::steady_clock::now();
  const auto deadline = t_start + std::chrono::seconds(duration_secs);
  uint64_t wr_id_ctr = 0;
#if BLOG_PLACEMENT_DRAM
  uint64_t replica_wr_id_ctr = 0, replica_completed = 0;
#endif

  auto now_ns = [] {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  };

  while (std::chrono::steady_clock::now() < deadline) {
    // Pacing for the funnel sweep's offered-load axis (open-loop-ish: skip posting if ahead of target).
    if (bytes_per_sec_target > 0) {
      double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
      double budget_bytes = elapsed * bytes_per_sec_target;
      if (static_cast<double>(bytes_posted) > budget_bytes) {
        CpuPause();
        continue;
      }
    }
    // Backpressure: drain completions before exceeding inflight_cap outstanding WRs.
    while (static_cast<int>(posted_wrs - completed_wrs) > inflight_cap - 4) {
      int n = PollCq(meta_qp.cq, wc, 64, &bad);
      if (n < 0) { fprintf(stderr, "[broker %d] WC error status=%d\n", broker_id, bad); return 1; }
      completed_wrs += n;
      uint64_t t_now = now_ns();
      for (int i = 0; i < n; ++i) {
        for (auto& pr : pending_ts) {
          if (pr.first == wc[i].wr_id) { lat.Add(t_now - pr.second); break; }
        }
      }
    }

    const uint64_t global_seq = batches;  // monotonic; doubles as pbr_absolute_index
    const size_t slot = global_seq % blog_capacity_slots;
    const uint64_t log_idx = slot * per_slot_blog_bytes;
    const uint64_t post_ts = now_ns();

    // Step 1: Blog payload write.
#if BLOG_PLACEMENT_DRAM
    memset(static_cast<uint8_t*>(local_blog_mem) + log_idx, static_cast<uint8_t>(global_seq & 0xFF),
           per_slot_blog_bytes);  // local CPU store, no network (leg-2 held)
    // Step 1b (Phase 2 RF>=2): replicate the SAME bytes to our right neighbor at the SAME log_idx
    // offset (1:1 mirrored layout) — best-effort, fire-and-mostly-forget: opportunistically drain
    // completions (non-blocking) rather than adding this QP to the main backpressure loop, since
    // its only job is keeping a redundant copy current, not gating the producer's own throughput.
    if (has_right_neighbor) {
      PostWrite(&replica_client_qp, static_cast<uint8_t*>(local_blog_mem) + log_idx,
                local_blog_mr.lkey(), right_replica_region.addr + log_idx, right_replica_region.rkey,
                static_cast<uint32_t>(per_slot_blog_bytes), /*wr_id=*/replica_wr_id_ctr++);
      ibv_wc replica_wc[16];
      int rn = ibv_poll_cq(replica_client_qp.cq, 16, replica_wc);
      if (rn > 0) replica_completed += rn;
      else if (rn < 0) fprintf(stderr, "[broker %d] replica QP poll error\n", broker_id);
    }
#else
    PostWrite(&meta_qp, stage.data(), stage_mr.lkey(), remote_blog_base + (log_idx % remote_blog_bytes),
              remote_blog_rkey, static_cast<uint32_t>(per_slot_blog_bytes), wr_id_ctr++);
    ++posted_wrs;
#endif

    // Step 2: header body [0,80) — one contiguous WRITE.
    BatchHeaderMirror hdr{};
    hdr.batch_seq = global_seq;
    hdr.client_id = 0;
    hdr.broker_id = static_cast<uint32_t>(broker_id);
    hdr.batch_complete = 1;
    hdr.pbr_absolute_index = global_seq;
    hdr.total_size = per_slot_blog_bytes;
    hdr.start_logical_offset = global_seq;
    hdr.log_idx = log_idx;
    hdr.publish_commit = kPublishUncommitted;  // body write must NOT set the sentinel
    static BatchHeaderMirror header_stage;
    header_stage = hdr;
    static Mr header_mr_cache{};
    static bool header_mr_registered = false;
    if (!header_mr_registered) {
      RegisterMr(d.pd, &header_stage, sizeof(header_stage), &header_mr_cache);
      header_mr_registered = true;
    }
    const uint64_t slot_addr = my_pbr_base + slot * sizeof(BatchHeaderMirror);
    PostWrite(&meta_qp, &header_stage, header_mr_cache.lkey(), slot_addr, my_pbr_rkey,
              /*len=*/80, wr_id_ctr++);
    ++posted_wrs;

    // Step 3: SEPARATE sentinel writes, after step 2, same QP, no fence (RC ordering suffices).
    static uint64_t sentinel_stage;
    sentinel_stage = global_seq;  // == pbr_absolute_index -> HeaderPublishCommitted() true
    static Mr sentinel_mr_cache{};
    static bool sentinel_mr_registered = false;
    if (!sentinel_mr_registered) {
      RegisterMr(d.pd, &sentinel_stage, sizeof(sentinel_stage), &sentinel_mr_cache);
      sentinel_mr_registered = true;
    }
    PostWrite(&meta_qp, &sentinel_stage, sentinel_mr_cache.lkey(),
              slot_addr + offsetof(BatchHeaderMirror, publish_commit), my_pbr_rkey, 8, wr_id_ctr++);
    ++posted_wrs;
    uint64_t last_wr_id = wr_id_ctr;
    PostWrite(&meta_qp, &sentinel_stage, sentinel_mr_cache.lkey(), sentinel_slot_addr, sentinel_rkey,
              8, wr_id_ctr++);
    ++posted_wrs;

    pending_ts.push_back({last_wr_id, post_ts});
    if (pending_ts.size() > static_cast<size_t>(inflight_cap) * 2) {
      pending_ts.erase(pending_ts.begin(), pending_ts.begin() + inflight_cap);
    }

    ++batches;
    bytes_posted += per_slot_blog_bytes;
  }

  // Drain remaining completions.
  while (completed_wrs < posted_wrs) {
    int n = PollCq(meta_qp.cq, wc, 64, &bad);
    if (n < 0) break;
    completed_wrs += n;
  }

  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
  fprintf(stderr,
          "[broker %d] RESULT batches=%lu bytes=%lu secs=%.3f throughput=%.3f Mmsg/s %.3f Gb/s "
          "rtt_p50_us=%.1f rtt_p99_us=%.1f rtt_p999_us=%.1f\n",
          broker_id, (unsigned long)batches, (unsigned long)bytes_posted, secs,
          batches / secs / 1e6, (bytes_posted * 8.0) / secs / 1e9,
          lat.Pct(0.50) / 1e3, lat.Pct(0.99) / 1e3, lat.Pct(0.999) / 1e3);
#if BLOG_PLACEMENT_DRAM
  if (has_right_neighbor) {
    fprintf(stderr, "[broker %d] REPLICA_RESULT posted=%lu completed=%lu\n", broker_id,
            (unsigned long)replica_wr_id_ctr, (unsigned long)replica_completed);
  }
#endif

#if BLOG_PLACEMENT_DRAM
  blog_server_thread.join();
  replica_acceptor_thread.join();
  if (has_right_neighbor) DestroyRcQp(&replica_client_qp);
  DeregisterMr(&local_blog_mr);
  DeregisterMr(&replica_mr);
#else
  DeregisterMr(&stage_mr);
#endif
  DestroyRcQp(&meta_qp);
  CloseDevice(&d);
  return 0;
}
