#include "cxl_manager/corfu_mailbox_sequencer.h"

#include <glog/logging.h>

#include "cxl_manager/corfu_mailbox_messages.h"
#include "common/performance_utils.h"

namespace Embarcadero {
namespace cxl_manager {

CorfuMailboxSequencer::CorfuMailboxSequencer(CorfuSequencerImpl* impl,
		cxl_transport::MailboxSegment* segment, uint32_t K)
		: impl_(impl), segment_(segment), K_(K) {
	CHECK_NOTNULL(impl_);
	CHECK_NOTNULL(segment_);
	CHECK_GT(K_, 0u);
	// The mailbox record_size must hold both the request (consumed from up) and the grant
	// (produced to down); otherwise Produce could fail on an oversized record and a token that
	// AssignToken already committed would be undeliverable.
	CHECK_GE(segment_->record_size(), sizeof(CorfuTokenRequest))
			<< "mailbox record_size too small for CorfuTokenRequest";
	CHECK_GE(segment_->record_size(), sizeof(CorfuTokenGrant))
			<< "mailbox record_size too small for CorfuTokenGrant";
}

CorfuMailboxSequencer::~CorfuMailboxSequencer() {
	if (poll_thread_) {
		Stop();
		Join();
	}
}

void CorfuMailboxSequencer::StartThread() {
	CHECK(!poll_thread_) << "CorfuMailboxSequencer already started";
	stop_flag_.store(false, std::memory_order_relaxed);
	poll_thread_ = std::make_unique<std::thread>([this] { Run(); });
}

void CorfuMailboxSequencer::Stop() {
	stop_flag_.store(true, std::memory_order_relaxed);
}

void CorfuMailboxSequencer::Join() {
	if (poll_thread_ && poll_thread_->joinable()) {
		poll_thread_->join();
	}
}

void CorfuMailboxSequencer::Run() {
	LOG(INFO) << "CorfuMailboxSequencer started (brokers=" << segment_->num_brokers()
			<< ", K=" << K_ << ")";
	while (true) {
		bool drained_any = false;
		for (uint32_t broker = 0; broker < segment_->num_brokers(); ++broker) {
			if (DrainBrokerRequests(broker) > 0) {
				drained_any = true;
			}
		}
		// Exit only after a full empty pass once Stop() was requested, so any backlog still
		// sitting in the up rings is drained (and its grants delivered) before we return.
		if (stop_flag_.load(std::memory_order_relaxed) && !drained_any) break;
		if (!drained_any) CXL::cpu_pause();
	}
	LOG(INFO) << "CorfuMailboxSequencer exited";
}

uint32_t CorfuMailboxSequencer::DrainBrokerRequests(uint32_t broker_id) {
	cxl_transport::MailboxRing& up = segment_->up(broker_id);
	cxl_transport::MailboxRing& down = segment_->down(broker_id);

	uint32_t processed = 0;
	for (uint32_t i = 0; i < K_; ++i) {
		// Deliverability-before-commit: AssignToken irreversibly advances next_order_ and the
		// per-(client,broker) expected_batch_seq, so a token it assigns MUST be delivered —
		// dropping a grant would leave a permanent gap in the client's grant stream and in the
		// global order. The sequencer is the sole, single-threaded writer of down(broker_id),
		// so if HasSpace() is true now the TryProduce below is guaranteed to succeed. If down is
		// full (a slow/stuck broker consumer), stop draining THIS broker and move on WITHOUT
		// consuming the request — so one wedged broker never head-of-line-blocks the others and
		// never hangs Stop() (Run() re-checks stop_flag_ every pass). Back-pressure to a wedged
		// broker is applied naturally: its up ring fills and its own producer blocks.
		if (!down.HasSpace()) break;

		CorfuTokenRequest req{};
		uint32_t req_len = 0;
		if (!up.TryConsume(&req, sizeof(req), &req_len)) break;  // ring empty
		if (req_len != sizeof(CorfuTokenRequest)) {
			// Impossible with the fixed record layout (record_size validated in the ctor); a
			// malformed record cannot be correlated, so log loudly (fatal in debug) and skip.
			LOG(DFATAL) << "malformed CorfuTokenRequest on up(" << broker_id << "): len="
					<< req_len;
			continue;
		}

		// SAME ordering code as the TCP baseline — one AssignToken call per request, one
		// fetch_add per batch (num_msg). No batching/merging of requests here.
		uint64_t total_order = 0, log_idx = 0, broker_batch_seq = 0;
		CorfuSequencerImpl::TokenStatus status = impl_->AssignToken(
				req.client_id, req.batch_seq, req.num_msg, req.total_size,
				static_cast<int>(req.broker_id), &total_order, &log_idx, &broker_batch_seq);

		CorfuTokenGrant grant{};
		grant.client_id = req.client_id;    // echo for correlation
		grant.batch_seq = req.batch_seq;    // echo for correlation
		grant.session_id = req.session_id;
		grant.correlation_id = req.correlation_id;
		grant.broker_id = req.broker_id;
		grant.status = StatusToGrantValue(status);
		if (status == CorfuSequencerImpl::TokenStatus::OK) {
			grant.total_order = total_order;
			grant.log_idx = log_idx;
			grant.broker_batch_seq = broker_batch_seq;
		}

		// Guaranteed to succeed: we reserved space via HasSpace() and are the sole writer.
		bool ok = down.TryProduce(&grant, sizeof(grant));
		CHECK(ok) << "down(" << broker_id << ") lost space after HasSpace() — sole-writer "
				<< "invariant violated";
		++processed;
	}
	return processed;
}

uint32_t CorfuMailboxSequencer::StatusToGrantValue(CorfuSequencerImpl::TokenStatus status) {
	switch (status) {
		case CorfuSequencerImpl::TokenStatus::OK: return 0;
		case CorfuSequencerImpl::TokenStatus::INVALID_ARGUMENT: return 1;
		case CorfuSequencerImpl::TokenStatus::ALREADY_PROCESSED: return 2;
		case CorfuSequencerImpl::TokenStatus::OUT_OF_ORDER: return 3;
	}
	return 1;
}

}  // namespace cxl_manager
}  // namespace Embarcadero
