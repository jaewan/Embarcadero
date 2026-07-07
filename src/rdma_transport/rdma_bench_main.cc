// rdma_transport/rdma_bench_main.cc — cross-host one-sided micro over the RoCEv2 RC foundation.
//
// Validates rdma_common end-to-end and provides two W5 measurements:
//   --op=fetchadd : naive RDMA token rate (reviewer-D repro, spec §5). NOTE: RC caps outstanding
//                   atomics (~16/QP), so a single QP won't hit ~30 M tok/s — scale QPs/threads later.
//   --op=write|read : one-sided bandwidth (W5-B funnel probe; compare to ib_write_bw device ceiling).
//
// Usage:
//   server: rdma_bench --role=server [--dev=mlx5_0] [--gid=-1] [--port=18515] [--bytes=1048576]
//   client: rdma_bench --role=client --ip=10.10.10.13 [--dev=mlx5_0] [--gid=-1] [--port=18515]
//           [--op=write] [--size=65536] [--inflight=16] [--secs=5]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "rdma_transport/rdma_common.h"

using namespace embarcadero::rdma;

namespace {
struct ConnMsg { QpEndpoint ep; RegionDesc region; };

const char* GetArg(int argc, char** argv, const char* key, const char* def) {
  size_t klen = strlen(key);
  for (int i = 1; i < argc; ++i)
    if (!strncmp(argv[i], key, klen) && argv[i][klen] == '=') return argv[i] + klen + 1;
  return def;
}
}  // namespace

int main(int argc, char** argv) {
  std::string role = GetArg(argc, argv, "--role", "");
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  std::string ip = GetArg(argc, argv, "--ip", "");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));
  int port = atoi(GetArg(argc, argv, "--port", "18515"));
  std::string op = GetArg(argc, argv, "--op", "write");
  uint32_t size = atoi(GetArg(argc, argv, "--size", "65536"));
  size_t region_bytes = atoll(GetArg(argc, argv, "--bytes", "1048576"));
  int inflight = atoi(GetArg(argc, argv, "--inflight", "16"));
  int secs = atoi(GetArg(argc, argv, "--secs", "5"));

  if (role != "server" && role != "client") {
    fprintf(stderr, "need --role=server|client\n");
    return 2;
  }

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;
  fprintf(stderr, "[%s] dev=%s gid_index=%d active_mtu=%d\n", role.c_str(), dev.c_str(), d.gid_index,
          d.port_attr.active_mtu);

  // Registered buffer: server exposes it as the target; client uses it as local staging.
  size_t buf_len = (role == "server") ? region_bytes : (op == "fetchadd" ? 8 : size);
  void* buf = nullptr;
  if (posix_memalign(&buf, 4096, buf_len < 8 ? 8 : buf_len)) { perror("posix_memalign"); return 1; }
  memset(buf, 0, buf_len < 8 ? 8 : buf_len);
  Mr mr{};
  if (!RegisterMr(d.pd, buf, buf_len < 8 ? 8 : buf_len, &mr)) return 1;

  RcQp q{};
  if (!CreateRcQp(&d, 4096, 512, /*psn_seed=*/(role == "server" ? 0x1000 : 0x2000), &q)) return 1;

  ConnMsg local{}, remote{};
  local.ep = LocalEndpoint(q);
  local.region = {reinterpret_cast<uint64_t>(mr.addr), mr.rkey(), static_cast<uint32_t>(mr.len)};

  bool ok = (role == "server") ? OobServerExchange(port, &local, &remote, sizeof(ConnMsg))
                               : OobClientExchange(ip, port, &local, &remote, sizeof(ConnMsg));
  if (!ok) { fprintf(stderr, "OOB exchange failed\n"); return 1; }
  if (!ConnectRcQp(&q, remote.ep)) return 1;
  fprintf(stderr, "[%s] connected to qpn=%u\n", role.c_str(), remote.ep.qpn);

  if (role == "server") {
    // Passive target: the NIC serves one-sided ops. Stay alive while the client drives, then report.
    fprintf(stderr, "[server] passive; sleeping %ds while client drives...\n", secs + 3);
    sleep(secs + 3);
    fprintf(stderr, "[server] counter (first 8B) = %llu\n",
            static_cast<unsigned long long>(*reinterpret_cast<volatile uint64_t*>(buf)));
    DestroyRcQp(&q); DeregisterMr(&mr); free(buf); CloseDevice(&d);
    return 0;
  }

  // ---- client: pipeline `inflight` one-sided ops for `secs`, measure rate/bandwidth ----
  if (op == "fetchadd" && inflight > 16) inflight = 16;  // RC atomic outstanding cap
  // H1 bounds guard: an op larger than the peer's advertised region is a guaranteed
  // REM_INV_REQ with a cryptic WC — fail fast with the actual numbers instead.
  if (op != "fetchadd" && (size == 0 || size > remote.region.len)) {
    fprintf(stderr, "[client] --size=%u out of bounds for remote region len=%u (server --bytes)\n",
            size, remote.region.len);
    return 1;
  }
  uint64_t raddr = remote.region.addr;
  uint32_t rkey = remote.region.rkey;
  auto post = [&](uint64_t wr_id) -> bool {
    if (op == "read")  return PostRead(&q, buf, mr.lkey(), raddr, rkey, size, wr_id);
    if (op == "write") return PostWrite(&q, buf, mr.lkey(), raddr, rkey, size, wr_id);
    return PostFetchAdd(&q, buf, mr.lkey(), raddr, rkey, /*add=*/1, wr_id);
  };

  ibv_wc wc[64];
  int bad = 0;
  uint64_t ops = 0, t0 = NowNs(), deadline = t0 + static_cast<uint64_t>(secs) * 1000000000ull;
  for (int i = 0; i < inflight; ++i) if (!post(i)) return 1;
  while (NowNs() < deadline) {
    int n = PollCq(q.cq, wc, 64, &bad);
    if (n < 0) { fprintf(stderr, "[client] WC error status=%d\n", bad); return 1; }
    ops += n;
    for (int i = 0; i < n; ++i) if (!post(ops + i)) return 1;
  }
  uint64_t dt = NowNs() - t0;
  double secs_f = dt / 1e9;
  double rate = ops / secs_f;
  double gbps = (op == "fetchadd") ? 0.0 : (ops * (double)size * 8.0) / (secs_f * 1e9);
  printf("op=%s size=%u inflight=%d : %.2f Mops/s", op.c_str(), size, inflight, rate / 1e6);
  if (op != "fetchadd") printf("  %.2f Gb/s", gbps);
  printf("  (%llu ops in %.2fs)\n", static_cast<unsigned long long>(ops), secs_f);

  DestroyRcQp(&q); DeregisterMr(&mr); free(buf); CloseDevice(&d);
  return 0;
}
