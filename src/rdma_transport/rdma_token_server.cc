// rdma_transport/rdma_token_server.cc — passive target for the reviewer-D naive RDMA token
// repro (spec §5, improvement_plan.md E10 tie-in). ONE 8B counter MR, N client QP connections
// accepted in sequence (same accept-loop pattern as rdma_memserver.cc, deliberately not shared
// code since this is a much smaller, single-purpose harness).
//
// Usage: rdma_token_server --port=18800 --num-clients=N --duration=10

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "rdma_transport/rdma_common.h"

using namespace embarcadero::rdma;

namespace {
const char* GetArg(int argc, char** argv, const char* key, const char* def) {
  size_t klen = strlen(key);
  for (int i = 1; i < argc; ++i)
    if (!strncmp(argv[i], key, klen) && argv[i][klen] == '=') return argv[i] + klen + 1;
  return def;
}
struct TokenHandshake {
  QpEndpoint client_ep;   // client -> server
  QpEndpoint server_ep;   // server -> client
  RegionDesc counter;     // server -> client
  uint32_t num_counters;  // server -> client: 1 = single global counter (real ordering
                          // semantics); >1 = sharded/independent counters (each on its own
                          // cache line, no cross-QP contention -- reproduces reviewer-D's
                          // aggregate-rate config, but does NOT give a single global order)
  uint32_t stride;        // server -> client: byte stride between shards
};
}  // namespace

int main(int argc, char** argv) {
  int port = atoi(GetArg(argc, argv, "--port", "18800"));
  int num_clients = atoi(GetArg(argc, argv, "--num-clients", "16"));
  int duration_secs = atoi(GetArg(argc, argv, "--duration", "10"));
  int num_counters = atoi(GetArg(argc, argv, "--num-counters", "1"));
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));
  constexpr uint32_t kStride = 64;  // one cache line per shard, avoids false sharing

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;
  fprintf(stderr, "[token-server] dev=%s gid_index=%d num_clients=%d num_counters=%d duration=%ds\n",
          dev.c_str(), d.gid_index, num_clients, num_counters, duration_secs);

  std::vector<uint8_t> counters(static_cast<size_t>(num_counters) * kStride, 0);
  Mr counter_mr{};
  if (!RegisterMr(d.pd, counters.data(), counters.size(), &counter_mr)) return 1;

  // ONE listening socket for the whole batch (see rdma_memserver.cc for why: a fresh
  // listen+accept+close per client races a concurrent multi-QP client that may connect all its
  // QPs back-to-back faster than the server can cycle its listener).
  int listen_fd = OobServerListen(port, num_clients);
  if (listen_fd < 0) return 1;
  std::vector<RcQp> qps(num_clients);
  for (int i = 0; i < num_clients; ++i) {
    if (!CreateRcQp(&d, 64, 64, /*psn_seed=*/0x8000 + i, &qps[i])) return 1;
    TokenHandshake local{}, remote{};
    local.server_ep = LocalEndpoint(qps[i]);
    local.counter = {reinterpret_cast<uint64_t>(counter_mr.addr), counter_mr.rkey(),
                     static_cast<uint32_t>(counter_mr.len)};
    local.num_counters = static_cast<uint32_t>(num_counters);
    local.stride = kStride;
    if (!OobServerAccept(listen_fd, &local, &remote, sizeof(TokenHandshake))) {
      fprintf(stderr, "[token-server] handshake failed for client %d\n", i);
      OobServerClose(listen_fd);
      return 1;
    }
    if (!ConnectRcQp(&qps[i], remote.client_ep)) return 1;
    fprintf(stderr, "[token-server] client %d connected qpn=%u\n", i, remote.client_ep.qpn);
  }
  OobServerClose(listen_fd);

  fprintf(stderr, "[token-server] all %d clients connected; passive for %ds\n", num_clients,
          duration_secs);
  std::this_thread::sleep_for(std::chrono::seconds(duration_secs));
  uint64_t total = 0;
  for (int i = 0; i < num_counters; ++i) {
    total += *reinterpret_cast<uint64_t*>(counters.data() + static_cast<size_t>(i) * kStride);
  }
  fprintf(stderr, "[token-server] final counter total=%lu (across %d shard(s))\n",
          (unsigned long)total, num_counters);

  for (auto& q : qps) DestroyRcQp(&q);
  DeregisterMr(&counter_mr);
  CloseDevice(&d);
  return 0;
}
