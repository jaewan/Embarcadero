#pragma once

// Scalog CXL mailbox PODs — the fixed-layout records the Scalog cut/report protocol rides
// on once the transport is swapped from the gRPC bidi stream to the CXL mailbox
// (docs/baselines/porting_rule.md). These are a pure re-encoding of the proto LocalCut /
// GlobalCut messages: same fields, same per-epoch report/aggregate/broadcast pattern, no
// protocol change. Plain POD with fixed layout and NO pointers, because the bytes cross the
// (non-coherent) CXL link and may be mapped at different virtual addresses in the local
// sequencer (broker) and the co-located global sequencer.

#include <cstdint>

namespace Embarcadero {
namespace cxl_manager {

// Max brokers addressable in a GlobalCut. Matches the broker_id range used elsewhere in the
// stack (CorfuSequencerImpl::kMaxBrokers == 32).
inline constexpr int kMaxBrokers = 32;

// Local sequencer (broker) -> global sequencer, posted on up(broker_id).
// Mirrors proto LocalCut{int64 local_cut, string topic, int64 broker_id, int64 epoch,
// int64 replica_id}.
//
// topic is a string in the proto; for the sequencer-core port we carry a uint32 topic_id.
// The single-topic bench uses id 0. A real multi-topic integration maps a topic NAME -> id
// at configuration time (a stable per-cluster mapping all participants agree on) — the wire
// format here is deliberately topic-id-based so the record stays a fixed POD; the name->id
// mapping is an application-layer concern, not part of this transport.
struct alignas(8) ScalogLocalCutMsg {
	int64_t local_cut;   // replica's current cumulative logical offset (proto local_cut)
	int64_t epoch;       // epoch number (proto epoch)
	uint32_t topic_id;   // topic id (single-topic bench: 0; proto string re-encoded)
	int32_t broker_id;   // owning broker (proto broker_id, narrowed)
	int32_t replica_id;  // replica index within the broker's replication set (proto replica_id)
	uint32_t pad;        // explicit padding: fixed 32-byte layout, no implicit tail padding
};
static_assert(sizeof(ScalogLocalCutMsg) == 32,
		"ScalogLocalCutMsg must be 32 bytes (fixed CXL layout)");

// Global sequencer -> broker, broadcast to every down(b). Mirrors proto
// GlobalCut{map<int64,int64> global_cut}.
//
// The proto map<broker_id, cut> is encoded as a FIXED array indexed by broker_id, up to
// kMaxBrokers. Absent brokers use the sentinel -1 (proto has no entry; the reader skips a
// -1). epoch is carried for correlation ("this cut is for epoch X"); the proto has no epoch
// on GlobalCut, so this is transport-local metadata that the reader may use to detect
// duplicate/reordered broadcasts (it does not change the ordering decision).
struct alignas(8) ScalogGlobalCutMsg {
	int64_t epoch;                 // epoch this cut applies to (transport correlation)
	uint32_t num_brokers;          // number of brokers in the cluster
	uint32_t pad0;                 // padding for 8-byte alignment of cut[]
	int64_t cut[kMaxBrokers];      // per-broker MIN cut (the durable prefix); -1 = absent
};
// 8 (epoch) + 4 (num_brokers) + 4 (pad0) + 32*8 (cut) = 272 bytes.
static_assert(sizeof(ScalogGlobalCutMsg) == 8 + 8 + kMaxBrokers * 8,
		"ScalogGlobalCutMsg must be 272 bytes (fixed CXL layout)");
static_assert(sizeof(ScalogGlobalCutMsg) == 272,
		"ScalogGlobalCutMsg size drift — pick MailboxParams.record_size >= 512");

// Sentinel value written into GlobalCutMsg.cut[b] for brokers that have no cut this epoch.
inline constexpr int64_t kAbsentCut = -1;

}  // namespace cxl_manager
}  // namespace Embarcadero
