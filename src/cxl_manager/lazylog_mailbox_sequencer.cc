#include "cxl_manager/lazylog_mailbox_sequencer.h"

#include <glog/logging.h>

#include "cxl_manager/lazylog_mailbox_messages.h"
#include "common/performance_utils.h"

namespace Embarcadero {
namespace cxl_manager {

LazyLogMailboxSequencer::LazyLogMailboxSequencer(LazyLogBindingCore* core,
		cxl_transport::MailboxSegment* segment, uint32_t K)
		: core_(core), segment_(segment), K_(K) {
	CHECK_NOTNULL(core_);
	CHECK_NOTNULL(segment_);
	CHECK_GT(K_, 0u);
	// The mailbox record_size must hold both the LocalProgress consumed from up and the
	// GlobalBinding broadcast to down; otherwise BroadcastDown would CHECK-fail on an oversized
	// record.
	CHECK_GE(segment_->record_size(), sizeof(LazyLogLocalProgressMsg))
			<< "mailbox record_size too small for LazyLogLocalProgressMsg";
	CHECK_GE(segment_->record_size(), sizeof(LazyLogGlobalBindingMsg))
			<< "mailbox record_size too small for LazyLogGlobalBindingMsg";
	// The GlobalBinding encodes deltas for up to segment_->num_brokers() brokers into a fixed
	// array of kMaxBrokers slots; more brokers than that cannot be addressed.
	CHECK_LE(segment_->num_brokers(), static_cast<uint32_t>(kMaxBrokers))
			<< "num_brokers exceeds LazyLogGlobalBindingMsg::kMaxBrokers";
}

LazyLogMailboxSequencer::~LazyLogMailboxSequencer() {
	if (poll_thread_) {
		Stop();
		Join();
	}
}

void LazyLogMailboxSequencer::StartThread() {
	CHECK(!poll_thread_) << "LazyLogMailboxSequencer already started";
	stop_flag_.store(false, std::memory_order_relaxed);
	poll_thread_ = std::make_unique<std::thread>([this] { Run(); });
}

void LazyLogMailboxSequencer::Stop() {
	stop_flag_.store(true, std::memory_order_relaxed);
}

void LazyLogMailboxSequencer::Join() {
	if (poll_thread_ && poll_thread_->joinable()) {
		poll_thread_->join();
	}
}

void LazyLogMailboxSequencer::Run() {
	LOG(INFO) << "LazyLogMailboxSequencer started (brokers=" << segment_->num_brokers()
			<< ", K=" << K_ << ")";
	while (true) {
		bool made_progress = false;

		// Phase 1: drain every up(b) ring, feeding LocalProgress records into the shared core.
		uint32_t drained = 0;
		for (uint32_t broker = 0; broker < segment_->num_brokers(); ++broker) {
			drained += DrainBrokerProgress(broker);
		}
		if (drained > 0) made_progress = true;

		// Phase 2: emit a GlobalBinding ONLY when new local progress arrived AND every broker has
		// reported (the same readiness gate the gRPC baseline uses). Gating on new data prevents
		// re-broadcasting the same (idempotent) snapshot every idle pass, so the epoch tag advances
		// exactly once per fresh per-epoch report round — the same report -> aggregate -> broadcast
		// cadence as the gRPC SendGlobalBinding loop, without the wall-clock interval.
		if (drained > 0) {
			if (MaybeEmitGlobalBinding()) made_progress = true;
		}

		// Exit only after a full pass makes no progress once Stop() was requested, so any backlog
		// still sitting in the up rings is drained (and a final binding emitted) first.
		if (stop_flag_.load(std::memory_order_relaxed) && !made_progress) break;
		if (!made_progress) CXL::cpu_pause();
	}
	LOG(INFO) << "LazyLogMailboxSequencer exited (bindings_broadcast=" << current_binding_round_
			<< ")";
}

uint32_t LazyLogMailboxSequencer::DrainBrokerProgress(uint32_t broker_id) {
	cxl_transport::MailboxRing& up = segment_->up(broker_id);
	uint32_t processed = 0;
	for (uint32_t i = 0; i < K_; ++i) {
		LazyLogLocalProgressMsg msg{};
		uint32_t msg_len = 0;
		if (!up.TryConsume(&msg, sizeof(msg), &msg_len)) break;  // ring empty
		if (msg_len != sizeof(LazyLogLocalProgressMsg)) {
			// Impossible with the fixed record layout (record_size validated in the ctor); a
			// malformed record cannot be interpreted, so log loudly (fatal in debug) and skip.
			LOG(DFATAL) << "malformed LazyLogLocalProgressMsg on up(" << broker_id << "): len="
					<< msg_len;
			continue;
		}
		// SAME accumulation code as the gRPC baseline (latest-cumulative-progress + regression
		// rejection), via the shared core. No batching/merging of progress here.
		core_->AddLocalProgress(msg.broker_id, msg.epoch, msg.local_progress);
		++processed;
	}
	return processed;
}

bool LazyLogMailboxSequencer::MaybeEmitGlobalBinding() {
	absl::btree_map<int, int64_t> binding_by_broker;
	int64_t aligned_epoch = -1;
	// Readiness gate + per-broker binding-delta decision, both owned by the shared core (identical
	// to the gRPC path). Returns false until every expected broker has reported AND a new epoch
	// E = MIN over brokers of their latest reported epoch has closed — this is the durability
	// coupling (no binding before the whole broker set has reported) plus LazyLog's per-epoch
	// report round barrier. aligned_epoch is E, the data epoch this snapshot represents.
	if (!core_->ComputeGlobalBinding(static_cast<int>(segment_->num_brokers()), binding_by_broker,
			&aligned_epoch, /*use_epoch_barrier=*/true)) {
		return false;
	}

	// Encode the map<broker_id, delta> as the fixed by-broker array (sentinel -1 for absent). Tag
	// the broadcast with the DATA epoch the snapshot represents (from the core's epoch barrier),
	// not a free-running counter — the tag must identify the aggregated epoch so a reader can
	// correlate the binding to the exact per-epoch report round it summarizes.
	LazyLogGlobalBindingMsg msg{};
	msg.epoch = aligned_epoch;
	msg.num_brokers = segment_->num_brokers();
	msg.pad0 = 0;
	for (int i = 0; i < kMaxBrokers; ++i) {
		msg.binding[i] = kAbsentBinding;
	}
	for (const auto& [broker_id, delta] : binding_by_broker) {
		CHECK_GE(broker_id, 0);
		CHECK_LT(broker_id, kMaxBrokers)
				<< "broker_id out of range for LazyLogGlobalBindingMsg::binding[]";
		msg.binding[broker_id] = delta;
	}

	// Wedged-safe broadcast: ONE bounded, decoupled pass to every down(b). A broker whose down
	// ring is full/not-draining is reported WEDGED and skipped for this pass — it never blocks the
	// healthy brokers and never hangs Stop(). Binding deltas are cumulative/idempotent, so the
	// wedged broker simply gets the aggregate of the rounds it missed on a later ready pass.
	cxl_transport::BroadcastStatus status = segment_->BroadcastDown(&msg, sizeof(msg));
	if (status.wedged_count > 0 || status.failed_count > 0 || (current_binding_round_ % 1000) == 0) {
		LOG(INFO) << "LazyLogMailboxSequencer broadcast #" << current_binding_round_
				<< " data_epoch=" << aligned_epoch
				<< " wedged=" << status.wedged_count
				<< " failed=" << status.failed_count;
	}

	// current_binding_round_ counts broadcasts (for logging only); a slow/wedged broker does not
	// hold up the others' progress. The WIRE tag is the data epoch (set above), not this counter.
	++current_binding_round_;
	return true;
}

}  // namespace cxl_manager
}  // namespace Embarcadero
