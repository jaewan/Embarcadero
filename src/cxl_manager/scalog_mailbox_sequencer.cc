#include "cxl_manager/scalog_mailbox_sequencer.h"

#include <glog/logging.h>

#include "cxl_manager/scalog_mailbox_messages.h"
#include "common/performance_utils.h"

namespace Embarcadero {
namespace cxl_manager {

ScalogMailboxSequencer::ScalogMailboxSequencer(ScalogGlobalOrderingCore* core,
		cxl_transport::MailboxSegment* segment, uint32_t K)
		: core_(core), segment_(segment), K_(K) {
	CHECK_NOTNULL(core_);
	CHECK_NOTNULL(segment_);
	CHECK_GT(K_, 0u);
	// The mailbox record_size must hold both the LocalCut consumed from up and the GlobalCut
	// broadcast to down; otherwise BroadcastDown would CHECK-fail on an oversized record.
	CHECK_GE(segment_->record_size(), sizeof(ScalogLocalCutMsg))
			<< "mailbox record_size too small for ScalogLocalCutMsg";
	CHECK_GE(segment_->record_size(), sizeof(ScalogGlobalCutMsg))
			<< "mailbox record_size too small for ScalogGlobalCutMsg";
	// The GlobalCut encodes cuts for up to segment_->num_brokers() brokers into a fixed
	// array of kMaxBrokers slots; more brokers than that cannot be addressed.
	CHECK_LE(segment_->num_brokers(), static_cast<uint32_t>(kMaxBrokers))
			<< "num_brokers exceeds ScalogGlobalCutMsg::kMaxBrokers";
}

ScalogMailboxSequencer::~ScalogMailboxSequencer() {
	if (poll_thread_) {
		Stop();
		Join();
	}
}

void ScalogMailboxSequencer::StartThread() {
	CHECK(!poll_thread_) << "ScalogMailboxSequencer already started";
	stop_flag_.store(false, std::memory_order_relaxed);
	poll_thread_ = std::make_unique<std::thread>([this] { Run(); });
}

void ScalogMailboxSequencer::Stop() {
	stop_flag_.store(true, std::memory_order_relaxed);
}

void ScalogMailboxSequencer::Join() {
	if (poll_thread_ && poll_thread_->joinable()) {
		poll_thread_->join();
	}
}

void ScalogMailboxSequencer::Run() {
	LOG(INFO) << "ScalogMailboxSequencer started (brokers=" << segment_->num_brokers()
			<< ", K=" << K_ << ")";
	while (true) {
		bool made_progress = false;

		// Phase 1: drain every up(b) ring, feeding LocalCut records into the shared core.
		uint32_t drained = 0;
		for (uint32_t broker = 0; broker < segment_->num_brokers(); ++broker) {
			drained += DrainBrokerCuts(broker);
		}
		if (drained > 0) made_progress = true;

		// Phase 2: emit a GlobalCut ONLY when new local cuts arrived AND the whole replica
		// set has reported (the same readiness gate the gRPC baseline uses). Gating on new
		// data prevents re-broadcasting the same (idempotent) snapshot every idle pass, so
		// the epoch tag advances exactly once per fresh per-epoch report round — the same
		// report -> aggregate -> broadcast cadence as the gRPC SendGlobalCut loop, without
		// the wall-clock interval.
		if (drained > 0) {
			if (MaybeEmitGlobalCut()) made_progress = true;
		}

		// Exit only after a full pass makes no progress once Stop() was requested, so any
		// backlog still sitting in the up rings is drained (and a final cut emitted) first.
		if (stop_flag_.load(std::memory_order_relaxed) && !made_progress) break;
		if (!made_progress) CXL::cpu_pause();
	}
	LOG(INFO) << "ScalogMailboxSequencer exited (epochs_broadcast=" << current_epoch_ << ")";
}

uint32_t ScalogMailboxSequencer::DrainBrokerCuts(uint32_t broker_id) {
	cxl_transport::MailboxRing& up = segment_->up(broker_id);
	uint32_t processed = 0;
	for (uint32_t i = 0; i < K_; ++i) {
		ScalogLocalCutMsg msg{};
		uint32_t msg_len = 0;
		if (!up.TryConsume(&msg, sizeof(msg), &msg_len)) break;  // ring empty
		if (msg_len != sizeof(ScalogLocalCutMsg)) {
			// Impossible with the fixed record layout (record_size validated in the ctor); a
			// malformed record cannot be interpreted, so log loudly (fatal in debug) and skip.
			LOG(DFATAL) << "malformed ScalogLocalCutMsg on up(" << broker_id << "): len="
					<< msg_len;
			continue;
		}
		// SAME accumulation code as the gRPC baseline (delta-since-last + regression
		// rejection), via the shared core. No batching/merging of cuts here.
		core_->AddLocalCut(msg.broker_id, msg.replica_id, msg.epoch, msg.local_cut);
		++processed;
	}
	return processed;
}

bool ScalogMailboxSequencer::MaybeEmitGlobalCut() {
	absl::btree_map<int, int64_t> cut_by_broker;
	int64_t aligned_epoch = -1;
	// Readiness gate + MIN-across-replicas decision, both owned by the shared core (identical
	// to the gRPC path). Returns false until every expected broker/replica has reported AND a
	// new epoch E = MIN over replicas of their latest reported epoch has closed — this is the
	// durability coupling (no cut ahead of the whole replica set's minimum) plus Scalog's
	// per-epoch report round barrier. aligned_epoch is E, the data epoch this snapshot
	// represents.
	if (!core_->ComputeGlobalCut(static_cast<int>(segment_->num_brokers()), cut_by_broker,
			&aligned_epoch, /*use_epoch_barrier=*/true)) {
		return false;
	}

	// Encode the map<broker_id, cut> as the fixed by-broker array (sentinel -1 for absent).
	// Tag the broadcast with the DATA epoch the snapshot represents (from the core's epoch
	// barrier), not a free-running counter — the tag must identify the aggregated epoch so a
	// reader can correlate the cut to the exact per-epoch report round it summarizes.
	ScalogGlobalCutMsg msg{};
	msg.epoch = aligned_epoch;
	msg.num_brokers = segment_->num_brokers();
	msg.pad0 = 0;
	for (int i = 0; i < kMaxBrokers; ++i) {
		msg.cut[i] = kAbsentCut;
	}
	for (const auto& [broker_id, cut] : cut_by_broker) {
		CHECK_GE(broker_id, 0);
		CHECK_LT(broker_id, kMaxBrokers)
				<< "broker_id out of range for ScalogGlobalCutMsg::cut[]";
		msg.cut[broker_id] = cut;
	}

	// Wedged-safe broadcast: ONE bounded, decoupled pass to every down(b). A broker whose
	// down ring is full/not-draining is reported WEDGED and skipped for this pass — it never
	// blocks the healthy brokers and never hangs Stop(). The cut is cumulative/idempotent, so
	// the wedged broker simply gets the newer cut on a later ready pass.
	cxl_transport::BroadcastStatus status = segment_->BroadcastDown(&msg, sizeof(msg));
	if (status.wedged_count > 0 || status.failed_count > 0 || (current_epoch_ % 1000) == 0) {
		LOG(INFO) << "ScalogMailboxSequencer broadcast #" << current_epoch_
				<< " data_epoch=" << aligned_epoch
				<< " wedged=" << status.wedged_count
				<< " failed=" << status.failed_count;
	}

	// current_epoch_ counts broadcasts (for logging only); a slow/wedged broker does not hold
	// up the others' progress. The WIRE tag is the data epoch (set above), not this counter.
	++current_epoch_;
	return true;
}

}  // namespace cxl_manager
}  // namespace Embarcadero
