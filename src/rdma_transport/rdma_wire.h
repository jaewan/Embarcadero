// rdma_transport/rdma_wire.h — protocol-faithful wire structures for the W5-A/W5-B measurement
// harness (E4e / E1-leg3 / E6, docs/design/W5_RDMA_VARIANTS_SPEC.md).
//
// These are NOT the real Embarcadero types (owned by Track 01 in
// src/cxl_manager/cxl_datastructure.h and src/embarlet/topic.{cc,h} — we never edit those files,
// per the spec's boundary rule). They are a byte-compatible MIRROR of the verified offsets (spec
// Appendix A, `offsetof`-checked): batch_complete@16, pbr_absolute_index@48, total_size@56,
// start_logical_offset@64, log_idx@72, total_order@80, batch_off_to_export@88, publish_commit@96,
// sizeof==128, broker-owned body [0,80). Reproducing the layout + the sentinel-last discipline is
// what makes this a transport-swap measurement (same invariants, same torn-window analysis) rather
// than an unrelated microbenchmark (spec §1, §3.3).
//
// The ordering/business logic here (batch_seq assignment, total_order minting) is intentionally
// minimal — a monotonic counter, same spirit as Track 03's Corfu/Scalog/LazyLog mailbox benches
// (src/cxl_manager/*_mailbox_bench.cc): faithful to the mechanical protocol (byte layout, sentinel
// discipline, RC same-QP ordering, poll cadence) under real cross-host RDMA, not a reimplementation
// of Embarcadero's session/dedup/hold-buffer semantics (which is D1's job over CXL, not this
// track's job over RDMA).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "rdma_transport/rdma_common.h"

namespace embarcadero::rdma_variant {

// ---- BatchHeader mirror (spec Appendix A / cxl_datastructure.h:393) ------------------------------
// Broker-owned body = [0,80): batch_seq/client_id/broker_id/batch_complete (identity + readiness),
// pbr_absolute_index/total_size/start_logical_offset/log_idx (payload locator). Sequencer-owned:
// ordered@24, total_order@80, batch_off_to_export@88. Sentinel: publish_commit@96, written LAST by
// a SEPARATE WRITE on the same RC QP as the body (spec §3.3 C2) — never folded into the body WRITE
// (a single WRITE's trailing word can land out of order internally; correctness needs a distinct,
// later, same-QP WRITE, spec's "why separate").
struct alignas(64) BatchHeaderMirror {
  uint64_t batch_seq;               // @0   broker-owned
  uint32_t client_id;                // @8   broker-owned
  uint32_t broker_id;                // @12  broker-owned
  uint32_t batch_complete;           // @16  broker-owned (readiness half of the sentinel gate)
  uint32_t _pad0;                    // @20
  uint32_t ordered;                  // @24  sequencer-owned
  uint32_t _pad1;                    // @28
  uint8_t  _reserved0[16];           // @32..47 (unused by this harness; reserved to preserve offsets)
  uint64_t pbr_absolute_index;       // @48  broker-owned
  uint64_t total_size;               // @56  broker-owned (Blog payload byte length)
  uint64_t start_logical_offset;     // @64  broker-owned
  uint64_t log_idx;                  // @72  broker-owned (Blog payload locator)
  uint64_t total_order;              // @80  sequencer-owned
  uint64_t batch_off_to_export;      // @88  sequencer-owned (unused by this harness)
  uint64_t publish_commit;           // @96  SENTINEL — written last, separate WRITE, same QP
  uint8_t  _tail[24];                // @104..127
};
static_assert(sizeof(BatchHeaderMirror) == 128, "must mirror the real BatchHeader size (spec App. A)");
static_assert(offsetof(BatchHeaderMirror, batch_complete) == 16, "offset drift vs spec Appendix A");
static_assert(offsetof(BatchHeaderMirror, pbr_absolute_index) == 48, "offset drift vs spec Appendix A");
static_assert(offsetof(BatchHeaderMirror, total_size) == 56, "offset drift vs spec Appendix A");
static_assert(offsetof(BatchHeaderMirror, start_logical_offset) == 64, "offset drift vs spec Appendix A");
static_assert(offsetof(BatchHeaderMirror, log_idx) == 72, "offset drift vs spec Appendix A");
static_assert(offsetof(BatchHeaderMirror, total_order) == 80, "offset drift vs spec Appendix A");
static_assert(offsetof(BatchHeaderMirror, batch_off_to_export) == 88, "offset drift vs spec Appendix A");
static_assert(offsetof(BatchHeaderMirror, publish_commit) == 96, "offset drift vs spec Appendix A");

constexpr uint64_t kPublishUncommitted = std::numeric_limits<uint64_t>::max();

// Mirrors BatchHeaderPublishCommitted (cxl_datastructure.h:432) exactly: never trust body fields
// until the sentinel matches AND batch_complete==1 (torn-window safety, spec §3.3 step 3).
inline bool HeaderPublishCommitted(const BatchHeaderMirror& h) {
  return h.publish_commit != kPublishUncommitted &&
         h.publish_commit == h.pbr_absolute_index &&
         h.batch_complete == 1;
}

// ---- ControlBlock mirror (epoch / lease / sequencer id / committed_seq) -------------------------
struct alignas(64) ControlBlockMirror {
  uint64_t epoch;               // sequencer generation; bumped on failover (not exercised here)
  uint64_t sequencer_lease_ns;   // absolute expiry (CLOCK_MONOTONIC ns), single-writer = sequencer
  uint32_t sequencer_id;
  uint32_t _pad0;
  uint64_t committed_seq;        // monotone GOI frontier
  uint8_t  _pad1[32];
};
static_assert(sizeof(ControlBlockMirror) == 64, "ControlBlockMirror must stay 64B (spec §2.1)");

// ---- CompletionVector entry mirror (one per broker; sequencer writes, owning broker reads — M1) --
struct alignas(64) CompletionVectorEntryMirror {
  uint64_t ack_offset;   // durable/ordered frontier for this broker (message count analog)
  uint8_t  _pad[56];
};
static_assert(sizeof(CompletionVectorEntryMirror) == 64, "CV entry must stay 64B");

// ---- Packed sentinel array slot (E2) -------------------------------------------------------------
// Contiguous per-broker publish_commit mirrors; each broker WRITEs only its own slot; the
// sequencer RDMA-READs the WHOLE array in ONE contiguous op (spec §3.3 step 1). A gather READ over
// the per-broker PBR *rings* is impossible (an RDMA READ's SGE scatters into LOCAL buffers only;
// its remote side is a single contiguous range) — this array is the fix (spec E2).
struct SentinelSlot {
  uint64_t value;   // mirrors this broker's current-slot publish_commit; kPublishUncommitted = none
};
static_assert(sizeof(SentinelSlot) == 8, "sentinel array slot must be 8B for a tight contiguous READ");

// ---- GOI entry mirror (global order index; sequencer single-writer) -----------------------------
struct alignas(64) GoiEntryMirror {
  uint64_t total_order;
  uint64_t batch_seq;
  uint32_t client_id;
  uint32_t broker_id;
  uint64_t log_idx;
  uint64_t total_size;
  uint8_t  _pad[24];
};
static_assert(sizeof(GoiEntryMirror) == 64, "GOI entry must stay 64B");

// ---- Region layout helpers ------------------------------------------------------------------------
// A memserver-hosted metadata region for N brokers with K PBR slots each. Byte offsets from the
// region base; all regions are individually MR-registered (spec §2.1 table) so remote_addr/rkey are
// per-region, not per-field.
struct MemserverLayout {
  size_t num_brokers = 0;
  size_t pbr_slots_per_broker = 0;
  size_t goi_entries = 0;

  size_t control_block_bytes() const { return sizeof(ControlBlockMirror); }
  size_t completion_vector_bytes() const { return num_brokers * sizeof(CompletionVectorEntryMirror); }
  size_t pbr_ring_bytes_per_broker() const { return pbr_slots_per_broker * sizeof(BatchHeaderMirror); }
  size_t pbr_ring_bytes_total() const { return num_brokers * pbr_ring_bytes_per_broker(); }
  size_t sentinel_array_bytes() const { return num_brokers * sizeof(SentinelSlot); }
  size_t goi_bytes() const { return goi_entries * sizeof(GoiEntryMirror); }
};

// ---- Memserver <-> broker/sequencer handshake blob -----------------------------------------------
// ONE combined blob for the entire OOB exchange (a single TCP connect/accept — OobServerExchange/
// OobClientExchange trade equal-size blobs in ONE call, so hello + region-descriptors must be one
// struct, not two calls). Client fills the `role`/`broker_id`/`client_ep` half before sending;
// server fills the `server_ep`/region-descriptor half before sending; each side only reads the
// half it received. SHARED here (not duplicated per binary) after a real bug: an earlier
// per-binary copy of this struct let the sequencer's GOI-address modulus silently disagree with
// the memserver's actual --goi-entries, causing an out-of-bounds RDMA WRITE (REM_ACCESS_ERR) the
// moment committed_seq exceeded the hardcoded constant. One definition removes that class of bug.
struct HandshakeBlob {
  // client -> server
  int32_t role;       // 0 = broker/producer, 1 = sequencer
  int32_t broker_id;  // valid when role==0
  embarcadero::rdma::QpEndpoint client_ep;
  // server -> client
  embarcadero::rdma::QpEndpoint server_ep;
  embarcadero::rdma::RegionDesc control;
  embarcadero::rdma::RegionDesc cv;        // whole array; client indexes by broker_id * sizeof(CompletionVectorEntryMirror)
  embarcadero::rdma::RegionDesc sentinel;  // whole array; client indexes by broker_id * sizeof(SentinelSlot)
  embarcadero::rdma::RegionDesc goi;
  embarcadero::rdma::RegionDesc pbr;       // whole combined region; client slices by broker_id * pbr_ring_bytes_per_broker
  embarcadero::rdma::RegionDesc blog;      // valid only if host_blog; same per-broker slicing
  uint32_t pbr_ring_bytes_per_broker;
  uint32_t blog_bytes_per_broker;
  uint32_t host_blog;      // 1 if this memserver hosts Blog (W5-B), 0 otherwise (W5-A metadata-only)
  uint32_t goi_entries;    // authoritative GOI ring capacity — sequencer MUST use this, not a guess
};

// ---- Broker-direct Blog handoff (W5-A only) ------------------------------------------------------
// In W5-A the payload Blog lives on each broker's own DRAM; the sequencer must open a SEPARATE RC
// connection directly to that broker (not via the memserver) to RDMA-READ payload. This is the
// tiny blob exchanged for that one connection (broker = server, sequencer = client).
struct BlogHandoffBlob {
  embarcadero::rdma::QpEndpoint ep;
  embarcadero::rdma::RegionDesc blog;
};

}  // namespace embarcadero::rdma_variant
