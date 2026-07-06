#pragma once

// LazyLog CXL mailbox PODs — the fixed-layout records the LazyLog binding-round protocol rides
// on once the transport is swapped from the gRPC bidi stream to the CXL mailbox
// (docs/baselines/porting_rule.md). These are a pure re-encoding of the proto LocalProgress /
// GlobalBinding messages: same fields, same per-round report/aggregate/broadcast pattern, no
// protocol change. Plain POD with fixed layout and NO pointers, because the bytes cross the
// (non-coherent) CXL link and may be mapped at different virtual addresses in the local
// sequencer (broker) and the co-located global sequencer.

#include <cstdint>

namespace Embarcadero {
namespace cxl_manager {

// Max brokers addressable in a GlobalBinding. Matches the broker_id range used elsewhere in the
// stack (Scalog's kMaxBrokers == 32, CorfuSequencerImpl::kMaxBrokers == 32).
inline constexpr int kMaxBrokers = 32;

// Local sequencer (broker) -> global sequencer, posted on up(broker_id).
// Mirrors proto LocalProgress{int64 local_progress, string topic, int64 broker_id, int64 epoch}.
//
// LazyLog's LocalProgress carries NO replica_id (unlike Scalog's LocalCut); the broker's local
// sequencer already reports the MIN-across-its-replicas durable frontier as a single
// local_progress value (SCALOG_LIMITATION.md §5c). So there is one cumulative progress value per
// broker, not per replica.
//
// topic is a string in the proto; for the sequencer-core port we carry a uint32 topic_id. The
// single-topic bench uses id 0. A real multi-topic integration maps a topic NAME -> id at
// configuration time (a stable per-cluster mapping all participants agree on) — the wire format
// here is deliberately topic-id-based so the record stays a fixed POD; the name->id mapping is an
// application-layer concern, not part of this transport.
struct alignas(8) LazyLogLocalProgressMsg {
	int64_t local_progress;  // broker's current cumulative durable frontier (proto local_progress)
	int64_t epoch;           // epoch number (proto epoch)
	uint32_t topic_id;       // topic id (single-topic bench: 0; proto string re-encoded)
	int32_t broker_id;       // owning broker (proto broker_id, narrowed)
	uint32_t pad;            // explicit padding: fixed 32-byte layout, no implicit tail padding
	uint32_t pad2;           // second pad word so alignas(8) needs no implicit tail padding
};
static_assert(sizeof(LazyLogLocalProgressMsg) == 32,
		"LazyLogLocalProgressMsg must be 32 bytes (fixed CXL layout)");

// Global sequencer -> broker, broadcast to every down(b). Mirrors proto
// GlobalBinding{map<int64,int64> global_binding}.
//
// The proto map<broker_id, delta> is encoded as a FIXED array indexed by broker_id, up to
// kMaxBrokers. Absent brokers (nothing newly bound this round) use the sentinel -1 (proto has no
// entry; the reader skips a -1). epoch is carried for correlation ("this binding is for epoch
// X"); the proto has no epoch on GlobalBinding, so this is transport-local metadata that the
// reader may use to detect duplicate/reordered broadcasts (it does not change the ordering
// decision).
struct alignas(8) LazyLogGlobalBindingMsg {
	int64_t epoch;                     // epoch this binding applies to (transport correlation)
	uint32_t num_brokers;              // number of brokers in the cluster
	uint32_t pad0;                     // padding for 8-byte alignment of binding[]
	int64_t binding[kMaxBrokers];      // per-broker delta (newly bound messages); -1 = absent
};
// 8 (epoch) + 4 (num_brokers) + 4 (pad0) + 32*8 (binding) = 272 bytes.
static_assert(sizeof(LazyLogGlobalBindingMsg) == 8 + 8 + kMaxBrokers * 8,
		"LazyLogGlobalBindingMsg must be 272 bytes (fixed CXL layout)");
static_assert(sizeof(LazyLogGlobalBindingMsg) == 272,
		"LazyLogGlobalBindingMsg size drift — pick MailboxParams.record_size >= 512");

// Sentinel value written into GlobalBindingMsg.binding[b] for brokers with no new binding this
// round.
inline constexpr int64_t kAbsentBinding = -1;

}  // namespace cxl_manager
}  // namespace Embarcadero
