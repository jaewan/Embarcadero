#include "cxl_manager/scalog_global_ordering_core.h"

#include <glog/logging.h>

#include <algorithm>
#include <limits>

namespace Embarcadero {
namespace cxl_manager {

void ScalogGlobalOrderingCore::RegisterBroker(int broker_id, int replication_factor) {
	// Broker 0 carries the cluster replication factor (matches gRPC HandleRegisterBroker,
	// which sets num_replicas_per_broker_ from broker 0's request).
	if (broker_id == 0) {
		num_replicas_per_broker_ = replication_factor;
	}
	registered_brokers_.insert(broker_id);
}

void ScalogGlobalOrderingCore::AddLocalCut(int broker_id, int replica_id, int64_t epoch,
		int64_t local_cut) {
	// Verbatim from ScalogGlobalSequencer::ReceiveLocalCut: accumulate the delta since the
	// replica's last report into cumulative_cut_, track the last raw offset in
	// logical_offsets_, and reject a regressing local cut (log WARNING, skip).
	int64_t previous_cut = 0;
	auto broker_it = logical_offsets_.find(broker_id);
	if (broker_it != logical_offsets_.end()) {
		auto replica_it = broker_it->second.find(replica_id);
		if (replica_it != broker_it->second.end()) {
			previous_cut = replica_it->second;
		}
	}
	if (local_cut < previous_cut) {
		LOG(WARNING) << "Scalog global sequencer ignoring regressing local cut broker="
		             << broker_id << " replica=" << replica_id
		             << " previous=" << previous_cut << " current=" << local_cut;
		return;
	}
	cumulative_cut_[broker_id][replica_id] += (local_cut - previous_cut);
	logical_offsets_[broker_id][replica_id] = local_cut;
	// Track the epoch this replica is now at, for the OPT-IN per-epoch barrier in
	// ComputeGlobalCut. Never let it regress (defensive: epochs are monotonic per replica on a
	// FIFO up-ring, but a stale/duplicate must not lower the closed epoch and stall the
	// barrier). Does not affect the accumulated cut value.
	int64_t& tracked_epoch = last_epoch_[broker_id][replica_id];
	tracked_epoch = std::max(tracked_epoch, epoch);
	if ((epoch % 1000) == 0) {
		LOG(INFO) << "Scalog global sequencer received local cut broker=" << broker_id
		          << " replica=" << replica_id
		          << " epoch=" << epoch
		          << " local_cut=" << local_cut
		          << " delta=" << (local_cut - previous_cut);
	}
}

bool ScalogGlobalOrderingCore::ComputeGlobalCut(int expected_brokers,
		absl::btree_map<int, int64_t>& out_cut_by_broker,
		int64_t* out_epoch,
		bool use_epoch_barrier) {
	// Readiness gate (verbatim from SendGlobalCut, used by BOTH transports): every expected
	// broker must have reported every expected replica stream. Each broker always contributes
	// at least one stream (replica_id=0) even when replication_factor=0, so waiting for exactly
	// rf streams would block RF=0 runs forever — floor the expectation at 1 per broker. This is
	// the ENTIRE readiness the gRPC baseline (E1 leg 1) uses — its behaviour is unchanged.
	size_t total_num_replicas = 0;
	for (const auto& [broker_id, replica_map] : cumulative_cut_) {
		total_num_replicas += replica_map.size();
	}
	const int streams_per_broker = std::max(1, num_replicas_per_broker_);
	const size_t expected_replicas =
			static_cast<size_t>(expected_brokers) * static_cast<size_t>(streams_per_broker);
	if (total_num_replicas != expected_replicas) {
		return false;  // durability coupling: not all replicas have reported yet
	}

	// OPT-IN per-epoch barrier (mailbox path only; the gRPC baseline passes false and skips
	// this entirely, preserving its count-only cadence). The poll-driven mailbox sequencer is
	// not message-driven, so the count above stays permanently satisfied after warmup and
	// cannot by itself tell that a fresh per-epoch report round has completed. Close the epoch
	// E = MIN over all reporting replicas of their latest reported epoch and emit only when E
	// ADVANCES past the last emitted epoch, so each completed round broadcasts exactly once.
	// This is a CADENCE/tagging mechanism: it changes *when* a cut is emitted, never the
	// min-across-replicas VALUES nor the durability coupling below (no per-tick cap).
	int64_t aligned_epoch = -1;
	if (use_epoch_barrier) {
		int64_t min_epoch = std::numeric_limits<int64_t>::max();
		bool have_epoch = false;
		for (const auto& [broker_id, replica_map] : last_epoch_) {
			for (const auto& [replica_id, ep] : replica_map) {
				min_epoch = std::min(min_epoch, ep);
				have_epoch = true;
			}
		}
		if (!have_epoch || min_epoch <= last_emitted_epoch_) {
			return false;  // no new fully-reported epoch since the last broadcast
		}
		aligned_epoch = min_epoch;
	}

	// MIN across replicas per broker (the durable-prefix decision), verbatim from
	// SendGlobalCut lines 139-153: empty replica set -> contribute 0; fewer than rf
	// replicas -> contribute 0; otherwise the element-wise minimum accumulated cut.
	out_cut_by_broker.clear();
	for (const auto& entry : cumulative_cut_) {
		if (entry.second.empty()) {
			out_cut_by_broker.insert({entry.first, 0});
			continue;
		}

		size_t num_replicas = entry.second.size();
		if (static_cast<int>(num_replicas) < num_replicas_per_broker_) {
			out_cut_by_broker.insert({entry.first, 0});
			continue;
		}

		auto min_entry = std::min_element(entry.second.begin(), entry.second.end(),
				[](const auto& a, const auto& b) {
					return a.second < b.second;
				});
		out_cut_by_broker.insert({entry.first, min_entry->second});
	}
	if (use_epoch_barrier) {
		last_emitted_epoch_ = aligned_epoch;  // this epoch is now broadcast; don't re-emit it
	}
	if (out_epoch != nullptr) {
		*out_epoch = aligned_epoch;  // E when the barrier is on; -1 (unused) for count-only gRPC
	}
	return true;
}

}  // namespace cxl_manager
}  // namespace Embarcadero
