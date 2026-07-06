#pragma once

// LazyLogBindingCore — the transport-independent LazyLog binding-round state machine.
//
// This is the LazyLog analogue of ScalogGlobalOrderingCore (docs/baselines/porting_rule.md):
// the per-broker binding-delta decision function and the broker-registration / cumulative
// progress bookkeeping are lifted VERBATIM out of LazyLogGlobalSequencer so that BOTH the
// gRPC baseline path (LazyLogGlobalSequencer::SendGlobalBinding / ReceiveLocalProgress /
// HandleRegisterBroker) and the CXL mailbox path (LazyLogMailboxSequencer) call literally the
// same ordering code. No ordering logic is duplicated or altered — only the wire changes
// (gRPC bidi stream -> CXL mailbox up/down rings).
//
// State (identical to the gRPC baseline's members it replaces):
//   * last_progress_[broker]  == the old last_progress_  (cumulative progress reported; the
//                                source of binding deltas — the raw durable frontier)
//   * bound_progress_[broker] == the old bound_progress_ (progress already bound, to compute
//                                delta each round)
//   * reported_brokers_       == the old reported_brokers_ (readiness gate: reported >= 1)
//   * registered_brokers_     == the old registered_brokers_ (readiness gate: registered >= 1)
// The per-broker binding-delta decision (SendGlobalBinding lines 177-191 of the baseline:
// available = reported - already_bound; bind_now = min(available, kMaxBindingsPerBrokerPerTick))
// is reproduced bit-for-bit in ComputeGlobalBinding, INCLUDING the no-cap policy
// (kMaxBindingsPerBrokerPerTick == max(), SCALOG_LIMITATION.md §5d) and the regressing-progress
// rejection in AddLocalProgress.
//
// LazyLog's coordinator tracks ONE cumulative progress value PER BROKER (its proto LocalProgress
// carries no replica_id; the broker's local sequencer is responsible for reporting the
// MIN-across-its-replicas durable frontier as its single local_progress — SCALOG_LIMITATION.md
// §5c). This core therefore does NOT track per-replica state; it mirrors the baseline's flat
// per-broker maps exactly.
//
// THREAD CONTRACT: this core has NO internal locks. It is shared mutable state and the caller
// MUST provide external synchronization:
//   * gRPC path: all core calls are made under LazyLogGlobalSequencer::progress_mu_ (exactly
//     where the inline logic used to run).
//   * mailbox path: the single LazyLogMailboxSequencer poll thread is the sole caller (SPSC),
//     so no lock is needed.
// Calling from multiple threads without external synchronization WILL corrupt the maps.

#include <cstdint>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"

namespace Embarcadero {
namespace cxl_manager {

class LazyLogBindingCore {
 public:
	LazyLogBindingCore() = default;

	// Register a broker (once at startup, idempotent per broker_id). Mirrors the gRPC
	// HandleRegisterBroker: LazyLog has no replication factor on the coordinator, so this is a
	// plain set insertion.
	void RegisterBroker(int broker_id);

	// Record a broker's reported local progress. `local_progress` is the broker's current
	// cumulative durable frontier (as in the proto LocalProgress.local_progress). Tracks the
	// latest cumulative value in last_progress_ and rejects regressions (logs WARNING and
	// skips), exactly as the gRPC ReceiveLocalProgress did. `epoch` is used only for the same
	// periodic logging cadence as the baseline and for the OPT-IN per-epoch barrier below.
	void AddLocalProgress(int broker_id, int64_t epoch, int64_t local_progress);

	// Compute the per-broker binding delta = newly available progress for this round
	// (reported - already_bound, uncapped). Fills out_binding_by_broker with {broker_id ->
	// delta}. Returns true iff the aggregate is "ready" and leaves out_binding_by_broker
	// untouched otherwise.
	//
	// Readiness (IDENTICAL for both transports): every expected broker must have registered and
	// reported progress at least once before any binding is emitted. This count gate is EXACTLY
	// the gRPC baseline's original `registered_snapshot.size() >= expected_brokers_ &&
	// reported_brokers_.size() >= expected_brokers_` check (SendGlobalBinding lines 174-175) —
	// the gRPC path (E1 leg 1) uses ONLY this, so its behaviour is preserved bit-for-bit.
	//
	// use_epoch_barrier (OPT-IN, mailbox path only; default OFF): the poll-driven mailbox
	// sequencer is not message-driven like the gRPC stream, so it needs to detect when a fresh
	// per-epoch report round has completed. When true, ComputeGlobalBinding additionally returns
	// a binding only once the closed epoch E = MIN over all brokers' latest reported epoch
	// ADVANCES past the last emitted epoch, and reports E via out_epoch (the broadcast
	// correlation tag). This is a transport-level CADENCE mechanism: it changes *when* a binding
	// is emitted, never the binding delta VALUES nor the min-across-replicas durability coupling.
	// The gRPC baseline passes false (readiness unchanged); only the mailbox port passes true.
	//
	// expected_brokers: number of brokers the cluster expects to report (the gRPC path passes
	// expected_brokers_; the mailbox path passes num_brokers()).
	bool ComputeGlobalBinding(int expected_brokers,
			absl::btree_map<int, int64_t>& out_binding_by_broker,
			int64_t* out_epoch = nullptr,
			bool use_epoch_barrier = false);

	int num_registered_brokers() const {
		return static_cast<int>(registered_brokers_.size());
	}

 private:
	// broker_id -> cumulative progress reported (the source of binding deltas). Was
	// LazyLogGlobalSequencer::last_progress_.
	absl::btree_map<int, int64_t> last_progress_;

	// broker_id -> progress already bound (to compute the delta each round). Was
	// LazyLogGlobalSequencer::bound_progress_.
	absl::btree_map<int, int64_t> bound_progress_;

	// Brokers that have reported at least once (readiness gate). Was
	// LazyLogGlobalSequencer::reported_brokers_.
	absl::btree_set<int> reported_brokers_;

	// broker_id -> latest epoch this broker reported. Used only by the OPT-IN epoch barrier in
	// ComputeGlobalBinding to close E = MIN over brokers of their latest epoch; it does not touch
	// the binding delta VALUES (the delta decision is unchanged).
	absl::btree_map<int, int64_t> last_epoch_;

	// Registered broker ids. Was LazyLogGlobalSequencer::registered_brokers_.
	absl::btree_set<int> registered_brokers_;

	// Highest epoch already broadcast by ComputeGlobalBinding. The per-epoch barrier emits only
	// when the closed epoch (MIN across brokers' latest epoch) advances past this, so each
	// completed report round is broadcast exactly once. Starts below epoch 0 (which is valid).
	int64_t last_emitted_epoch_ = -1;
};

}  // namespace cxl_manager
}  // namespace Embarcadero
