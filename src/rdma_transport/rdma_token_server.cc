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
};
}  // namespace

int main(int argc, char** argv) {
  int port = atoi(GetArg(argc, argv, "--port", "18800"));
  int num_clients = atoi(GetArg(argc, argv, "--num-clients", "16"));
  int duration_secs = atoi(GetArg(argc, argv, "--duration", "10"));
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;
  fprintf(stderr, "[token-server] dev=%s gid_index=%d num_clients=%d duration=%ds\n", dev.c_str(),
          d.gid_index, num_clients, duration_secs);

  uint64_t counter = 0;
  Mr counter_mr{};
  if (!RegisterMr(d.pd, &counter, sizeof(counter), &counter_mr)) return 1;

  std::vector<RcQp> qps(num_clients);
  for (int i = 0; i < num_clients; ++i) {
    if (!CreateRcQp(&d, 64, 64, /*psn_seed=*/0x8000 + i, &qps[i])) return 1;
    TokenHandshake local{}, remote{};
    local.server_ep = LocalEndpoint(qps[i]);
    local.counter = {reinterpret_cast<uint64_t>(counter_mr.addr), counter_mr.rkey(),
                     static_cast<uint32_t>(counter_mr.len)};
    if (!OobServerExchange(port, &local, &remote, sizeof(TokenHandshake))) {
      fprintf(stderr, "[token-server] handshake failed for client %d\n", i); return 1;
    }
    if (!ConnectRcQp(&qps[i], remote.client_ep)) return 1;
    fprintf(stderr, "[token-server] client %d connected qpn=%u\n", i, remote.client_ep.qpn);
  }

  fprintf(stderr, "[token-server] all %d clients connected; passive for %ds\n", num_clients,
          duration_secs);
  std::this_thread::sleep_for(std::chrono::seconds(duration_secs));
  fprintf(stderr, "[token-server] final counter=%lu\n", (unsigned long)counter);

  for (auto& q : qps) DestroyRcQp(&q);
  DeregisterMr(&counter_mr);
  CloseDevice(&d);
  return 0;
}
