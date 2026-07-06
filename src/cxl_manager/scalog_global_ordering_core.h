#pragma once

// ScalogGlobalOrderingCore — the transport-independent Scalog global-cut state machine.
//
// This is the Scalog analogue of Corfu's CorfuSequencerImpl::AssignToken extraction
// (docs/baselines/porting_rule.md): the min-across-replicas-per-shard decision function
// (the durable-prefix computation) and the broker-registration / per-replica cut
// bookkeeping are lifted VERBATIM out of ScalogGlobalSequencer so that BOTH the gRPC
// baseline path (ScalogGlobalSequencer::SendGlobalCut / ReceiveLocalCut /
// HandleRegisterBroker) and the CXL mailbox path (ScalogMailboxSequencer) call literally
// the same ordering code. No ordering logic is duplicated or altered — only the wire
// changes (gRPC bidi stream -> CXL mailbox up/down rings).
//
// State (identical to the gRPC baseline's members it replaces):
//   * cumulative_cut_[broker][replica]  == the old global_cut_  (accumulated deltas)
//   * logical_offsets_[broker][replica] == the old logical_offsets_ (last raw local_cut)
//   * registered_brokers_ / num_replicas_per_broker_
// The MIN-across-replicas-per-broker decision (SendGlobalCut lines 139-153 of the
// baseline) is reproduced bit-for-bit in ComputeGlobalCut, including the "empty ->
// contribute 0" and "fewer than rf replicas -> contribute 0" fallbacks and the
// regressing-cut rejection in AddLocalCut.
//
// THREAD CONTRACT: this core has NO internal locks. It is shared mutable state and the
// caller MUST provide external synchronization:
//   * gRPC path: all core calls are made under ScalogGlobalSequencer::global_cut_mu_
//     (exactly where the inline logic used to run).
//   * mailbox path: the single ScalogMailboxSequencer poll thread is the sole caller
//     (SPSC), so no lock is needed.
// Calling from multiple threads without external synchronization WILL corrupt the maps.

#include <cstdint>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"

namespace Embarcadero {
namespace cxl_manager {

class ScalogGlobalOrderingCore {
 public:
	explicit ScalogGlobalOrderingCore(int num_replicas_per_broker = 1)
			: num_replicas_per_broker_(num_replicas_per_broker) {}

	// Register a broker (once at startup, idempotent per broker_id). Mirrors the gRPC
	// HandleRegisterBroker: broker 0 also carries the cluster replication factor.
	void RegisterBroker(int broker_id, int replication_factor);

	// Record a replica's local cut. `local_cut` is the replica's current cumulative
	// logical offset (as in the proto LocalCut.local_cut). Accumulates the delta since the
	// replica's last report into cumulative_cut_ and rejects regressions (logs WARNING and
	// skips), exactly as the gRPC ReceiveLocalCut did. `epoch` is used only for the same
	// periodic logging cadence as the baseline.
	void AddLocalCut(int broker_id, int replica_id, int64_t epoch, int64_t local_cut);

	// Compute the per-broker global cut = MIN across that broker's replicas of the
	// accumulated cut (the durable prefix). Fills out_cut_by_broker with {broker_id ->
	// cut}. Returns true iff the aggregate is "ready" and leaves out_cut_by_broker
	// untouched otherwise.
	//
	// Readiness (the durability coupling, IDENTICAL for both transports): every expected
	// replica across every expected broker must have reported before any cut is emitted (the
	// cut is MIN across ALL replicas per shard; no per-tick cap). This count gate is exactly
	// the gRPC baseline's original `total_num_replicas == expected_replicas` check — the gRPC
	// path (E1 leg 1) uses ONLY this, so its behaviour is preserved bit-for-bit.
	//
	// use_epoch_barrier (OPT-IN, mailbox path only; default OFF): the poll-driven mailbox
	// sequencer is not message-driven like the gRPC stream, so it needs to detect when a fresh
	// per-epoch report round has completed. When true, ComputeGlobalCut additionally returns a
	// cut only once the closed epoch E = MIN over replicas of their latest reported epoch
	// ADVANCES past the last emitted epoch, and reports E via out_epoch (the broadcast
	// correlation tag). This is a transport-level CADENCE mechanism: it changes *when* a cut is
	// emitted, never the min-across-replicas VALUES nor the durability coupling. The gRPC
	// baseline passes false (readiness unchanged); only the mailbox port passes true.
	//
	// expected_brokers: number of brokers the cluster expects to report (the gRPC path
	// passes ExpectedScalogBrokerCount(); the mailbox path passes num_brokers()).
	bool ComputeGlobalCut(int expected_brokers,
			absl::btree_map<int, int64_t>& out_cut_by_broker,
			int64_t* out_epoch = nullptr,
			bool use_epoch_barrier = false);

	int num_replicas_per_broker() const { return num_replicas_per_broker_; }
	int num_registered_brokers() const {
		return static_cast<int>(registered_brokers_.size());
	}

 private:
	// broker_id -> replica_id -> accumulated logical offset (the durable-prefix candidate).
	// Was ScalogGlobalSequencer::global_cut_.
	absl::btree_map<int, absl::btree_map<int, int64_t>> cumulative_cut_;

	// broker_id -> replica_id -> last raw local_cut reported (to compute deltas / detect
	// regressions). Was ScalogGlobalSequencer::logical_offsets_.
	absl::btree_map<int, absl::btree_map<int, int64_t>> logical_offsets_;

	// broker_id -> replica_id -> last epoch this replica reported. Used only by the epoch
	// barrier in ComputeGlobalCut to close E = MIN over replicas of their latest epoch; it
	// does not touch the cut VALUES (the min-across-replicas decision is unchanged).
	absl::btree_map<int, absl::btree_map<int, int64_t>> last_epoch_;

	// Registered broker ids. Was ScalogGlobalSequencer::registered_brokers_.
	absl::btree_set<int> registered_brokers_;

	// Replication factor (streams per broker), captured from broker 0's RegisterBroker.
	int num_replicas_per_broker_ = 1;

	// Highest epoch already broadcast by ComputeGlobalCut. The per-epoch barrier emits only
	// when the closed epoch (min across replicas' latest epoch) advances past this, so each
	// completed report round is broadcast exactly once. Starts below epoch 0 (which is valid).
	int64_t last_emitted_epoch_ = -1;
};

}  // namespace cxl_manager
}  // namespace Embarcadero
