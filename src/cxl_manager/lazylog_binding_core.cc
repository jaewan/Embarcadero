#include "cxl_manager/lazylog_binding_core.h"

#include <glog/logging.h>

#include <algorithm>
#include <limits>

namespace Embarcadero {
namespace cxl_manager {

namespace {
// No artificial per-tick cap (SCALOG_LIMITATION.md §5d): the LazyLog gRPC baseline sets
// kMaxBindingsPerBrokerPerTick == std::numeric_limits<int64_t>::max() so every tick binds all
// available progress. Reproduced here VERBATIM so the extracted core binds the same amount the
// baseline does; do NOT reintroduce a finite cap.
constexpr int64_t kMaxBindingsPerBrokerPerTick = std::numeric_limits<int64_t>::max();
}  // namespace

void LazyLogBindingCore::RegisterBroker(int broker_id) {
	// LazyLog's coordinator has no replication factor (unlike Scalog); RegisterBroker is a plain
	// idempotent insertion, mirroring the gRPC HandleRegisterBroker.
	registered_brokers_.insert(broker_id);
}

void LazyLogBindingCore::AddLocalProgress(int broker_id, int64_t epoch, int64_t local_progress) {
	// Verbatim from LazyLogGlobalSequencer::ReceiveLocalProgress: track the latest cumulative
	// progress in last_progress_, mark the broker as having reported, and reject a regressing
	// local progress (the baseline silently `continue`s; we log WARNING and skip).
	int64_t previous = 0;
	auto it = last_progress_.find(broker_id);
	if (it != last_progress_.end()) {
		previous = it->second;
	}
	if (local_progress < previous) {
		LOG(WARNING) << "LazyLog binding core ignoring regressing local progress broker="
		             << broker_id << " previous=" << previous << " current=" << local_progress;
		return;
	}
	last_progress_[broker_id] = local_progress;
	reported_brokers_.insert(broker_id);

	// Track the epoch this broker is now at, for the OPT-IN per-epoch barrier in
	// ComputeGlobalBinding. Never let it regress (defensive: epochs are monotonic per broker on a
	// FIFO up-ring, but a stale/duplicate must not lower the closed epoch and stall the barrier).
	// Does not affect the binding delta value.
	int64_t& tracked_epoch = last_epoch_[broker_id];
	tracked_epoch = std::max(tracked_epoch, epoch);

	if ((epoch % 1000) == 0) {
		LOG(INFO) << "LazyLog binding core received local progress broker=" << broker_id
		          << " epoch=" << epoch << " local_progress=" << local_progress
		          << " delta=" << (local_progress - previous);
	}
}

bool LazyLogBindingCore::ComputeGlobalBinding(int expected_brokers,
		absl::btree_map<int, int64_t>& out_binding_by_broker,
		int64_t* out_epoch,
		bool use_epoch_barrier) {
	// Readiness gate (verbatim from SendGlobalBinding lines 174-175, used by BOTH transports):
	// LazyLog binding starts only after all expected brokers have registered AND reported progress
	// at least once. This is the ENTIRE readiness the gRPC baseline (E1 leg 1) uses — its
	// behaviour is unchanged bit-for-bit when use_epoch_barrier=false.
	if (static_cast<int>(registered_brokers_.size()) < expected_brokers ||
			static_cast<int>(reported_brokers_.size()) < expected_brokers) {
		return false;  // durability coupling: not all brokers have reported yet
	}

	// OPT-IN per-epoch barrier (mailbox path only; the gRPC baseline passes false and skips this
	// entirely, preserving its count-only cadence). The poll-driven mailbox sequencer is not
	// message-driven, so the count above stays permanently satisfied after warmup and cannot by
	// itself tell that a fresh per-epoch report round has completed. Close the epoch E = MIN over
	// all brokers of their latest reported epoch and emit only when E ADVANCES past the last
	// emitted epoch, so each completed round broadcasts exactly once. This is a CADENCE/tagging
	// mechanism: it changes *when* a binding is emitted, never the delta VALUES nor the durability
	// coupling below (no per-tick cap).
	int64_t aligned_epoch = -1;
	if (use_epoch_barrier) {
		int64_t min_epoch = std::numeric_limits<int64_t>::max();
		bool have_epoch = false;
		for (const auto& [broker_id, ep] : last_epoch_) {
			min_epoch = std::min(min_epoch, ep);
			have_epoch = true;
		}
		if (!have_epoch || min_epoch <= last_emitted_epoch_) {
			return false;  // no new fully-reported epoch since the last broadcast
		}
		aligned_epoch = min_epoch;
	}

	// Per-broker binding delta (the ordering decision), verbatim from SendGlobalBinding lines
	// 177-191: for each registered broker, available = reported - already_bound; bind_now =
	// min(available, kMaxBindingsPerBrokerPerTick == max()) — i.e. bind everything available, NO
	// artificial cap (§5d). Skip brokers with nothing new; advance bound_progress_ to reported.
	out_binding_by_broker.clear();
	for (int broker_id : registered_brokers_) {
		const int64_t reported = last_progress_[broker_id];
		const int64_t already_bound = bound_progress_[broker_id];
		if (reported <= already_bound) {
			continue;  // nothing new for this broker
		}

		const int64_t available = reported - already_bound;
		const int64_t bind_now = std::min<int64_t>(available, kMaxBindingsPerBrokerPerTick);
		if (bind_now <= 0) {
			continue;
		}
		out_binding_by_broker[broker_id] = bind_now;
		bound_progress_[broker_id] = already_bound + bind_now;
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
