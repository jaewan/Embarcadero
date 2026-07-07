// rdma_transport/rdma_common.cc — see rdma_common.h.
#include "rdma_transport/rdma_common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace embarcadero::rdma {

void LogFail(const char* msg, const char* file, int line) {
  fprintf(stderr, "[rdma] FAIL: %s (%s:%d) errno=%d(%s)\n", msg, file, line, errno,
          errno ? strerror(errno) : "");
}

uint64_t NowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// ---- device --------------------------------------------------------------------------------------
int FindRoceV2GidIndex(ibv_context* ctx, uint8_t port_num) {
  const char* dev = ibv_get_device_name(ctx->device);
  for (int i = 0; i < 128; ++i) {
    char path[256], buf[128];
    // gid type must be RoCE v2
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%u/gid_attrs/types/%d", dev,
             port_num, i);
    FILE* f = fopen(path, "r");
    if (!f) continue;
    buf[0] = 0;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); continue; }
    fclose(f);
    if (!strstr(buf, "v2")) continue;  // "RoCE v2"
    // gid must be IPv4-mapped (…:ffff:XXXX:XXXX)
    snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%u/gids/%d", dev, port_num, i);
    f = fopen(path, "r");
    if (!f) continue;
    buf[0] = 0;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); continue; }
    fclose(f);
    if (strstr(buf, "0000:0000:0000:0000:0000:ffff:")) return i;
  }
  return -1;
}

bool OpenDevice(const std::string& dev_name, int gid_index, uint8_t port_num, DeviceCtx* out) {
  ibv_device** list = ibv_get_device_list(nullptr);
  if (!list) { LogFail("ibv_get_device_list", __FILE__, __LINE__); return false; }
  ibv_device* dev = nullptr;
  for (int i = 0; list[i]; ++i)
    if (dev_name == ibv_get_device_name(list[i])) { dev = list[i]; break; }
  if (!dev) { LogFail("device not found", __FILE__, __LINE__); ibv_free_device_list(list); return false; }

  out->ctx = ibv_open_device(dev);
  ibv_free_device_list(list);
  if (!out->ctx) { LogFail("ibv_open_device", __FILE__, __LINE__); return false; }
  out->port_num = port_num;
  out->pd = ibv_alloc_pd(out->ctx);
  if (!out->pd) { LogFail("ibv_alloc_pd", __FILE__, __LINE__); return false; }
  if (ibv_query_port(out->ctx, port_num, &out->port_attr)) {
    LogFail("ibv_query_port", __FILE__, __LINE__); return false;
  }
  out->gid_index = (gid_index >= 0) ? gid_index : FindRoceV2GidIndex(out->ctx, port_num);
  if (out->gid_index < 0) { LogFail("no RoCEv2 IPv4 GID", __FILE__, __LINE__); return false; }
  if (ibv_query_gid(out->ctx, port_num, out->gid_index, &out->gid)) {
    LogFail("ibv_query_gid", __FILE__, __LINE__); return false;
  }
  return true;
}

void CloseDevice(DeviceCtx* d) {
  if (!d) return;
  if (d->pd) ibv_dealloc_pd(d->pd);
  if (d->ctx) ibv_close_device(d->ctx);
  *d = DeviceCtx{};
}

// ---- MR ------------------------------------------------------------------------------------------
bool RegisterMr(ibv_pd* pd, void* buf, size_t len, Mr* out) {
  int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_REMOTE_ATOMIC;
  out->mr = ibv_reg_mr(pd, buf, len, access);
  if (!out->mr) { LogFail("ibv_reg_mr", __FILE__, __LINE__); return false; }
  out->addr = buf;
  out->len = len;
  return true;
}
void DeregisterMr(Mr* m) { if (m && m->mr) { ibv_dereg_mr(m->mr); *m = Mr{}; } }

// ---- QP ------------------------------------------------------------------------------------------
static bool CreateRcQpCommon(DeviceCtx* dev, ibv_cq* cq, int max_wr, uint32_t psn_seed, RcQp* out) {
  out->dev = dev;
  out->cq = cq;
  ibv_qp_init_attr ia{};
  ia.send_cq = out->cq;
  ia.recv_cq = out->cq;
  ia.qp_type = IBV_QPT_RC;
  ia.sq_sig_all = 0;
  ia.cap.max_send_wr = max_wr;
  ia.cap.max_recv_wr = max_wr;
  ia.cap.max_send_sge = 1;
  ia.cap.max_recv_sge = 1;
  out->qp = ibv_create_qp(dev->pd, &ia);
  if (!out->qp) { LogFail("ibv_create_qp", __FILE__, __LINE__); return false; }
  out->local_psn = psn_seed & 0xffffff;

  ibv_qp_attr a{};
  a.qp_state = IBV_QPS_INIT;
  a.pkey_index = 0;
  a.port_num = dev->port_num;
  a.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
                      IBV_ACCESS_REMOTE_ATOMIC;
  if (ibv_modify_qp(out->qp, &a,
                    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
    LogFail("modify_qp INIT", __FILE__, __LINE__); return false;
  }
  return true;
}

bool CreateRcQp(DeviceCtx* dev, int cqe, int max_wr, uint32_t psn_seed, RcQp* out) {
  ibv_cq* cq = ibv_create_cq(dev->ctx, cqe, nullptr, nullptr, 0);
  if (!cq) { LogFail("ibv_create_cq", __FILE__, __LINE__); return false; }
  out->owns_cq = true;
  if (!CreateRcQpCommon(dev, cq, max_wr, psn_seed, out)) { ibv_destroy_cq(cq); return false; }
  return true;
}

bool CreateRcQpOnSharedCq(DeviceCtx* dev, ibv_cq* shared_cq, int max_wr, uint32_t psn_seed, RcQp* out) {
  out->owns_cq = false;
  return CreateRcQpCommon(dev, shared_cq, max_wr, psn_seed, out);
}

void DestroyRcQp(RcQp* q) {
  if (!q) return;
  if (q->qp) ibv_destroy_qp(q->qp);
  if (q->cq && q->owns_cq) ibv_destroy_cq(q->cq);
  *q = RcQp{};
}

QpEndpoint LocalEndpoint(const RcQp& q) {
  QpEndpoint e{};
  e.qpn = q.qp->qp_num;
  e.psn = q.local_psn;
  e.gid = q.dev->gid;
  e.lid = q.dev->port_attr.lid;  // 0 on RoCE
  e.mtu = static_cast<uint8_t>(q.dev->port_attr.active_mtu);
  return e;
}

bool ConnectRcQp(RcQp* q, const QpEndpoint& remote) {
  // INIT -> RTR
  ibv_qp_attr a{};
  a.qp_state = IBV_QPS_RTR;
  // Negotiate path MTU to min(local active, remote active) — see QpEndpoint::mtu. Setting the
  // local active MTU unilaterally (the previous behavior) is only correct while both ports
  // happen to agree; on divergence the responder NAKs multi-packet WRITEs with REM_INV_REQ.
  a.path_mtu = q->dev->port_attr.active_mtu;
  if (remote.mtu != 0 && static_cast<ibv_mtu>(remote.mtu) < a.path_mtu) {
    a.path_mtu = static_cast<ibv_mtu>(remote.mtu);
  }
  if (remote.mtu != 0 && static_cast<ibv_mtu>(remote.mtu) != q->dev->port_attr.active_mtu) {
    fprintf(stderr, "[rdma] path_mtu negotiated to %d (local active=%d, remote active=%d)\n",
            a.path_mtu, q->dev->port_attr.active_mtu, remote.mtu);
  }
  a.dest_qp_num = remote.qpn;
  a.rq_psn = remote.psn;
  a.max_dest_rd_atomic = 16;
  a.min_rnr_timer = 12;
  a.ah_attr.is_global = 1;            // RoCE requires GRH
  a.ah_attr.dlid = 0;                 // unused on RoCE
  a.ah_attr.sl = 0;
  a.ah_attr.src_path_bits = 0;
  a.ah_attr.port_num = q->dev->port_num;
  a.ah_attr.grh.dgid = remote.gid;
  a.ah_attr.grh.sgid_index = static_cast<uint8_t>(q->dev->gid_index);
  a.ah_attr.grh.hop_limit = 1;
  a.ah_attr.grh.traffic_class = 0;
  if (ibv_modify_qp(q->qp, &a,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
    LogFail("modify_qp RTR", __FILE__, __LINE__); return false;
  }
  // RTR -> RTS
  ibv_qp_attr r{};
  r.qp_state = IBV_QPS_RTS;
  r.timeout = 14;
  r.retry_cnt = 7;
  r.rnr_retry = 7;
  r.sq_psn = q->local_psn;
  r.max_rd_atomic = 16;
  if (ibv_modify_qp(q->qp, &r,
                    IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                        IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
    LogFail("modify_qp RTS", __FILE__, __LINE__); return false;
  }
  return true;
}

// ---- one-sided ops -------------------------------------------------------------------------------
static bool PostOne(RcQp* q, ibv_send_wr* wr) {
  ibv_send_wr* bad = nullptr;
  if (ibv_post_send(q->qp, wr, &bad)) { LogFail("ibv_post_send", __FILE__, __LINE__); return false; }
  return true;
}

bool PostRead(RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey, uint32_t len,
              uint64_t wr_id) {
  ibv_sge sge{reinterpret_cast<uint64_t>(laddr), len, lkey};
  ibv_send_wr wr{};
  wr.wr_id = wr_id;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_READ;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = raddr;
  wr.wr.rdma.rkey = rkey;
  return PostOne(q, &wr);
}

bool PostWrite(RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey, uint32_t len,
               uint64_t wr_id) {
  ibv_sge sge{reinterpret_cast<uint64_t>(laddr), len, lkey};
  ibv_send_wr wr{};
  wr.wr_id = wr_id;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = raddr;
  wr.wr.rdma.rkey = rkey;
  return PostOne(q, &wr);
}

bool PostFetchAdd(RcQp* q, void* laddr, uint32_t lkey, uint64_t raddr, uint32_t rkey, uint64_t add,
                  uint64_t wr_id) {
  ibv_sge sge{reinterpret_cast<uint64_t>(laddr), 8, lkey};
  ibv_send_wr wr{};
  wr.wr_id = wr_id;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.atomic.remote_addr = raddr;  // must be 8-byte aligned
  wr.wr.atomic.rkey = rkey;
  wr.wr.atomic.compare_add = add;
  return PostOne(q, &wr);
}

int PollCq(ibv_cq* cq, ibv_wc* wc, int max, int* bad_status) {
  for (;;) {
    int n = ibv_poll_cq(cq, max, wc);
    if (n < 0) { if (bad_status) *bad_status = -1; return -1; }
    if (n == 0) continue;
    for (int i = 0; i < n; ++i)
      if (wc[i].status != IBV_WC_SUCCESS) {
        // Self-documenting failure: WC9 (REM_INV_REQ) here has been environment-sensitive
        // (path-MTU divergence class); capture everything needed to diagnose a one-off.
        fprintf(stderr,
                "[rdma] WC error: status=%d(%s) opcode=%d wr_id=%llu vendor_err=0x%x qp_num=%u\n",
                wc[i].status, ibv_wc_status_str(wc[i].status), wc[i].opcode,
                static_cast<unsigned long long>(wc[i].wr_id), wc[i].vendor_err, wc[i].qp_num);
        if (bad_status) *bad_status = wc[i].status;
        return -1;
      }
    return n;
  }
}

// ---- OOB TCP -------------------------------------------------------------------------------------
static bool WriteAll(int fd, const void* p, size_t n) {
  const char* b = static_cast<const char*>(p);
  while (n) { ssize_t w = ::write(fd, b, n); if (w <= 0) return false; b += w; n -= w; }
  return true;
}
static bool ReadAll(int fd, void* p, size_t n) {
  char* b = static_cast<char*>(p);
  while (n) { ssize_t r = ::read(fd, b, n); if (r <= 0) return false; b += r; n -= r; }
  return true;
}

bool OobServerExchange(int port, const void* send, void* recv, size_t n) {
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) { LogFail("socket", __FILE__, __LINE__); return false; }
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = INADDR_ANY;
  a.sin_port = htons(port);
  if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) || listen(ls, 1)) {
    LogFail("bind/listen", __FILE__, __LINE__); close(ls); return false;
  }
  // Bounded accept: if no client connects within the timeout, fail instead of blocking forever
  // (a hung server would hold the exclusive testbed flock — see handoff post-mortem).
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(ls, &rfds);
  timeval tv{30, 0};  // 30 s
  int sel = select(ls + 1, &rfds, nullptr, nullptr, &tv);
  if (sel <= 0) { LogFail("accept timeout/select", __FILE__, __LINE__); close(ls); return false; }
  int fd = accept(ls, nullptr, nullptr);
  close(ls);
  if (fd < 0) { LogFail("accept", __FILE__, __LINE__); return false; }
  bool ok = WriteAll(fd, send, n) && ReadAll(fd, recv, n);
  close(fd);
  return ok;
}

bool OobClientExchange(const std::string& ip, int port, const void* send, void* recv, size_t n) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { LogFail("socket", __FILE__, __LINE__); return false; }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
    LogFail("inet_pton", __FILE__, __LINE__); close(fd); return false;
  }
  // retry connect briefly so client/server start order doesn't matter
  int tries = 0;
  while (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a))) {
    if (++tries > 100) { LogFail("connect", __FILE__, __LINE__); close(fd); return false; }
    usleep(100000);
  }
  // Server writes-then-reads, so client reads-then-writes (avoids deadlock on small kernel buffers).
  bool ok = ReadAll(fd, recv, n) && WriteAll(fd, send, n);
  close(fd);
  return ok;
}

}  // namespace embarcadero::rdma
