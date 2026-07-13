#pragma once

// LazyLog's append contract is intentionally distinct from its background
// ordering/read-stability protocol.  An append is complete once the record
// data and the corresponding sequencing metadata have both reached their
// required durable replica sets; global binding may happen later.  Keep this
// boundary in one small helper so ACK plumbing cannot accidentally regain a
// dependency on the binding round.

namespace Embarcadero {

struct LazyLogAppendReplicationState {
	bool data_replicas_durable{false};
	bool metadata_replicas_durable{false};
	bool global_binding_received{false};
};

// Client-visible append completion.  Deliberately does not inspect
// global_binding_received: binding establishes the later total-order/read
// frontier, not append durability.
inline constexpr bool CanCompleteLazyLogAppend(
		const LazyLogAppendReplicationState& state) {
	return state.data_replicas_durable && state.metadata_replicas_durable;
}

// A record may be exposed through the ordered log only after binding.  This
// is separate from the append-completion predicate above.
inline constexpr bool CanExposeLazyLogOrderedRecord(
		const LazyLogAppendReplicationState& state) {
	return CanCompleteLazyLogAppend(state) && state.global_binding_received;
}

}  // namespace Embarcadero
