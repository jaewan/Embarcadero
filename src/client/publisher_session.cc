
#include "common/env_flags.h"
#include "common/stage_trace.h"
#include <array>
#include <iomanip>
#include "publisher.h"
#include "publisher_profile.h"
#include "latency_stats.h"
#include "common/config.h"
#include "common/order_level.h"
#include "common/scoped_fd.h"
#include "common/fault_injection.h"
#include "network_manager/protocol.h"
#include "session.pb.h"
#include "absl/container/flat_hash_map.h"
#include <cstring>
#include <random>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <limits>
#include <mutex>
#include <condition_variable>
#include <netdb.h>
#include <numeric>
#include <set>
#include <stdexcept>



#include "publisher_internal.h"

using namespace Embarcadero::client::detail;

// Session negotiation, fencing, and retained-suffix recovery.
namespace {
bool SendAllBlocking(int fd, const void* data, size_t len) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (n == 0) return false;
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool RecvAllBlocking(int fd, void* data, size_t len) {
	uint8_t* p = static_cast<uint8_t*>(data);
	size_t got = 0;
	while (got < len) {
		ssize_t n = recv(fd, p + got, len - got, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (n == 0) return false;
		got += static_cast<size_t>(n);
	}
	return true;
}

void SetSocketBlockingTemporarily(int fd, bool blocking, int* old_flags) {
	if (old_flags == nullptr) return;
	if (*old_flags < 0) *old_flags = fcntl(fd, F_GETFL, 0);
	if (*old_flags < 0) return;
	int flags = blocking ? (*old_flags & ~O_NONBLOCK) : *old_flags;
	fcntl(fd, F_SETFL, flags);
}

int GetSessionOpenTimeoutSec() {
	if (const char* env = std::getenv("EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC")) {
		char* end = nullptr;
		long parsed = std::strtol(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0 && parsed <= 60) {
			return static_cast<int>(parsed);
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC='" << env
		             << "'; using default 5s";
	}
	return 5;
}

}  // namespace

uint32_t Publisher::InitialSessionEpochRequest() const {
	const uint32_t override_epoch = ReadSessionEpochOverride();
	return override_epoch != 0 ? override_epoch : 1U;
}

uint64_t Publisher::SessionLeaseNs() const {
	if (const char* env = std::getenv("EMBARCADERO_SESSION_LEASE_MS")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			return static_cast<uint64_t>(parsed) * 1000ULL * 1000ULL;
		}
	}
	// Match broker GetSessionLeaseNs: RF0/RF2 defaults are both 30s so client
	// rollover drain and broker hold-gap fencing agree.
	(void)ack_level_;
	return 30000ULL * 1000ULL * 1000ULL;
}

bool Publisher::SendSessionOpenOnSocket(
		int sock_fd,
		int,
		size_t broker_id,
		bool allow_fence_recovery) {
	if (!IsOrder5SessionMode()) return true;
	uint32_t requested = requested_session_epoch_.load(std::memory_order_acquire);
	if (requested == 0) {
		requested = InitialSessionEpochRequest();
		uint32_t expected = 0;
		requested_session_epoch_.compare_exchange_strong(expected, requested);
	}

	embarcadero::session::SessionOpen open;
	open.set_client_id(static_cast<uint32_t>(client_id_));
	open.set_requested_session_epoch(requested);
	open.set_topic(std::string(topic_, strnlen(topic_, TOPIC_NAME_SIZE)));
	std::string payload;
	if (!open.SerializeToString(&payload)) return false;
	if (payload.size() > kMaxSessionControlPayload) return false;
	const SessionControlHeader header{kSessionControlMagic, static_cast<uint32_t>(payload.size())};

	int old_flags = -1;
	SetSocketBlockingTemporarily(sock_fd, true, &old_flags);
	struct timeval tv;
	tv.tv_sec = GetSessionOpenTimeoutSec();
	tv.tv_usec = 0;
	setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	const bool sent = SendAllBlocking(sock_fd, &header, sizeof(header)) &&
	                  SendAllBlocking(sock_fd, payload.data(), payload.size());
	if (!sent) {
		if (old_flags >= 0) fcntl(sock_fd, F_SETFL, old_flags);
		return false;
	}

	SessionControlHeader ack_header{};
	if (!RecvAllBlocking(sock_fd, &ack_header, sizeof(ack_header)) ||
	    ack_header.magic != kSessionControlMagic ||
	    ack_header.length == 0 ||
	    ack_header.length > kMaxSessionControlPayload) {
		if (old_flags >= 0) fcntl(sock_fd, F_SETFL, old_flags);
		return false;
	}
	std::string ack_payload(ack_header.length, '\0');
	if (!RecvAllBlocking(sock_fd, ack_payload.data(), ack_payload.size())) {
		if (old_flags >= 0) fcntl(sock_fd, F_SETFL, old_flags);
		return false;
	}
	if (old_flags >= 0) fcntl(sock_fd, F_SETFL, old_flags);

	embarcadero::session::SessionOpenAck ack;
	if (!ack.ParseFromString(ack_payload) || ack.assigned_session_epoch() == 0) {
		return false;
	}
	LOG(INFO) << "[SESSION_OPEN_ACK]"
	          << " broker_id=" << broker_id
	          << " client_id=" << client_id_
	          << " assigned_session_epoch=" << ack.assigned_session_epoch()
	          << " committed_hwm=" << ack.committed_hwm()
	          << " has_committed_prefix=" << (ack.has_committed_prefix() ? 1 : 0)
	          << " status=" << ack.status();
    if (ack.status() == embarcadero::session::SessionOpenAck::RESOURCE_EXHAUSTED) {
        LOG(ERROR) << "Session OPEN rejected: broker authoritative state is full/unavailable";
        return false;
    }
	if (ack.status() == embarcadero::session::SessionOpenAck::FENCED ||
	    ack.status() == embarcadero::session::SessionOpenAck::EPOCH_STALE) {
		// Auxiliary retransmit sockets are opened while retransmit_send_mu_ is
		// held. They must never initiate rollover: doing so can invert
		// retransmit_send_mu_ and session_fence_handle_mu_ against the primary
		// ACK thread. Fail closed and let the primary ACK/control path recover.
		if (!allow_fence_recovery) {
			LOG(WARNING) << "[SESSION_OPEN_AUX_FENCED]"
			             << " broker_id=" << broker_id
			             << " assigned_session_epoch=" << ack.assigned_session_epoch()
			             << " status=" << ack.status();
			return false;
		}
		// A retransmit connection can race the primary ACK connection that
		// already started rollover. SendRawBatchToBroker holds
		// retransmit_send_mu_ while this handshake runs; recursively entering
		// HandleSessionFenced would then deadlock with the outer handler, which
		// closes epoch-bound retransmit channels while holding
		// session_fence_handle_mu_. Let the active handler own recovery.
		if (session_fenced_reopen_pending_.load(std::memory_order_acquire)) {
			LOG(WARNING) << "[SESSION_OPEN_FENCE_DEFERRED]"
			             << " broker_id=" << broker_id
			             << " assigned_session_epoch=" << ack.assigned_session_epoch()
			             << " status=" << ack.status();
			return false;
		}
		embarcadero::session::SessionFenced fenced;
		if (ack.has_committed_prefix()) {
			fenced.set_committed_batch_seq(ack.committed_hwm());
			fenced.set_has_committed_prefix(true);
		} else {
			fenced.set_committed_batch_seq(0);
			fenced.set_has_committed_prefix(false);
		}
		fenced.set_committed_msg_hwm(0);
		fenced.set_control_epoch(0);
		fenced.set_reason(ack.status() == embarcadero::session::SessionOpenAck::EPOCH_STALE
			? embarcadero::session::SessionFenced::EPOCH_STALE
			: embarcadero::session::SessionFenced::HOLD_EXPIRY);
		HandleSessionFenced(fenced, static_cast<int>(broker_id));
		return false;
	} else if (ack.has_committed_prefix() && IsOrder5SessionMode() && ack_level_ >= 1) {
		session_epoch_.store(ack.assigned_session_epoch(), std::memory_order_release);
		requested_session_epoch_.store(ack.assigned_session_epoch(), std::memory_order_release);
		// Trim optimistic unacked prefix through the inclusive reconnect HWM.
		// Must advance the retire cursor with the erase — otherwise
		// CompleteUnackedThrough wedges permanently on a retire gap.
		std::vector<Embarcadero::BatchHeader*> release_after_unlock;
		{
			std::lock_guard<std::mutex> lock(unacked_mu_);
			TrimUnackedThroughBatchSeqLocked(
				ack.committed_hwm(), &release_after_unlock);
		}
		for (auto* batch : release_after_unlock) {
			pubQue_.ReleaseBatch(batch);
		}
	} else {
		session_epoch_.store(ack.assigned_session_epoch(), std::memory_order_release);
		requested_session_epoch_.store(ack.assigned_session_epoch(), std::memory_order_release);
	}
	return true;
}

bool Publisher::EnsureRetransmitChannel(int broker_id, int* out_fd) {
	if (out_fd == nullptr) return false;
	{
		std::lock_guard<std::mutex> lock(retransmit_channel_mu_);
		auto it = retransmit_channels_.find(broker_id);
		if (it != retransmit_channels_.end() && it->second >= 0) {
			*out_fd = it->second;
			return true;
		}
	}

	std::string addr;
	size_t num_brokers = 0;
	{
		absl::MutexLock lock(&mutex_);
		auto it = nodes_.find(broker_id);
		if (it == nodes_.end()) return false;
		try {
			auto parsed = ParseAddressPort(it->second);
			addr = parsed.first;
		} catch (const std::exception& e) {
			LOG(ERROR) << "EnsureRetransmitChannel: invalid broker address " << it->second
			           << ": " << e.what();
			return false;
		}
		num_brokers = nodes_.size();
	}

	ScopedFd sock(GetNonblockingSock(const_cast<char*>(addr.c_str()), PORT + broker_id));
	if (sock.get() < 0) return false;
	ScopedFd efd(epoll_create1(0));
	if (efd.get() < 0) return false;
	epoll_event event;
	event.data.fd = sock.get();
	event.events = EPOLLOUT | EPOLLRDHUP;
	if (epoll_ctl(efd.get(), EPOLL_CTL_ADD, sock.get(), &event) != 0) return false;
	epoll_event events[4];
	int ready = epoll_wait(efd.get(), events, 4, 1000);
	if (ready <= 0) return false;
	int sock_err = 0;
	socklen_t sock_err_len = sizeof(sock_err);
	if (getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len) != 0 ||
	    sock_err != 0) {
		return false;
	}
	int old_flags = -1;
	SetSocketBlockingTemporarily(sock.get(), true, &old_flags);

	Embarcadero::EmbarcaderoReq shake;
	std::memset(&shake, 0, sizeof(shake));
	shake.client_req = Embarcadero::Publish;
	shake.client_id = client_id_;
	std::memcpy(shake.topic, topic_, std::min<size_t>(TOPIC_NAME_SIZE - 1, sizeof(shake.topic) - 1));
	shake.ack = ack_level_;
	shake.port = ack_port_;
	shake.num_msg = num_brokers;
	if (!SendAllBlocking(sock.get(), &shake, sizeof(shake))) return false;
	if (!SendSessionOpenOnSocket(
		    sock.get(),
		    efd.get(),
		    static_cast<size_t>(broker_id),
		    /*allow_fence_recovery=*/false)) {
		return false;
	}
	if (old_flags >= 0) fcntl(sock.get(), F_SETFL, old_flags);

	const int fd = sock.release();
	{
		std::lock_guard<std::mutex> lock(retransmit_channel_mu_);
		auto it = retransmit_channels_.find(broker_id);
		if (it != retransmit_channels_.end() && it->second >= 0) {
			close(fd);
			*out_fd = it->second;
			return true;
		}
		retransmit_channels_[broker_id] = fd;
		*out_fd = fd;
	}
	return true;
}

void Publisher::CloseRetransmitChannel(int broker_id) {
	std::lock_guard<std::mutex> lock(retransmit_channel_mu_);
	auto it = retransmit_channels_.find(broker_id);
	if (it == retransmit_channels_.end()) return;
	if (it->second >= 0) {
		close(it->second);
	}
	retransmit_channels_.erase(it);
}

void Publisher::CloseAllRetransmitChannels() {
	// Interrupt any blocked old-epoch send before waiting for the send lock.
	// Closing first would permit fd reuse while SendRawBatchToBroker still uses
	// the integer descriptor; shutdown() wakes the send without releasing the fd.
	{
		std::lock_guard<std::mutex> channel_lock(retransmit_channel_mu_);
		for (const auto& [broker_id, fd] : retransmit_channels_) {
			(void)broker_id;
			if (fd >= 0) shutdown(fd, SHUT_RDWR);
		}
	}
	// SendRawBatchToBroker does not hold retransmit_channel_mu_ while writing.
	// Once its send has returned, take both locks in the normal send->channel
	// order and close descriptors without a close()/fd-reuse race.
	std::lock_guard<std::mutex> send_lock(retransmit_send_mu_);
	std::lock_guard<std::mutex> channel_lock(retransmit_channel_mu_);
	for (auto& [broker_id, fd] : retransmit_channels_) {
		(void)broker_id;
		if (fd >= 0) close(fd);
	}
	retransmit_channels_.clear();
}

bool Publisher::SendRawBatchToBroker(const void* bytes, size_t wire_bytes, int broker_id) {
	if (bytes == nullptr || wire_bytes < sizeof(Embarcadero::BatchHeader)) return false;
	std::lock_guard<std::mutex> send_lock(retransmit_send_mu_);
	int fd = -1;
	if (!EnsureRetransmitChannel(broker_id, &fd) || fd < 0) return false;
	int old_flags = -1;
	SetSocketBlockingTemporarily(fd, true, &old_flags);
	const bool ok = SendAllBlocking(fd, bytes, wire_bytes);
	if (old_flags >= 0) fcntl(fd, F_SETFL, old_flags);
	if (!ok) {
		CloseRetransmitChannel(broker_id);
		return false;
	}
	return true;
}

bool Publisher::WaitForSessionSendDrain(size_t target_messages) {
	// [[DRAIN_TIMEOUT]] Use a bounded 5 s timeout rather than the full session
	// lease. Idle publish threads blocked in pubQue_.Read() will never advance
	// session_sent_hwm_, so waiting the full lease (up to 180 s) just blocks the
	// AckThread inside HandleSessionFenced — preventing Poll() from ever checking
	// the post-fence exit condition. 5 s is enough for any genuinely in-flight
	// batches to complete. An incomplete drain must fail closed: proceeding to
	// the unacked snapshot would silently omit a dequeued/in-flight batch.
	constexpr int64_t kDrainTimeoutNs = 5LL * 1000LL * 1000LL * 1000LL;
	const int64_t deadline_ns = SteadyNowNs() + kDrainTimeoutNs;
	while (SteadyNowNs() < deadline_ns) {
		size_t sent_hwm = 0;
		{
			std::lock_guard<std::mutex> lock(unacked_mu_);
			sent_hwm = ack_message_base_.load(std::memory_order_acquire) + session_sent_hwm_;
		}
		if (sent_hwm >= target_messages) return true;
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	LOG(WARNING) << "[SESSION_ROLLOVER_DRAIN_TIMEOUT]"
	             << " target_messages=" << target_messages;
	return false;
}

void Publisher::HandleSessionFenced(const embarcadero::session::SessionFenced& fenced, int broker_id) {
	if (!IsOrder5SessionMode()) return;
	// Serialize concurrent fence notifications (multiple brokers / remapped ACK
	// sockets) so Pause/Seal/resubmit cannot interleave.
	std::lock_guard<std::mutex> fence_lock(session_fence_handle_mu_);
    if (publisher_workers_stop_.load(std::memory_order_acquire) ||
        shutdown_.load(std::memory_order_acquire)) return;

	// During ACK2 durable drain, identical lease restates of a frozen committed
	// prefix must not reopen/resubmit (that re-burns BLog before ingest dedup).
	// A newer HWM with remaining unacked suffix still falls through to reopen so
	// held/purged batches can complete after force-expire fencing.
	if (publish_finished_.load(std::memory_order_acquire) && ack_level_ >= 2 &&
	    ack_drain_active_.load(std::memory_order_acquire) &&
	    fenced.has_committed_prefix()) {
		const uint64_t release_hwm = fenced.committed_batch_seq();
		std::vector<Embarcadero::BatchHeader*> release_after_unlock;
		size_t locally_committed_msgs = 0;
		size_t remaining_suffix = 0;
		{
			std::lock_guard<std::mutex> lock(unacked_mu_);
			// Same retire-cursor advance as SessionOpen trim — erase alone left
			// next_retire stuck and wedged CompleteUnackedThrough.
			locally_committed_msgs =
				TrimUnackedThroughBatchSeqLocked(release_hwm, &release_after_unlock);
			remaining_suffix = unacked_batches_.size();
			if (locally_committed_msgs > 0) {
				unacked_cv_.notify_all();
			}
		}
		for (auto* batch : release_after_unlock) {
			pubQue_.ReleaseBatch(batch);
		}
		if (locally_committed_msgs > 0) {
			broker_stats_[0].acked_messages.fetch_add(locally_committed_msgs, std::memory_order_relaxed);
			ack_received_.fetch_add(locally_committed_msgs, std::memory_order_release);
			last_ack_progress_ns_.store(SteadyNowNs(), std::memory_order_release);
			LOG(WARNING) << "[SESSION_FENCE_ACK_DRAIN_CREDIT]"
			             << " broker_id=" << broker_id
			             << " committed_batch_seq=" << release_hwm
			             << " credited_msgs=" << locally_committed_msgs
			             << " remaining_suffix=" << remaining_suffix;
		}
		const uint64_t prev_hwm =
			last_ack_drain_fence_hwm_.load(std::memory_order_acquire);
		if (remaining_suffix == 0 || release_hwm == prev_hwm) {
			VLOG(1) << "[SESSION_FENCE_SUPPRESSED_DURING_ACK_DRAIN]"
			        << " broker_id=" << broker_id
			        << " committed_batch_seq=" << release_hwm
			        << " remaining_suffix=" << remaining_suffix
			        << " same_hwm=" << (release_hwm == prev_hwm ? 1 : 0);
			return;
		}
		last_ack_drain_fence_hwm_.store(release_hwm, std::memory_order_release);
		LOG(WARNING) << "[SESSION_FENCE_ACK_DRAIN_RESUBMIT]"
		             << " broker_id=" << broker_id
		             << " committed_batch_seq=" << release_hwm
		             << " remaining_suffix=" << remaining_suffix;
		// Fall through to reopen + suffix resubmit.
	}

	const uint32_t old_epoch = session_epoch_.load(std::memory_order_acquire);
	const uint32_t new_epoch = NextSessionEpochAfterFence(old_epoch);
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
    if (!Embarcadero::fault::Pause("session.before_reopen",
            {static_cast<uint64_t>(client_id_), old_epoch, new_epoch,
             fenced.has_committed_prefix() ? 1ULL : 0ULL, fenced.committed_msg_hwm()}, &shutdown_)) return;
#endif
	session_fenced_observed_.fetch_add(1, std::memory_order_relaxed);
	// [[FENCE_POLL_EXIT]] Record the first fence observation time so Poll() can
	// bound the post-fence ACK wait regardless of trickle ACKs from survivors.
	{
		int64_t expected_zero = 0;
		const int64_t now_ns = SteadyNowNs();
		session_fence_observed_ns_.compare_exchange_strong(
			expected_zero, now_ns, std::memory_order_release, std::memory_order_relaxed);
	}
	const uint64_t release_hwm = fenced.has_committed_prefix() ? fenced.committed_batch_seq() : UINT64_MAX;
	// UINT64_MAX sentinel means empty prefix: credit nothing by batch_seq compare.
	session_fenced_committed_batch_seq_.store(
		fenced.has_committed_prefix() ? fenced.committed_batch_seq() : 0,
		std::memory_order_release);
	session_fenced_reopen_pending_.store(true, std::memory_order_release);
	LOG(WARNING) << "[SESSION_ROLLOVER_PHASE] phase=pause_begin"
	             << " old_epoch=" << old_epoch
	             << " new_epoch=" << new_epoch;
	pubQue_.PauseSessionRollover();
	LOG(WARNING) << "[SESSION_ROLLOVER_PHASE] phase=pause_done";
	struct RolloverResumeGuard {
		Publisher* self;
		bool active{true};
		~RolloverResumeGuard() {
			if (!active) return;
			self->session_fenced_reopen_pending_.store(false, std::memory_order_release);
			self->pubQue_.ResumeSessionRollover();
            self->NotifyPublisherWork();
		}
		void dismiss() { active = false; }
	} rollover_guard{this};
	NotifyPublisherWork();
	const size_t sealed = pubQue_.SealAllForSessionRollover();
	NotifyPublisherWork();
	LOG(WARNING) << "[SESSION_ROLLOVER_PHASE] phase=seal_done"
	             << " sealed_messages=" << sealed;
	if (sealed > 0) {
		client_order_.fetch_add(sealed, std::memory_order_release);
	}
	const size_t drain_target = std::max(
		client_order_.load(std::memory_order_acquire), pubQue_.PublishedMessages());
	if (!WaitForSessionSendDrain(drain_target)) {
		LOG(ERROR) << "[SESSION_REOPEN_UNSENT_BATCH]"
		           << " target_messages=" << drain_target
		           << " published_messages=" << pubQue_.PublishedMessages();
		shutdown_.store(true, std::memory_order_release);
		pubQue_.ReturnReads();
		unacked_cv_.notify_all();
		return;
	}
	LOG(WARNING) << "[SESSION_ROLLOVER_PHASE] phase=send_drain_done";
	requested_session_epoch_.store(new_epoch, std::memory_order_release);
	session_epoch_.store(new_epoch, std::memory_order_release);
	last_ack_progress_ns_.store(SteadyNowNs(), std::memory_order_release);
	// Retransmit sockets are epoch-bound by their SessionOpen handshake. Reusing
	// an epoch-N socket for epoch N+1 makes the broker reject the correctly
	// restamped suffix and creates another gap/fence cycle.
	LOG(WARNING) << "[SESSION_ROLLOVER_PHASE] phase=close_retransmit_begin";
	CloseAllRetransmitChannels();
	LOG(WARNING) << "[SESSION_ROLLOVER_PHASE] phase=close_retransmit_done";

	std::vector<UnackedBatch> suffix;
	std::vector<Embarcadero::BatchHeader*> release_after_unlock;
	size_t locally_committed_msgs = 0;
	const size_t ack_base_before_local_credit = ack_received_.load(std::memory_order_acquire);
	{
		std::lock_guard<std::mutex> lock(unacked_mu_);
		for (auto& rec : unacked_batches_) {
			if (rec.session_epoch != old_epoch) continue;
			if (fenced.has_committed_prefix() && rec.original_batch_seq <= release_hwm) {
				locally_committed_msgs += rec.num_msg;
				if (rec.pool_batch != nullptr) {
					release_after_unlock.push_back(rec.pool_batch);
				}
			} else {
				suffix.push_back(std::move(rec));
			}
		}
		std::sort(suffix.begin(), suffix.end(), [](const UnackedBatch& a, const UnackedBatch& b) {
			return a.original_batch_seq < b.original_batch_seq;
		});
		unacked_batches_.clear();
		unacked_bytes_ = 0;
		session_sent_hwm_ = 0;
		session_retire_prefix_hwm_ = 0;
		session_next_retire_batch_seq_ = 0;
		session_trim_committed_hwm_ = 0;
		session_trim_committed_valid_ = false;
		unacked_cv_.notify_all();
	}
	// A fence report identifies the first sequence the head could not commit.
	// The retained record exists only after the client finished sending the
	// batch. Diagnose that boundary without logging on the publish hot path.
	const uint64_t missing_seq = fenced.has_committed_prefix() ? release_hwm + 1 : 0;
	const auto missing_it = std::lower_bound(
		suffix.begin(), suffix.end(), missing_seq,
		[](const UnackedBatch& rec, uint64_t seq) {
			return rec.original_batch_seq < seq;
		});
	const bool missing_retained =
		missing_it != suffix.end() && missing_it->original_batch_seq == missing_seq;
	LOG(WARNING) << "[SESSION_FENCE_PREDECESSOR]"
	             << " old_epoch=" << old_epoch
	             << " missing_batch_seq=" << missing_seq
	             << " retained_after_send=" << (missing_retained ? 1 : 0)
	             << " sent_age_ms="
	             << (missing_retained
	                     ? std::max<int64_t>(0, SteadyNowNs() - missing_it->last_send_ns) / 1000000
	                     : -1)
	             << " broker_id=" << (missing_retained ? missing_it->broker_id : -1)
	             << " suffix_first_seq=" << (suffix.empty() ? 0 : suffix.front().original_batch_seq)
	             << " suffix_last_seq=" << (suffix.empty() ? 0 : suffix.back().original_batch_seq)
	             << " suffix_batches=" << suffix.size();
	LOG(WARNING) << "[SESSION_ROLLOVER_PHASE] phase=suffix_collected"
	             << " suffix_batches=" << suffix.size()
	             << " committed_messages=" << locally_committed_msgs;
	for (auto* batch : release_after_unlock) {
		pubQue_.ReleaseBatch(batch);
	}
	if (locally_committed_msgs > 0) {
		// Prefer the global ORDER=5 ledger; also keep B0 diagnostic counters in sync
		// so per-broker ACK dumps do not look like a head stall after remap.
		broker_stats_[0].acked_messages.fetch_add(locally_committed_msgs, std::memory_order_relaxed);
		ack_received_.fetch_add(locally_committed_msgs, std::memory_order_release);
		LOG(WARNING) << "[SESSION_FENCE_LOCAL_COMMIT]"
		             << " old_epoch=" << old_epoch
		             << " committed_batch_seq=" << fenced.committed_batch_seq()
		             << " credited_msgs=" << locally_committed_msgs;
	}
	const size_t ack_after_local_credit = ack_received_.load(std::memory_order_acquire);
	const size_t rebased_ack_base =
		RebasedAckBaseAfterFenceCredit(ack_base_before_local_credit, locally_committed_msgs);
	CHECK_EQ(rebased_ack_base, ack_after_local_credit);
	// This base anchors the new session's local retire cursor. The head's ACK
	// wire value is already cumulative across sessions (GetClientOrdered), so
	// EpollAckThread must not add this base to the wire value again.
	ack_message_base_.store(rebased_ack_base, std::memory_order_release);
	order5_last_ack_hwm_.store(rebased_ack_base, std::memory_order_release);
	// Collapse post-fence attribution onto the global ledger so Poll/WaitUntilAcked
	// and B0 diagnostics agree even if later ACK deltas arrive on remapped sockets.
	if (!broker_stats_.empty()) {
		broker_stats_[0].acked_messages.store(rebased_ack_base, std::memory_order_relaxed);
	}
	{
		std::lock_guard<std::mutex> lock(unacked_mu_);
		session_retire_prefix_hwm_ = rebased_ack_base;
	}

	// Preserve the retained prefix during recovery by draining it through one
	// surviving PublishThread. Re-striping a large suffix immediately can make
	// batch N+K overtake batch N on a different queue and consume the entire
	// new-session lease before the missing predecessor is dispatched.
	int recovery_broker = -1;
	size_t recovery_queue = std::numeric_limits<size_t>::max();
	{
		absl::MutexLock lock(&mutex_);
		for (int candidate : brokers_) {
			auto it = broker_queue_indices_.find(candidate);
			if (it == broker_queue_indices_.end()) continue;
			for (size_t qidx : it->second) {
				if (!pubQue_.IsQueueActive(qidx)) continue;
				recovery_broker = candidate;
				recovery_queue = qidx;
				break;
			}
			if (recovery_broker >= 0) break;
		}
	}
	if (recovery_broker < 0 ||
	    recovery_queue == std::numeric_limits<size_t>::max()) {
		LOG(ERROR) << "[SESSION_REOPEN_NO_RECOVERY_QUEUE]"
		           << " old_epoch=" << old_epoch
		           << " new_epoch=" << new_epoch;
		shutdown_.store(true, std::memory_order_release);
	}

	uint64_t new_seq = 0;
	size_t requeued_pool_batches = 0;
	size_t direct_resubmit_batches = 0;
	LOG(WARNING) << "[SESSION_REOPEN_RESUBMIT_BEGIN]"
	             << " old_epoch=" << old_epoch
	             << " new_epoch=" << new_epoch
	             << " suffix_batches=" << suffix.size()
	             << " recovery_broker=" << recovery_broker
	             << " recovery_queue=" << recovery_queue;
	for (auto& rec : suffix) {
		if (shutdown_.load(std::memory_order_acquire)) break;
		auto* header = rec.Header();
		if (header == nullptr) continue;
		header->batch_seq = new_seq++;
		header->session_epoch = static_cast<uint16_t>(new_epoch & 0xFFFFU);
		header->session_epoch32 = new_epoch;
		header->client_id = client_id_;
		header->broker_id = static_cast<uint32_t>(recovery_broker);
		if (rec.pool_batch != nullptr) {
			if (!pubQue_.EnqueueBatchForSessionRollover(recovery_queue, header)) {
				LOG(ERROR) << "[SESSION_REOPEN_REQUEUE_FAILED]"
				           << " epoch=" << new_epoch
				           << " batch_seq=" << header->batch_seq
				           << " broker=" << recovery_broker
				           << " queue=" << recovery_queue;
				shutdown_.store(true, std::memory_order_release);
				break;
			}
			NotifyPublisherWork();
			++requeued_pool_batches;
			if ((requeued_pool_batches % 64) == 0 ||
			    requeued_pool_batches == suffix.size()) {
				LOG(WARNING) << "[SESSION_REOPEN_REQUEUE_PROGRESS]"
				             << " epoch=" << new_epoch
				             << " requeued=" << requeued_pool_batches
				             << " suffix_batches=" << suffix.size();
			}
			continue;
		}

		// Owned-copy ACK2 records cannot be placed directly on QueueBuffer's
		// pool queues. Keep the existing backstop path for that uncommon case;
		// ACK1 (the failure experiment) always takes the queue path above.
		if (!SendRawBatchToBroker(
			    rec.WireBytes(), rec.wire_bytes, recovery_broker)) {
			LOG(ERROR) << "[SESSION_REOPEN_DIRECT_RESUBMIT_FAILED]"
			           << " epoch=" << new_epoch
			           << " batch_seq=" << header->batch_seq
			           << " broker=" << recovery_broker;
			shutdown_.store(true, std::memory_order_release);
			break;
		}
		++direct_resubmit_batches;
		rec.current_batch_seq = header->batch_seq;
		rec.session_epoch = new_epoch;
		rec.broker_id = recovery_broker;
		rec.attempt = 1;
		rec.last_send_ns = SteadyNowNs();
		last_unacked_send_ns_.store(rec.last_send_ns, std::memory_order_release);
		std::lock_guard<std::mutex> lock(unacked_mu_);
		session_sent_hwm_ += rec.num_msg;
		rec.broker_ack_end = std::numeric_limits<size_t>::max();
		unacked_bytes_ += rec.wire_bytes;
		unacked_batches_.push_back(std::move(rec));
	}
	pubQue_.SetNextBatchSeqForNewSession(NextBatchSeqAfterSuffixResubmit(static_cast<size_t>(new_seq)));
	session_fenced_reopen_pending_.store(false, std::memory_order_release);
	// Resume the queue for epoch=N+1 publishing.
	pubQue_.ResumeSessionRollover();
	NotifyPublisherWork();
	rollover_guard.dismiss();
	LOG(WARNING) << "[SESSION_REOPEN_RESUBMIT]"
	             << " old_epoch=" << old_epoch
	             << " new_epoch=" << new_epoch
	             << " committed_batch_seq=" << fenced.committed_batch_seq()
	             << " suffix_batches=" << suffix.size()
	             << " requeued_pool_batches=" << requeued_pool_batches
	             << " direct_resubmit_batches=" << direct_resubmit_batches
	             << " recovery_broker=" << recovery_broker
	             << " recovery_queue=" << recovery_queue;
}

void Publisher::RetransmitThread() {
	while (!shutdown_.load(std::memory_order_relaxed)) {
		double delta_ms = DeltaEstimator::kDeltaFloorMs;
		{
			std::lock_guard<std::mutex> lock(delta_mu_);
			delta_ms = delta_estimator_.delta_ms();
		}
		const double sleep_ms = std::min(delta_ms / 2.0, 2.0);
		std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(sleep_ms * 1000.0)));
		if (session_fenced_reopen_pending_.load(std::memory_order_acquire)) {
			continue;
		}

		// During durable ACK2 drain, suppress RTO storms that re-burn BLog for
		// already-ingested batches. Allow a slow backstop after prolonged stall so
		// truly-lost in-flight batches can still recover before the ACK timeout.
		{
			static thread_local int64_t drain_stall_started_ns = 0;
			const bool in_ack2_drain =
				ack_drain_active_.load(std::memory_order_acquire) && ack_level_ >= 2;
			if (!in_ack2_drain) {
				drain_stall_started_ns = 0;
			} else {
				const int64_t now_ns = SteadyNowNs();
				if (drain_stall_started_ns == 0) {
					drain_stall_started_ns = now_ns;
				}
				constexpr int64_t kAckDrainRtoBackstopNs =
					30LL * 1000LL * 1000LL * 1000LL;  // 30s
				if (now_ns - drain_stall_started_ns < kAckDrainRtoBackstopNs) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				// Fall through once for a backstop retransmit pass, then reset.
				drain_stall_started_ns = now_ns;
			}
		}

		const int64_t now_ns = SteadyNowNs();
		struct DueRetransmit {
			std::vector<uint8_t> bytes;
			int broker_id{-1};
			uint64_t current_batch_seq{0};
		};
		std::vector<DueRetransmit> due;
		{
			std::lock_guard<std::mutex> lock(unacked_mu_);
			// ACK1 is a cumulative per-session prefix.  When progress stalls, only
			// the earliest unretired batch can unblock that prefix; retransmitting
			// every later batch cannot advance the ACK.  Snapshotting the complete
			// suffix here used to turn one missing predecessor into an O(window)
			// duplicate burst (hundreds of 512 KiB batches), starving normal ingest
			// and creating a self-amplifying ACK/RTO storm.  Select at most the
			// current predecessor per pass and retain its existing exponential
			// backoff/reroute policy.
			auto it = FindDueSessionRetirePredecessor(
				unacked_batches_.begin(), unacked_batches_.end(),
				session_next_retire_batch_seq_,
				[](const UnackedBatch& candidate) {
					return candidate.current_batch_seq;
				},
				[&](const UnackedBatch& rec) {
					const double backoff_ms = delta_ms * static_cast<double>(
						1ULL << std::min<uint32_t>(rec.attempt, 10));
					const double backstop_ms = std::max(
						backoff_ms, RuntimeSessionRtoFloorMs());
					const int64_t due_ns = static_cast<int64_t>(backstop_ms * 1.0e6);
					// Later suffix sends do not constitute progress for this missing
					// predecessor. Gate only on its own send and cumulative ACK progress.
					const int64_t last_progress_ns = std::max(
						rec.last_send_ns,
						last_ack_progress_ns_.load(std::memory_order_acquire));
					return (last_progress_ns <= 0 || now_ns - last_progress_ns >= due_ns) &&
					       now_ns - rec.last_send_ns >= due_ns;
				});
			if (it != unacked_batches_.end()) {
				auto& rec = *it;
				DueRetransmit item;
				item.broker_id = rec.broker_id;
				item.current_batch_seq = rec.current_batch_seq;
				const void* src = rec.WireBytes();
				if (src != nullptr && rec.wire_bytes > 0) {
					item.bytes.resize(rec.wire_bytes);
					std::memcpy(item.bytes.data(), src, rec.wire_bytes);
					due.push_back(std::move(item));
				}
				rec.attempt++;
				rec.last_send_ns = now_ns;
			}
		}
		for (auto& rec : due) {
			// A due snapshot can contain hundreds of batches. Re-check between
			// sends so rollover does not wait for the entire stale-epoch
			// snapshot to drain while the retransmit thread repeatedly
			// reacquires retransmit_send_mu_.
			if (session_fenced_reopen_pending_.load(std::memory_order_acquire) ||
			    shutdown_.load(std::memory_order_relaxed)) {
				break;
			}
			std::vector<int> survivors;
			{
				absl::MutexLock lock(&mutex_);
				survivors = brokers_;
			}
			const int target = RendezvousBroker(static_cast<uint32_t>(client_id_),
			                                    rec.current_batch_seq,
			                                    survivors,
			                                    rec.broker_id);
			if (target < 0) continue;
			auto* header = reinterpret_cast<Embarcadero::BatchHeader*>(rec.bytes.data());
			header->broker_id = static_cast<uint32_t>(target);
			header->session_epoch = static_cast<uint16_t>(session_epoch_.load(std::memory_order_acquire) & 0xFFFFU);
			header->session_epoch32 = session_epoch_.load(std::memory_order_acquire);
			if (session_fenced_reopen_pending_.load(std::memory_order_acquire)) {
				break;
			}
			if (SendRawBatchToBroker(rec.bytes.data(), rec.bytes.size(), target)) {
				retransmit_attempts_.fetch_add(1, std::memory_order_relaxed);
				last_unacked_send_ns_.store(SteadyNowNs(), std::memory_order_release);
			}
		}
	}
}
