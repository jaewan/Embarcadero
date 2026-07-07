// rdma_transport/rdma_common.h — minimal RoCEv2 RC verbs foundation (pure libibverbs).
//
// Manual RC QP bring-up + a tiny TCP out-of-band (OOB) exchange of QP/MR endpoints, so this builds
// with only libibverbs-dev (no librdmacm-dev). Shared by the W5 token server (reviewer-D repro),
// the W5-B memory server, and the RdmaAccessor skeleton.
//
// Scope: single active port (mlx5_0/port 1), RoCEv2 (GID-addressed). Not thread-safe per QP — one
// posting thread per RcQp, matching the single-writer discipline (spec §1).
#pragma once

#include <infiniband/verbs.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace embarcadero::rdma {

// ---- errors --------------------------------------------------------------------------------------
// All calls return false / nullptr on failure and log via perror-style stderr; callers check.
#define RDMA_CHECK(cond, msg) \
  do { if (!(cond)) { ::embarcadero::rdma::LogFail(msg, __FILE__, __LINE__); return {}; } } while (0)
void LogFail(const char* msg, const char* file, int line);

// ---- device / PD ---------------------------------------------------------------------------------
struct DeviceCtx {
  ibv_context* ctx = nullptr;
  ibv_pd*      pd = nullptr;
  ibv_port_attr port_attr{};
  uint8_t      port_num = 1;
  int          gid_index = -1;   // RoCEv2 IPv4 GID index (scanned)
  ibv_gid      gid{};

  bool ok() const { return ctx && pd && gid_index >= 0; }
};

// Open `dev_name` (e.g. "mlx5_0"), alloc PD, query port, and select the RoCEv2 IPv4 GID.
// If gid_index >= 0 it is used verbatim; else the table is scanned for a RoCEv2 v2 IPv4 entry.
bool OpenDevice(const std::string& dev_name, int gid_index, uint8_t port_num, DeviceCtx* out);
void CloseDevice(DeviceCtx* d);

// Scan the GID table for a RoCEv2 (v2) IPv4-mapped entry; returns index or -1.
int FindRoceV2GidIndex(ibv_context* ctx, uint8_t port_num);

// ---- memory region -------------------------------------------------------------------------------
struct Mr {
  ibv_mr* mr = nullptr;
  void*   addr = nullptr;
  size_t  len = 0;
  uint32_t rkey() const { return mr ? mr->rkey : 0; }
  uint32_t lkey() const { return mr ? mr->lkey : 0; }
};
// Register `buf`/`len` for local+remote READ/WRITE/ATOMIC. `buf` must stay alive for the MR's life.
bool RegisterMr(ibv_pd* pd, void* buf, size_t len, Mr* out);
void DeregisterMr(Mr* m);

// ---- completion queue + queue pair ---------------------------------------------------------------
struct RcQp {
  ibv_cq* cq = nullptr;
  ibv_qp* qp = nullptr;
  uint32_t local_psn = 0;
  DeviceCtx* dev = nullptr;
  bool owns_cq = true;  // false for CreateRcQpOnSharedCq — DestroyRcQp then leaves `cq` alone
  bool ok() const { return cq && qp; }
};
// Create a CQ (depth `cqe`) and an RC QP (send/recv depth `max_wr`). PSN is derived from `psn_seed`
// (workflows/tests pass an index; no RNG so runs are reproducible).
bool CreateRcQp(DeviceCtx* dev, int cqe, int max_wr, uint32_t psn_seed, RcQp* out);
// Like CreateRcQp, but binds the new QP to an EXISTING CQ instead of creating one. Lets a single
// poll loop watch completions from many QPs at once (e.g. one per remote broker) — needed
// whenever a pipelined poller must treat several one-sided-target QPs as one completion stream.
// DestroyRcQp will NOT destroy a shared CQ; the caller owns its lifetime.
bool CreateRcQpOnSharedCq(DeviceCtx* dev, ibv_cq* shared_cq, int max_wr, uint32_t psn_seed, RcQp* out);
void DestroyRcQp(RcQp* q);

// Endpoint info exchanged out-of-band to connect two QPs.
struct QpEndpoint {
  uint32_t qpn = 0;
  uint32_t psn = 0;
  ibv_gid  gid{};
  uint16_t lid = 0;   // 0 for RoCE
  // Port active MTU (ibv_mtu enum) at endpoint creation. Exchanged so BOTH sides can set
  // path_mtu = min(local, remote), like stock perftest. Without negotiation, an active-MTU
  // divergence between the two ports (the fabric MTU config is runtime/non-persistent) makes
  // the requester segment WRITEs at a PMTU the responder rejects -> IBV_WC_REM_INV_REQ_ERR on
  // the first write, while READ (requester-side response check is laxer) and single-packet
  // atomics keep passing. 0 = legacy peer (field absent): fall back to the local active MTU.
  uint8_t  mtu = 0;
};
QpEndpoint LocalEndpoint(const RcQp& q);

// Drive INIT->RTR->RTS against a remote endpoint. path_mtu = min(local active, remote.mtu).
bool ConnectRcQp(RcQp* q, const QpEndpoint& remote);

// ---- one-sided ops (post + poll a single completion) ---------------------------------------------
// Post an RDMA READ of `len` bytes from remote[raddr,rkey] into local[laddr,lkey]. Signaled.
bool PostRead(RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey, uint32_t len,
              uint64_t wr_id);
// Post an RDMA WRITE of `len` bytes from local[laddr,lkey] to remote[raddr,rkey]. Signaled.
bool PostWrite(RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey, uint32_t len,
               uint64_t wr_id);
// Post an ATOMIC FETCH_ADD of `add` to remote 8B [raddr,rkey]; old value lands in local[laddr,lkey].
bool PostFetchAdd(RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey, uint64_t add,
                  uint64_t wr_id);
// Poll up to `max` completions (blocking spin). Returns #completions, or -1 on a WC error.
// On error, the offending status is written to *bad_status (ibv_wc_status).
int PollCq(ibv_cq* cq, ibv_wc* wc, int max, int* bad_status);

// ---- tiny TCP out-of-band exchange ---------------------------------------------------------------
// Blocking, one-shot. Server listens on `port`; client connects to `ip`:`port`. Both send their own
// blob and receive the peer's (same fixed size). Used to swap QpEndpoint + MR descriptors at setup.
bool OobServerExchange(int port, const void* send, void* recv, size_t n);
bool OobClientExchange(const std::string& ip, int port, const void* send, void* recv, size_t n);

// A convenience bundle a target (server) advertises for each region it exposes.
struct RegionDesc {
  uint64_t addr = 0;
  uint32_t rkey = 0;
  uint32_t len = 0;
};

// Monotonic nanosecond clock (CLOCK_MONOTONIC) for rate/latency measurement.
uint64_t NowNs();

}  // namespace embarcadero::rdma
