// rdma_transport/rdma_memserver.cc — W5-B (and W5-A metadata-half) passive memory server.
//
// Spec (docs/design/W5_RDMA_VARIANTS_SPEC.md) Decision 1: ALL failure-critical metadata
// (ControlBlock, CompletionVector, PBR rings, packed sentinel array, GOI) lives on this memserver
// in BOTH W5-A and W5-B. The payload Blog is additionally hosted here ONLY when --host-blog is set
// (W5-B); in W5-A the Blog lives on each broker's own DRAM (rdma_broker.cc) and this process hosts
// metadata only.
//
// Passive by design (spec §4.1): once QPs reach RTS, ordinary RDMA WRITE from a remote requester
// completes with NO completion at this (responder) side and NO CPU involvement here — the memory
// server's CPU is never in the data path. This process's only jobs are (1) bring up N broker QPs +
// 1 sequencer QP over an RC connection each, (2) advertise region descriptors for every MR it
// hosts, (3) sleep for the test duration, (4) dump final region state for an independent
// correctness check, (5) tear down cleanly.
//
// Usage:
//   rdma_memserver --port=18600 --num-brokers=2 --pbr-slots=4096 --goi-entries=1000000
//                  [--host-blog] [--blog-bytes=1073741824] [--duration=10]
//                  [--dev=mlx5_0] [--gid=-1]
//
// Launch under numactl for NUMA-local placement to the NIC's PCIe root (spec §4.1):
//   numactl --cpunodebind=<nic_numa_node> --membind=<nic_numa_node> rdma_memserver ...

#include <sys/mman.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
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
bool HasFlag(int argc, char** argv, const char* key) {
  for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], key)) return true;
  return false;
}

void* AllocHugeOrFallback(size_t bytes) {
  size_t aligned = (bytes + (1ull << 21) - 1) & ~((1ull << 21) - 1);  // round to 2MB
  void* p = mmap(nullptr, aligned, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (p != MAP_FAILED) {
    fprintf(stderr, "[memserver] hugepage alloc ok: %zu bytes (2MB-aligned %zu)\n", bytes, aligned);
    return p;
  }
  fprintf(stderr, "[memserver] MAP_HUGETLB failed (%s), falling back to regular mmap\n", strerror(errno));
  p = mmap(nullptr, aligned, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { perror("mmap fallback"); exit(1); }
  return p;
}

}  // namespace

int main(int argc, char** argv) {
  int port = atoi(GetArg(argc, argv, "--port", "18600"));
  int num_brokers = atoi(GetArg(argc, argv, "--num-brokers", "2"));
  int pbr_slots = atoi(GetArg(argc, argv, "--pbr-slots", "4096"));
  long goi_entries = atol(GetArg(argc, argv, "--goi-entries", "1000000"));
  bool host_blog = HasFlag(argc, argv, "--host-blog");
  size_t blog_bytes_per_broker = atoll(GetArg(argc, argv, "--blog-bytes", "1073741824"));  // 1 GiB
  int duration_secs = atoi(GetArg(argc, argv, "--duration", "10"));
  size_t spare_bytes = atoll(GetArg(argc, argv, "--spare-bytes", "0"));  // Phase 2 recovery target; 0 = none
  int recovery_port = atoi(GetArg(argc, argv, "--recovery-port", "18690"));
  // Sub-phase 3A item 2: a multi-threaded W5-B sequencer opens ONE QP per worker thread instead
  // of one QP for the whole sequencer process -- the memserver's fixed accept loop needs to know
  // how many sequencer-role connections to expect (all use role=1; the memserver doesn't need to
  // tell them apart individually).
  int num_sequencer_conns = atoi(GetArg(argc, argv, "--num-sequencer-conns", "1"));
  int max_recoveries = atoi(GetArg(argc, argv, "--max-recoveries", "4"));
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));

  fprintf(stderr, "[memserver] num_brokers=%d pbr_slots=%d goi_entries=%ld host_blog=%d "
                  "blog_bytes/broker=%zu duration=%ds\n",
          num_brokers, pbr_slots, goi_entries, host_blog ? 1 : 0, blog_bytes_per_broker, duration_secs);

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;
  fprintf(stderr, "[memserver] dev=%s gid_index=%d active_mtu=%d\n", dev.c_str(), d.gid_index,
          d.port_attr.active_mtu);

  MemserverLayout layout;
  layout.num_brokers = static_cast<size_t>(num_brokers);
  layout.pbr_slots_per_broker = static_cast<size_t>(pbr_slots);
  layout.goi_entries = static_cast<size_t>(goi_entries);

  // ---- Allocate + register every metadata region (Decision 1: always, both variants) ----
  void* control_mem = AllocHugeOrFallback(layout.control_block_bytes());
  void* cv_mem = AllocHugeOrFallback(layout.completion_vector_bytes());
  void* sentinel_mem = AllocHugeOrFallback(layout.sentinel_array_bytes());
  void* goi_mem = AllocHugeOrFallback(layout.goi_bytes());
  void* pbr_mem = AllocHugeOrFallback(layout.pbr_ring_bytes_total());
  memset(control_mem, 0, layout.control_block_bytes());
  memset(cv_mem, 0, layout.completion_vector_bytes());
  memset(pbr_mem, 0, layout.pbr_ring_bytes_total());
  // Init sentinel array + every PBR slot's publish_commit to "uncommitted" — the sequencer must
  // never mistake a zeroed slot for a valid commit (kPublishUncommitted != 0).
  auto* sentinels = static_cast<SentinelSlot*>(sentinel_mem);
  for (size_t i = 0; i < layout.num_brokers; ++i) sentinels[i].value = kPublishUncommitted;
  auto* pbr_slots_arr = static_cast<BatchHeaderMirror*>(pbr_mem);
  for (size_t i = 0; i < layout.num_brokers * layout.pbr_slots_per_broker; ++i)
    pbr_slots_arr[i].publish_commit = kPublishUncommitted;

  void* blog_mem = nullptr;
  size_t blog_total = 0;
  if (host_blog) {
    blog_total = layout.num_brokers * blog_bytes_per_broker;
    blog_mem = AllocHugeOrFallback(blog_total);
    memset(blog_mem, 0, blog_total < (1u << 20) ? blog_total : (1u << 20));  // touch first page only; full zero is unnecessary for a payload buffer
  }
  void* spare_mem = nullptr;
  if (spare_bytes > 0) {
    spare_mem = AllocHugeOrFallback(spare_bytes);
    memset(spare_mem, 0, spare_bytes < (1u << 20) ? spare_bytes : (1u << 20));
  }

  Mr control_mr{}, cv_mr{}, sentinel_mr{}, goi_mr{}, pbr_mr{}, blog_mr{}, spare_mr{};
  if (!RegisterMr(d.pd, control_mem, layout.control_block_bytes(), &control_mr)) return 1;
  if (!RegisterMr(d.pd, cv_mem, layout.completion_vector_bytes(), &cv_mr)) return 1;
  if (!RegisterMr(d.pd, sentinel_mem, layout.sentinel_array_bytes(), &sentinel_mr)) return 1;
  if (!RegisterMr(d.pd, goi_mem, layout.goi_bytes(), &goi_mr)) return 1;
  if (!RegisterMr(d.pd, pbr_mem, layout.pbr_ring_bytes_total(), &pbr_mr)) return 1;
  if (host_blog && !RegisterMr(d.pd, blog_mem, blog_total, &blog_mr)) return 1;
  if (spare_bytes > 0 && !RegisterMr(d.pd, spare_mem, spare_bytes, &spare_mr)) return 1;

  fprintf(stderr, "[memserver] regions registered: control=%zuB cv=%zuB sentinel=%zuB goi=%zuB "
                  "pbr=%zuB blog=%zuB spare=%zuB\n",
          layout.control_block_bytes(), layout.completion_vector_bytes(),
          layout.sentinel_array_bytes(), layout.goi_bytes(), layout.pbr_ring_bytes_total(),
          host_blog ? blog_total : 0, spare_bytes);

  // ---- Accept num_brokers + 1 (sequencer) QP connections ----
  // Passive server: ONE listening socket stays open for the whole batch (backlog sized for all
  // total_clients), so a client's connect can land at any point during the accept loop instead of
  // only during the narrow window a single-shot listen+accept+close is up — that race caused a
  // hang under concurrent multi-host launch (see w5_phase1_progress.md). Connect order among
  // brokers/sequencer is still unconstrained (role+broker_id in the hello disambiguates).
  int total_clients = num_brokers + num_sequencer_conns;
  int listen_fd = OobServerListen(port, total_clients);
  if (listen_fd < 0) return 1;
  std::vector<RcQp> qps(total_clients);
  for (int i = 0; i < total_clients; ++i) {
    if (!CreateRcQp(&d, 4096, 512, /*psn_seed=*/0x3000 + i, &qps[i])) return 1;
    HandshakeBlob local{}, remote{};
    local.server_ep = LocalEndpoint(qps[i]);
    local.control = {reinterpret_cast<uint64_t>(control_mr.addr), control_mr.rkey(),
                      static_cast<uint32_t>(control_mr.len)};
    local.cv = {reinterpret_cast<uint64_t>(cv_mr.addr), cv_mr.rkey(), static_cast<uint32_t>(cv_mr.len)};
    local.sentinel = {reinterpret_cast<uint64_t>(sentinel_mr.addr), sentinel_mr.rkey(),
                       static_cast<uint32_t>(sentinel_mr.len)};
    local.goi = {reinterpret_cast<uint64_t>(goi_mr.addr), goi_mr.rkey(), static_cast<uint32_t>(goi_mr.len)};
    local.pbr = {reinterpret_cast<uint64_t>(pbr_mr.addr), pbr_mr.rkey(), static_cast<uint32_t>(pbr_mr.len)};
    if (host_blog) {
      local.blog = {reinterpret_cast<uint64_t>(blog_mr.addr), blog_mr.rkey(),
                    static_cast<uint32_t>(blog_mr.len)};
    }
    if (spare_bytes > 0) {
      local.spare = {reinterpret_cast<uint64_t>(spare_mr.addr), spare_mr.rkey(),
                     static_cast<uint32_t>(spare_mr.len)};
    }
    local.pbr_ring_bytes_per_broker = static_cast<uint32_t>(layout.pbr_ring_bytes_per_broker());
    local.blog_bytes_per_broker = static_cast<uint32_t>(blog_bytes_per_broker);
    local.host_blog = host_blog ? 1 : 0;
    local.goi_entries = static_cast<uint32_t>(layout.goi_entries);

    // ONE exchange: client's half (role/broker_id/client_ep) arrives in `remote`; our half (above)
    // goes out in `local`. Single TCP connect/accept for the whole handshake.
    if (!OobServerAccept(listen_fd, &local, &remote, sizeof(HandshakeBlob))) {
      fprintf(stderr, "[memserver] handshake exchange failed for client %d\n", i);
      OobServerClose(listen_fd);
      return 1;
    }
    fprintf(stderr, "[memserver] client %d hello: role=%d broker_id=%d\n", i, remote.role, remote.broker_id);
    if (!ConnectRcQp(&qps[i], remote.client_ep)) return 1;
    fprintf(stderr, "[memserver] client %d connected qpn=%u\n", i, remote.client_ep.qpn);
  }
  OobServerClose(listen_fd);

  fprintf(stderr, "[memserver] all %d clients connected; passive for %ds (CPU not in data path)\n",
          total_clients, duration_secs);

  // ---- Phase 2 recovery-target acceptor (spare_bytes>0 only) ----
  // Separate, independent listener from the fixed-count client loop above: the recovery tool
  // connects LATE (only after a kill has actually happened, at an unpredictable point mid-run),
  // not as part of the initial bring-up, so it needs its own persistent listener that stays armed
  // for the whole run rather than closing after a fixed handshake count.
  std::vector<RcQp> recovery_qps;
  std::thread recovery_acceptor;
  if (spare_bytes > 0) {
    recovery_acceptor = std::thread([&]() {
      int rfd = OobServerListen(recovery_port, max_recoveries);
      if (rfd < 0) { fprintf(stderr, "[memserver] recovery-acceptor listen failed\n"); return; }
      for (int i = 0; i < max_recoveries; ++i) {
        RcQp rqp{};
        if (!CreateRcQp(&d, 64, 64, /*psn_seed=*/0x9500 + i, &rqp)) break;
        ReplicaHandoffBlob hlocal{}, hremote{};
        hlocal.role = 1;  // recovery target: someone is about to WRITE a recovered replica here
        hlocal.ep = LocalEndpoint(rqp);
        hlocal.region = {reinterpret_cast<uint64_t>(spare_mr.addr), spare_mr.rkey(),
                         static_cast<uint32_t>(spare_mr.len)};
        if (!OobServerAccept(rfd, &hlocal, &hremote, sizeof(ReplicaHandoffBlob))) break;  // no more
                                                                                            // recoveries this run
        if (!ConnectRcQp(&rqp, hremote.ep)) break;
        fprintf(stderr, "[memserver] recovery connection %d accepted qpn=%u\n", i, hremote.ep.qpn);
        recovery_qps.push_back(rqp);
      }
      OobServerClose(rfd);
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(duration_secs));
  if (recovery_acceptor.joinable()) recovery_acceptor.join();

  // ---- Post-run independent state dump (correctness cross-check, same spirit as Track 03's
  // mailbox benches' independent recompute) ----
  fprintf(stderr, "[memserver] --- final state dump ---\n");
  auto* cb = static_cast<ControlBlockMirror*>(control_mem);
  fprintf(stderr, "[memserver] ControlBlock: epoch=%lu committed_seq=%lu sequencer_id=%u\n",
          (unsigned long)cb->epoch, (unsigned long)cb->committed_seq, cb->sequencer_id);
  auto* cvs = static_cast<CompletionVectorEntryMirror*>(cv_mem);
  for (int b = 0; b < num_brokers; ++b)
    fprintf(stderr, "[memserver] CV[broker=%d] ack_offset=%lu\n", b, (unsigned long)cvs[b].ack_offset);
  for (int b = 0; b < num_brokers; ++b)
    fprintf(stderr, "[memserver] sentinel[broker=%d]=%lu\n", b, (unsigned long)sentinels[b].value);

  for (auto& q : qps) DestroyRcQp(&q);
  for (auto& q : recovery_qps) DestroyRcQp(&q);
  DeregisterMr(&control_mr); DeregisterMr(&cv_mr); DeregisterMr(&sentinel_mr);
  DeregisterMr(&goi_mr); DeregisterMr(&pbr_mr);
  if (host_blog) DeregisterMr(&blog_mr);
  if (spare_bytes > 0) DeregisterMr(&spare_mr);
  CloseDevice(&d);
  fprintf(stderr, "[memserver] clean exit\n");
  return 0;
}
