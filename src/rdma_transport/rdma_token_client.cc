// rdma_transport/rdma_token_client.cc — reviewer-D naive RDMA token repro (spec §5).
//
// Two modes, matching the two things the pre-registration (docs/experiments/
// w5_e4e_preregistration.md) commits to measuring:
//   --mode=throughput (default): open --num-qps QPs to the token server, pipeline up to
//     --inflight outstanding FETCH_ADDs per QP (RC caps outstanding atomics at 16/QP —
//     ConnectRcQp sets max_dest_rd_atomic=16), single-threaded round-robin across QPs (the NIC/
//     PCIe path is the bottleneck for 8B atomics, not the posting CPU, so one thread cycling
//     many QPs is sufficient and avoids multi-threaded QP posting races). Reproduces the
//     aggregate token RATE.
//   --mode=latency: ONE QP, ONE outstanding FETCH_ADD at a time (strictly serial — post, block
//     until completion, repeat). This is the number that matters for the pre-registered claim:
//     "token RTT on the client critical path" is what a real ordered-append would pay per
//     message if it depended on this token server, regardless of how fast the server can go in
//     aggregate. Reports p50/p99/p99.9 RTT for direct comparison against the delta-measurement
//     branch's append_send_to_ack numbers (1.0-1.1 ms typical, 4.95 ms worst).
//
// Usage:
//   rdma_token_client --server-ip=10.10.10.181 --mode=throughput --num-qps=16 --inflight=16 --duration=5
//   rdma_token_client --server-ip=10.10.10.181 --mode=latency --duration=5

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  QpEndpoint client_ep;
  QpEndpoint server_ep;
  RegionDesc counter;
};
uint64_t NowNsLocal() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

int main(int argc, char** argv) {
  std::string server_ip = GetArg(argc, argv, "--server-ip", "");
  int port = atoi(GetArg(argc, argv, "--port", "18800"));
  std::string mode = GetArg(argc, argv, "--mode", "throughput");
  int num_qps = atoi(GetArg(argc, argv, "--num-qps", "16"));
  int inflight = atoi(GetArg(argc, argv, "--inflight", "16"));
  int duration_secs = atoi(GetArg(argc, argv, "--duration", "5"));
  std::string dev = GetArg(argc, argv, "--dev", "mlx5_0");
  int gid = atoi(GetArg(argc, argv, "--gid", "-1"));

  if (mode == "latency") num_qps = 1;
  if (inflight > 16) inflight = 16;  // RC outstanding-atomic cap

  DeviceCtx d{};
  if (!OpenDevice(dev, gid, 1, &d)) return 1;

  std::vector<uint64_t> landing(num_qps * inflight, 0);
  Mr landing_mr{};
  if (!RegisterMr(d.pd, landing.data(), landing.size() * sizeof(uint64_t), &landing_mr)) return 1;

  std::vector<RcQp> qps(num_qps);
  RegionDesc counter{};
  for (int i = 0; i < num_qps; ++i) {
    if (!CreateRcQp(&d, 4096, 64, /*psn_seed=*/0x9000 + i, &qps[i])) return 1;
    TokenHandshake local{}, remote{};
    local.client_ep = LocalEndpoint(qps[i]);
    if (!OobClientExchange(server_ip, port, &local, &remote, sizeof(TokenHandshake))) {
      fprintf(stderr, "[token-client] OOB exchange failed (qp %d)\n", i); return 1;
    }
    if (!ConnectRcQp(&qps[i], remote.server_ep)) return 1;
    counter = remote.counter;
  }
  fprintf(stderr, "[token-client] mode=%s num_qps=%d inflight=%d duration=%ds\n", mode.c_str(),
          num_qps, inflight, duration_secs);

  ibv_wc wc[64];
  int bad = 0;
  const auto t_start = std::chrono::steady_clock::now();
  const auto deadline = t_start + std::chrono::seconds(duration_secs);

  if (mode == "latency") {
    std::vector<uint64_t> samples;
    while (std::chrono::steady_clock::now() < deadline) {
      uint64_t t0 = NowNsLocal();
      PostFetchAdd(&qps[0], &landing[0], landing_mr.lkey(), counter.addr, counter.rkey, 1, 0);
      int n = PollCq(qps[0].cq, wc, 1, &bad);
      if (n < 0) { fprintf(stderr, "[token-client] WC error status=%d\n", bad); return 1; }
      samples.push_back(NowNsLocal() - t0);
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) -> double {
      if (samples.empty()) return 0;
      return samples[static_cast<size_t>(p * (samples.size() - 1))] / 1e3;  // -> us
    };
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    fprintf(stderr, "[token-client] RESULT mode=latency ops=%zu secs=%.3f rate=%.1f/s "
                    "rtt_p50_us=%.2f p99_us=%.2f p999_us=%.2f\n",
            samples.size(), secs, samples.size() / secs, pct(0.50), pct(0.99), pct(0.999));
  } else {
    std::vector<uint64_t> posted(num_qps, 0), completed(num_qps, 0);
    for (int q = 0; q < num_qps; ++q) {
      for (int i = 0; i < inflight; ++i) {
        PostFetchAdd(&qps[q], &landing[q * inflight + i], landing_mr.lkey(), counter.addr,
                     counter.rkey, 1, /*wr_id=*/i);
        ++posted[q];
      }
    }
    uint64_t total_ops = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      for (int q = 0; q < num_qps; ++q) {
        int n = ibv_poll_cq(qps[q].cq, 64, wc);
        if (n < 0) { fprintf(stderr, "[token-client] poll error qp=%d\n", q); return 1; }
        if (n == 0) continue;
        for (int i = 0; i < n; ++i) {
          if (wc[i].status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[token-client] WC error qp=%d status=%d\n", q, wc[i].status);
            return 1;
          }
        }
        completed[q] += n;
        total_ops += n;
        for (int i = 0; i < n; ++i) {
          uint64_t slot = wc[i].wr_id;
          PostFetchAdd(&qps[q], &landing[q * inflight + slot], landing_mr.lkey(), counter.addr,
                       counter.rkey, 1, slot);
          ++posted[q];
        }
      }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    fprintf(stderr, "[token-client] RESULT mode=throughput num_qps=%d inflight=%d ops=%lu "
                    "secs=%.3f rate=%.3f Mtok/s\n",
            num_qps, inflight, (unsigned long)total_ops, secs, total_ops / secs / 1e6);
  }

  for (auto& q : qps) DestroyRcQp(&q);
  DeregisterMr(&landing_mr);
  CloseDevice(&d);
  return 0;
}
