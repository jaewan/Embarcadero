#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../src/client/publisher.h"
#include "../src/client/queue_buffer.h"
#include "../src/common/order_level.h"

using namespace std::chrono_literals;

struct PublisherTestPeer {
    static void ConfigureAckWait(Publisher& publisher, bool session) {
        publisher.ack_level_ = 1;
        publisher.ack_timeout_seconds_ = 1;
        if (!session) publisher.order_level_ = 0;
        publisher.broker_stats_[0].sent_messages.store(3);
    }
    static void AdvanceAck(Publisher& publisher, bool session, size_t value) {
        if (session) publisher.order5_last_ack_hwm_.store(value, std::memory_order_release);
        else publisher.broker_stats_[0].acked_messages.store(value, std::memory_order_release);
        publisher.ack_progress_event_.Notify();
    }
    static void StopAckWait(Publisher& publisher) {
        publisher.shutdown_.store(true, std::memory_order_release);
        publisher.NotifyPublisherWork();
    }
	static void SetPublishAllowlist(Publisher& publisher, std::vector<int> brokers) {
		publisher.order5_broker_allowlist_ = std::move(brokers);
	}
	static void SetHomeBrokers(Publisher& publisher, size_t count) {
		publisher.order5_home_brokers_ = count;
	}
	static bool AckRoutingSupported(const Publisher& publisher, int ack_level) {
		return publisher.HasSupportedOrder5AckRouting(ack_level);
	}

	static void ExpectNotStarted(const Publisher& publisher) {
		EXPECT_FALSE(publisher.ack_thread_.joinable());
		EXPECT_FALSE(publisher.retransmit_thread_.joinable());
		EXPECT_FALSE(publisher.cluster_probe_thread_.joinable());
		EXPECT_TRUE(publisher.threads_.empty());
		EXPECT_EQ(publisher.thread_count_.load(), 0);
	}

	static size_t PoolBytes(Publisher& publisher) {
		return publisher.pubQue_.PoolBytes();
	}

	static void ConfigureOrder5Session(Publisher& publisher, uint32_t epoch) {
		publisher.ack_level_ = 1;
		publisher.session_epoch_.store(epoch, std::memory_order_release);
		publisher.requested_session_epoch_.store(epoch, std::memory_order_release);
		publisher.pubQue_.SetActiveQueues(1);
		ASSERT_TRUE(publisher.pubQue_.AddBuffers(0));
	}

    static void FinishInput(Publisher& publisher) {
        publisher.pubQue_.WriteFinished();
        publisher.publish_finished_.store(true, std::memory_order_release);
        publisher.consumer_should_exit_.store(true, std::memory_order_release);
    }
    static Embarcadero::BatchHeader* ReadWorker(Publisher& publisher) {
        return publisher.ReadPublishBatch(0);
    }
    static void Requeue(Publisher& publisher, Embarcadero::BatchHeader* batch) {
        publisher.pubQue_.PauseSessionRollover();
        const bool queued = publisher.pubQue_.EnqueueBatchForSessionRollover(0, batch);
        publisher.pubQue_.ResumeSessionRollover();
        publisher.NotifyPublisherWork();
        EXPECT_TRUE(queued);
    }
    static void StopWorkers(Publisher& publisher) {
        publisher.publisher_workers_stop_.store(true, std::memory_order_release);
        publisher.NotifyPublisherWork();
    }

	static void SetAckLevel(Publisher& publisher, int ack_level) {
		publisher.ack_level_ = ack_level;
	}

	static void ReleasePoolBatch(Publisher& publisher, Embarcadero::BatchHeader* batch) {
		publisher.pubQue_.ReleaseBatch(batch);
	}

	static Embarcadero::BatchHeader* SealOneBatch(Publisher& publisher,
	                                              uint32_t epoch,
	                                              uint64_t batch_seq,
	                                              size_t messages) {
		std::vector<char> payload(publisher.message_size_, 'x');
		size_t sealed = 0;
		for (size_t i = 0; i < messages; ++i) {
			if (!publisher.pubQue_.Write(i, payload.data(), payload.size(), payload.size(), sealed)) {
				return nullptr;
			}
		}
		publisher.pubQue_.SealAll();
		auto* batch = static_cast<Embarcadero::BatchHeader*>(publisher.pubQue_.Read(0));
		EXPECT_NE(batch, nullptr);
		if (batch == nullptr) return nullptr;
		batch->client_id = publisher.client_id_;
		batch->broker_id = 0;
		batch->batch_seq = batch_seq;
		batch->session_epoch = static_cast<uint16_t>(epoch & 0xFFFFU);
		batch->session_epoch32 = epoch;
		return batch;
	}

	static bool Record(Publisher& publisher, Embarcadero::BatchHeader* batch) {
		const size_t wire_bytes = sizeof(Embarcadero::BatchHeader) + batch->total_size;
		return publisher.RecordUnackedBatch(*batch, batch, wire_bytes, 0, 0);
	}

	static void Fence(Publisher& publisher, uint64_t committed_batch_seq) {
		embarcadero::session::SessionFenced fenced;
		fenced.set_committed_batch_seq(committed_batch_seq);
		fenced.set_has_committed_prefix(true);
		fenced.set_committed_msg_hwm(0);
		fenced.set_control_epoch(2);
		fenced.set_reason(embarcadero::session::SessionFenced::HOLD_EXPIRY);
		publisher.HandleSessionFenced(fenced, 0);
	}

	static size_t UnackedOwnedCopies(Publisher& publisher) {
		std::lock_guard<std::mutex> lock(publisher.unacked_mu_);
		size_t n = 0;
		for (const auto& rec : publisher.unacked_batches_) {
			if (!rec.wire.empty()) ++n;
		}
		return n;
	}

	static size_t UnackedPoolPins(Publisher& publisher) {
		std::lock_guard<std::mutex> lock(publisher.unacked_mu_);
		size_t n = 0;
		for (const auto& rec : publisher.unacked_batches_) {
			if (rec.pool_batch != nullptr) ++n;
		}
		return n;
	}

	static void Complete(Publisher& publisher, size_t session_global_ack) {
		publisher.CompleteUnackedThrough(0, session_global_ack);
	}

	static size_t AckReceived(const Publisher& publisher) {
		return publisher.ack_received_.load(std::memory_order_acquire);
	}

	static size_t AckBase(const Publisher& publisher) {
		return publisher.ack_message_base_.load(std::memory_order_acquire);
	}

	static uint32_t Epoch(const Publisher& publisher) {
		return publisher.session_epoch_.load(std::memory_order_acquire);
	}

	static void SetClientOrder(Publisher& publisher, size_t value) {
		publisher.client_order_.store(value, std::memory_order_release);
	}

	static size_t UnackedBatches(Publisher& publisher) {
		std::lock_guard<std::mutex> lock(publisher.unacked_mu_);
		return publisher.unacked_batches_.size();
	}

	static void InstallRetransmitChannel(Publisher& publisher, int broker_id, int fd) {
		std::lock_guard<std::mutex> lock(publisher.retransmit_channel_mu_);
		publisher.retransmit_channels_[broker_id] = fd;
	}

	static size_t RetransmitChannels(Publisher& publisher) {
		std::lock_guard<std::mutex> lock(publisher.retransmit_channel_mu_);
		return publisher.retransmit_channels_.size();
	}

	static void MapQueueToBroker(Publisher& publisher, int broker_id, size_t queue_idx) {
		absl::MutexLock lock(&publisher.mutex_);
		publisher.brokers_ = {broker_id};
		publisher.nodes_[broker_id] = "127.0.0.1:1212";
		publisher.broker_queue_indices_[broker_id] = {queue_idx};
	}

	static Embarcadero::BatchHeader* ReadQueue(Publisher& publisher, int queue_idx) {
		return static_cast<Embarcadero::BatchHeader*>(publisher.pubQue_.Read(queue_idx));
	}
};

struct QueueBufferTestPeer {
	static bool Begin(QueueBuffer& queue) {
		return queue.BeginProducerOp();
	}

	static void End(QueueBuffer& queue) {
		queue.EndProducerOp();
	}
};

namespace {

TEST(Order5PublisherRoutingTest, FollowerOnlyAcknowledgedInitRejectsBeforeWorkersOrPool) {
	char topic[TOPIC_NAME_SIZE] = {};
	std::strncpy(topic, "RoutingAdmission", sizeof(topic) - 1);
	for (int ack_level : {1, 2}) {
		Publisher publisher(topic, "127.0.0.1", "1", 1, 64, 1 << 20,
		                    Embarcadero::kOrderStrong,
		                    heartbeat_system::SequencerType::EMBARCADERO);
		PublisherTestPeer::SetPublishAllowlist(publisher, {1, 2});
		const size_t pool_before = PublisherTestPeer::PoolBytes(publisher);
		EXPECT_FALSE(publisher.Init(ack_level));
		PublisherTestPeer::ExpectNotStarted(publisher);
		EXPECT_EQ(PublisherTestPeer::PoolBytes(publisher), pool_before);
		PublisherTestPeer::SetPublishAllowlist(publisher, {});
		PublisherTestPeer::SetHomeBrokers(publisher, 1);
		EXPECT_TRUE(PublisherTestPeer::AckRoutingSupported(publisher, 0));
		EXPECT_FALSE(publisher.Init(ack_level));
		PublisherTestPeer::ExpectNotStarted(publisher);
		EXPECT_EQ(PublisherTestPeer::PoolBytes(publisher), pool_before);
	}
}

TEST(Order5PublisherRoutingTest, GuardPreservesDefaultExplicitHeadAndUnacknowledgedModes) {
	char topic[TOPIC_NAME_SIZE] = {};
	for (const auto seq : {heartbeat_system::SequencerType::EMBARCADERO,
	                       heartbeat_system::SequencerType::CORFU}) {
		for (const int order : {1, Embarcadero::kOrderStrong}) {
			Publisher publisher(topic, "127.0.0.1", "1", 1, 64, 1 << 20, order, seq);
			PublisherTestPeer::SetPublishAllowlist(publisher, {});
			PublisherTestPeer::SetHomeBrokers(publisher, 0);
			EXPECT_TRUE(PublisherTestPeer::AckRoutingSupported(publisher, 1));
			EXPECT_TRUE(PublisherTestPeer::AckRoutingSupported(publisher, 2));
			PublisherTestPeer::SetPublishAllowlist(publisher, {1, 2});
			EXPECT_TRUE(PublisherTestPeer::AckRoutingSupported(publisher, 0));
			if (seq != heartbeat_system::SequencerType::EMBARCADERO ||
			    order != Embarcadero::kOrderStrong) {
				EXPECT_TRUE(PublisherTestPeer::AckRoutingSupported(publisher, 1));
				EXPECT_TRUE(PublisherTestPeer::AckRoutingSupported(publisher, 2));
			}
			PublisherTestPeer::SetPublishAllowlist(publisher, {0, 2});
			PublisherTestPeer::SetHomeBrokers(publisher, 1);
			EXPECT_TRUE(PublisherTestPeer::AckRoutingSupported(publisher, 1));
			EXPECT_TRUE(PublisherTestPeer::AckRoutingSupported(publisher, 2));
		}
	}
}

TEST(Order5PublisherRetryPolicyTest, OverdueSuffixSelectsOnlyRetireCursorPredecessor) {
	struct Candidate {
		uint64_t batch_seq;
		bool overdue;
	};
	// Every record is overdue.  Even if the container is not sorted, only the
	// missing predecessor at the retire cursor is eligible; suffix records do
	// not advance a cumulative ACK.
	std::vector<Candidate> unacked{{9, true}, {7, true}, {8, true}};
	auto selected = FindDueSessionRetirePredecessor(
		unacked.begin(), unacked.end(), 7,
		[](const Candidate& candidate) { return candidate.batch_seq; },
		[](const Candidate& candidate) { return candidate.overdue; });
	ASSERT_NE(selected, unacked.end());
	EXPECT_TRUE(selected->overdue);
	EXPECT_EQ(selected->batch_seq, 7u);

	selected = FindDueSessionRetirePredecessor(
		unacked.begin(), unacked.end(), 6,
		[](const Candidate& candidate) { return candidate.batch_seq; },
		[](const Candidate& candidate) { return candidate.overdue; });
	EXPECT_EQ(selected, unacked.end())
		<< "a suffix must not substitute for a missing retire-cursor batch";

	unacked[1].overdue = false;
	selected = FindDueSessionRetirePredecessor(
		unacked.begin(), unacked.end(), 7,
		[](const Candidate& candidate) { return candidate.batch_seq; },
		[](const Candidate& candidate) { return candidate.overdue; });
	EXPECT_EQ(selected, unacked.end())
		<< "an overdue suffix must not substitute for a predecessor that is not due";
}

TEST(Order5PublisherRetryPolicyTest, ReadinessWithoutProgressStillExpiresDeadline) {
	using Clock = std::chrono::steady_clock;
	const auto start = Clock::time_point{};
	const auto timeout = 30ms;

	// Model repeated readiness notifications followed by EAGAIN.  Readiness is
	// deliberately not an input to the predicate: without byte progress it must
	// not reset or suppress the wall-clock deadline.
	for (int readiness_events = 1; readiness_events < 30; ++readiness_events) {
		EXPECT_FALSE(HeaderSendNoProgressExpired(
			start + std::chrono::milliseconds(readiness_events), start, timeout));
	}
	EXPECT_TRUE(HeaderSendNoProgressExpired(start + timeout, start, timeout));
	EXPECT_TRUE(HeaderSendNoProgressExpired(start + 10 * timeout, start, timeout));

	const auto progressed = start + 25ms;
	EXPECT_FALSE(HeaderSendNoProgressExpired(start + 40ms, progressed, timeout))
		<< "actual byte progress resets the no-progress interval";
}

TEST(Order5PublisherRolloverTest, QueueBufferPauseCannotMissProducerEndWakeup) {
	setenv("EMBAR_USE_HUGETLB", "0", 1);
	QueueBuffer queue(/*num_buf=*/1, /*num_threads_per_broker=*/1, /*client_id=*/17,
	                  /*message_size=*/64, Embarcadero::kOrderStrong);
	ASSERT_TRUE(queue.AddBuffers(0));
	queue.SetActiveQueues(1);

	std::atomic<size_t> writes{0};
	std::atomic<bool> stop_consumer{false};
	std::thread consumer([&]() {
		while (!stop_consumer.load(std::memory_order_acquire)) {
			if (auto* batch = queue.Read(0)) {
				queue.ReleaseBatch(batch);
			} else {
				std::this_thread::yield();
			}
		}
		while (auto* batch = queue.Read(0)) {
			queue.ReleaseBatch(batch);
		}
	});
	auto producer_burst = [&]() {
		std::vector<char> payload(64, 'p');
		for (size_t i = 0; i < 16; ++i) {
			size_t sealed = 0;
			if (!queue.Write(writes.load(std::memory_order_relaxed),
			                 payload.data(),
			                 payload.size(),
			                 payload.size(),
			                 sealed)) {
				break;
			}
			writes.fetch_add(1, std::memory_order_relaxed);
			std::this_thread::yield();
		}
	};

	for (size_t i = 0; i < 256; ++i) {
		auto producer = std::async(std::launch::async, producer_burst);
		auto paused = std::async(std::launch::async, [&]() {
			queue.PauseSessionRollover();
			queue.SealAllForSessionRollover();
			queue.ResumeSessionRollover();
		});
		if (paused.wait_for(1s) != std::future_status::ready) {
			queue.ReturnReads();
			paused.wait();
			stop_consumer.store(true, std::memory_order_release);
			if (consumer.joinable()) consumer.join();
			FAIL() << "PauseSessionRollover missed the producer active->0 wakeup";
		}
		paused.get();
		ASSERT_EQ(producer.wait_for(1s), std::future_status::ready);
		producer.get();
	}

	queue.WriteFinished();
	stop_consumer.store(true, std::memory_order_release);
	consumer.join();
	while (auto* batch = queue.Read(0)) {
		queue.ReleaseBatch(batch);
	}
	queue.ReturnReads();
	EXPECT_GT(writes.load(std::memory_order_relaxed), 0u);
}

TEST(Order5PublisherRolloverTest, PauseRolloverSerializesProducerExit) {
	setenv("EMBAR_USE_HUGETLB", "0", 1);
	QueueBuffer queue(/*num_buf=*/1, /*num_threads_per_broker=*/1, /*client_id=*/23,
	                  /*message_size=*/64, Embarcadero::kOrderStrong);
	ASSERT_TRUE(queue.AddBuffers(0));
	queue.SetActiveQueues(1);

	ASSERT_TRUE(QueueBufferTestPeer::Begin(queue));

	auto paused = std::async(std::launch::async, [&]() {
		queue.PauseSessionRollover();
		queue.SealAllForSessionRollover();
		queue.ResumeSessionRollover();
	});
	EXPECT_EQ(paused.wait_for(5ms), std::future_status::timeout)
		<< "pauser should wait while a producer op is active";

	QueueBufferTestPeer::End(queue);
	ASSERT_EQ(paused.wait_for(1s), std::future_status::ready)
		<< "pauser must observe the producer active->0 transition";
	paused.get();

	queue.WriteFinished();
	while (auto* batch = queue.Read(0)) {
		queue.ReleaseBatch(batch);
	}
	queue.ReturnReads();
}

TEST(Order5PublisherRolloverTest, PublisherFenceAndRetireUseProductionState) {
	setenv("EMBAR_USE_HUGETLB", "0", 1);
	setenv("NUM_BROKERS", "1", 1);
	char topic[TOPIC_NAME_SIZE] = {};
	std::strncpy(topic, "TestTopic", sizeof(topic) - 1);
	Publisher publisher(topic,
	                    "127.0.0.1",
	                    "1212",
	                    /*num_threads_per_broker=*/1,
	                    /*message_size=*/64,
	                    /*queueSize=*/1 << 20,
	                    Embarcadero::kOrderStrong,
	                    heartbeat_system::SequencerType::EMBARCADERO);
	PublisherTestPeer::ConfigureOrder5Session(publisher, 1);

	auto* committed = PublisherTestPeer::SealOneBatch(publisher, 1, 0, 8);
	ASSERT_NE(committed, nullptr);
	ASSERT_TRUE(PublisherTestPeer::Record(publisher, committed));
	PublisherTestPeer::SetClientOrder(publisher, committed->num_msg);

	PublisherTestPeer::Fence(publisher, 0);
	const size_t committed_msgs = PublisherTestPeer::AckReceived(publisher);
	EXPECT_EQ(committed_msgs, committed->num_msg);
	EXPECT_EQ(PublisherTestPeer::AckBase(publisher), committed_msgs);
	EXPECT_EQ(PublisherTestPeer::Epoch(publisher), 2u);
	EXPECT_EQ(PublisherTestPeer::UnackedBatches(publisher), 0u);

	auto* next = PublisherTestPeer::SealOneBatch(publisher, 2, 0, 5);
	ASSERT_NE(next, nullptr);
	ASSERT_TRUE(PublisherTestPeer::Record(publisher, next));
	EXPECT_EQ(PublisherTestPeer::UnackedBatches(publisher), 1u);

	PublisherTestPeer::Complete(publisher, committed_msgs + next->num_msg);
	EXPECT_EQ(PublisherTestPeer::UnackedBatches(publisher), 0u);
	publisher.WriteFinishedOrPaused();
}

TEST(Order5PublisherRolloverTest, FenceInvalidatesEpochBoundRetransmitChannels) {
	setenv("EMBAR_USE_HUGETLB", "0", 1);
	setenv("NUM_BROKERS", "1", 1);
	char topic[TOPIC_NAME_SIZE] = {};
	std::strncpy(topic, "TestTopicEpochChannel", sizeof(topic) - 1);
	Publisher publisher(topic,
	                    "127.0.0.1",
	                    "1212",
	                    /*num_threads_per_broker=*/1,
	                    /*message_size=*/64,
	                    /*queueSize=*/1 << 20,
	                    Embarcadero::kOrderStrong,
	                    heartbeat_system::SequencerType::EMBARCADERO);
	PublisherTestPeer::ConfigureOrder5Session(publisher, 1);

	int sockets[2] = {-1, -1};
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
	PublisherTestPeer::InstallRetransmitChannel(publisher, 0, sockets[0]);
	ASSERT_EQ(PublisherTestPeer::RetransmitChannels(publisher), 1u);

	PublisherTestPeer::Fence(publisher, 0);
	EXPECT_EQ(PublisherTestPeer::Epoch(publisher), 2u);
	EXPECT_EQ(PublisherTestPeer::RetransmitChannels(publisher), 0u);
	EXPECT_EQ(close(sockets[0]), -1) << "publisher must close the old-epoch fd";
	close(sockets[1]);
	publisher.WriteFinishedOrPaused();
}

TEST(Order5PublisherRolloverTest, FenceRequeuesAck1SuffixOnNormalPublishQueue) {
	setenv("EMBAR_USE_HUGETLB", "0", 1);
	setenv("NUM_BROKERS", "1", 1);
	char topic[TOPIC_NAME_SIZE] = {};
	std::strncpy(topic, "TestTopicSuffixQueue", sizeof(topic) - 1);
	Publisher publisher(topic,
	                    "127.0.0.1",
	                    "1212",
	                    /*num_threads_per_broker=*/1,
	                    /*message_size=*/64,
	                    /*queueSize=*/1 << 20,
	                    Embarcadero::kOrderStrong,
	                    heartbeat_system::SequencerType::EMBARCADERO);
	PublisherTestPeer::ConfigureOrder5Session(publisher, 1);
	PublisherTestPeer::MapQueueToBroker(publisher, 0, 0);

	auto* committed = PublisherTestPeer::SealOneBatch(publisher, 1, 0, 4);
	ASSERT_NE(committed, nullptr);
	ASSERT_TRUE(PublisherTestPeer::Record(publisher, committed));
	auto* suffix = PublisherTestPeer::SealOneBatch(publisher, 1, 1, 5);
	ASSERT_NE(suffix, nullptr);
	ASSERT_TRUE(PublisherTestPeer::Record(publisher, suffix));
	PublisherTestPeer::SetClientOrder(
		publisher, committed->num_msg + suffix->num_msg);

	PublisherTestPeer::Fence(publisher, 0);
	EXPECT_EQ(PublisherTestPeer::Epoch(publisher), 2u);
	EXPECT_EQ(PublisherTestPeer::UnackedBatches(publisher), 0u)
		<< "normal PublishThread must re-record the requeued suffix";

	auto* requeued = PublisherTestPeer::ReadQueue(publisher, 0);
	ASSERT_EQ(requeued, suffix);
	EXPECT_EQ(requeued->session_epoch32, 2u);
	EXPECT_EQ(requeued->batch_seq, 0u);
	PublisherTestPeer::ReleasePoolBatch(publisher, requeued);
	publisher.WriteFinishedOrPaused();
}

TEST(Order5PublisherRolloverTest, Ack2RecordsOwnedCopyWithoutPoolPin) {
	setenv("EMBAR_USE_HUGETLB", "0", 1);
	setenv("NUM_BROKERS", "1", 1);
	unsetenv("EMBARCADERO_CHAIN_REPLICATION_SINK");
	unsetenv("EMBARCADERO_CHAIN_REPLICATION_INMEM");
	setenv("EMBARCADERO_ACK2_RETENTION", "owned_rto_copy", 1);
	char topic[TOPIC_NAME_SIZE] = {};
	std::strncpy(topic, "TestTopicAck2", sizeof(topic) - 1);
	Publisher publisher(topic,
	                    "127.0.0.1",
	                    "1212",
	                    /*num_threads_per_broker=*/1,
	                    /*message_size=*/64,
	                    /*queueSize=*/1 << 20,
	                    Embarcadero::kOrderStrong,
	                    heartbeat_system::SequencerType::EMBARCADERO);
	PublisherTestPeer::ConfigureOrder5Session(publisher, 1);
	PublisherTestPeer::SetAckLevel(publisher, 2);

	auto* batch = PublisherTestPeer::SealOneBatch(publisher, 1, 0, 4);
	ASSERT_NE(batch, nullptr);
	const size_t num_msg = batch->num_msg;
	const bool owns_pool = PublisherTestPeer::Record(publisher, batch);
	EXPECT_FALSE(owns_pool);
	EXPECT_EQ(PublisherTestPeer::UnackedBatches(publisher), 1u);
	EXPECT_EQ(PublisherTestPeer::UnackedOwnedCopies(publisher), 1u);
	EXPECT_EQ(PublisherTestPeer::UnackedPoolPins(publisher), 0u);
	// Caller releases the send slot immediately under disk ACK2.
	PublisherTestPeer::ReleasePoolBatch(publisher, batch);
	PublisherTestPeer::Complete(publisher, num_msg);
	EXPECT_EQ(PublisherTestPeer::UnackedBatches(publisher), 0u);
	publisher.WriteFinishedOrPaused();
	unsetenv("EMBARCADERO_ACK2_RETENTION");
}

TEST(Order5PublisherRolloverTest, Ack2MemorySinkPinsPoolWithoutOwnedCopy) {
	setenv("EMBAR_USE_HUGETLB", "0", 1);
	setenv("NUM_BROKERS", "1", 1);
	setenv("EMBARCADERO_CHAIN_REPLICATION_SINK", "memory-copy", 1);
	unsetenv("EMBARCADERO_ACK2_RETENTION");
	char topic[TOPIC_NAME_SIZE] = {};
	std::strncpy(topic, "TestTopicAck2Mem", sizeof(topic) - 1);
	Publisher publisher(topic,
	                    "127.0.0.1",
	                    "1212",
	                    /*num_threads_per_broker=*/1,
	                    /*message_size=*/64,
	                    /*queueSize=*/1 << 20,
	                    Embarcadero::kOrderStrong,
	                    heartbeat_system::SequencerType::EMBARCADERO);
	PublisherTestPeer::ConfigureOrder5Session(publisher, 1);
	PublisherTestPeer::SetAckLevel(publisher, 2);

	auto* batch = PublisherTestPeer::SealOneBatch(publisher, 1, 0, 4);
	ASSERT_NE(batch, nullptr);
	const size_t num_msg = batch->num_msg;
	const bool owns_pool = PublisherTestPeer::Record(publisher, batch);
	EXPECT_TRUE(owns_pool);
	EXPECT_EQ(PublisherTestPeer::UnackedBatches(publisher), 1u);
	EXPECT_EQ(PublisherTestPeer::UnackedOwnedCopies(publisher), 0u);
	EXPECT_EQ(PublisherTestPeer::UnackedPoolPins(publisher), 1u);
	PublisherTestPeer::Complete(publisher, num_msg);
	EXPECT_EQ(PublisherTestPeer::UnackedBatches(publisher), 0u);
	publisher.WriteFinishedOrPaused();
	unsetenv("EMBARCADERO_CHAIN_REPLICATION_SINK");
}

}  // namespace

TEST(Order5PublisherRollover, FinishedInputStillConsumesRecoveryAndStopsWithoutAnotherBatch) {
    setenv("NUM_BROKERS", "1", 1);
    char topic[TOPIC_NAME_SIZE] = {};
    std::strncpy(topic, "RecoveryWorker", sizeof(topic) - 1);
    Publisher publisher(topic, "127.0.0.1", "1212", 1, 64, 1 << 20,
                        Embarcadero::kOrderStrong, heartbeat_system::SequencerType::EMBARCADERO);
    PublisherTestPeer::ConfigureOrder5Session(publisher, 1);
    auto* retained = PublisherTestPeer::SealOneBatch(publisher, 1, 0, 2);
    ASSERT_NE(retained, nullptr);
    PublisherTestPeer::FinishInput(publisher);
    auto worker = std::async(std::launch::async, [&] { return PublisherTestPeer::ReadWorker(publisher); });
    // Input completion alone must not terminate the actual sender read path.
    const bool stayed_available = worker.wait_for(20ms) == std::future_status::timeout;
    PublisherTestPeer::Requeue(publisher, retained);
    const bool woke = worker.wait_for(1s) == std::future_status::ready;
    if (!woke) PublisherTestPeer::StopWorkers(publisher);
    auto* consumed = worker.get();
    EXPECT_TRUE(stayed_available);
    EXPECT_TRUE(woke);
    EXPECT_EQ(consumed, retained);
    if (consumed) PublisherTestPeer::ReleasePoolBatch(publisher, consumed);
    auto stopping = std::async(std::launch::async, [&] { return PublisherTestPeer::ReadWorker(publisher); });
    PublisherTestPeer::StopWorkers(publisher);
    EXPECT_EQ(stopping.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(stopping.get(), nullptr);
}

TEST(AdaptiveAckWait, ProgressBeforeRegistrationAndDuringPredicateCheck) {
    Embarcadero::AdaptiveWaitEvent event;
    std::atomic<bool> ready{true};
    event.Notify();
    event.WaitUntil(std::chrono::steady_clock::now() + 1s, [&] { return ready.load(); });
    ready.store(false);
    std::promise<void> predicate_entered, updated;
    auto update_done = updated.get_future();
    auto writer = std::async(std::launch::async, [&] {
        predicate_entered.get_future().wait();
        ready.store(true, std::memory_order_release);
        updated.set_value();
        event.Notify(); // Must wait for registration's mutex before notifying.
    });
    bool first = true;
    const auto started = std::chrono::steady_clock::now();
    event.WaitUntil(started + 1s, [&] {
        if (first) {
            first = false;
            predicate_entered.set_value();
            update_done.wait();
            return false; // Force the check-to-park race despite published progress.
        }
        return ready.load(std::memory_order_acquire);
    });
    writer.get();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_TRUE(ready.load());
}

TEST(AdaptiveAckWait, BoundedFallbackWithoutNotification) {
    Embarcadero::AdaptiveWaitEvent event;
    const auto start = std::chrono::steady_clock::now();
    event.WaitUntil(start + 2ms, [] { return false; });
    EXPECT_GE(std::chrono::steady_clock::now() - start, 2ms);
}

TEST(Order5PublisherRollover, AckWaitUsesAuthoritativeFrontierAndLegacyNormalizer) {
    for (bool session : {true, false}) {
        char topic[TOPIC_NAME_SIZE] = "AckEvent";
        Publisher publisher(topic, "127.0.0.1", "1212", 1, 64, 1 << 20,
                            Embarcadero::kOrderStrong, heartbeat_system::SequencerType::EMBARCADERO);
        PublisherTestPeer::ConfigureAckWait(publisher, session);
        auto waiter = std::async(std::launch::async, [&] { return publisher.WaitUntilAcked(3); });
        EXPECT_EQ(waiter.wait_for(10ms), std::future_status::timeout);
        PublisherTestPeer::AdvanceAck(publisher, session, 2);
        EXPECT_EQ(waiter.wait_for(10ms), std::future_status::timeout);
        PublisherTestPeer::AdvanceAck(publisher, session, 3);
        EXPECT_EQ(waiter.wait_for(500ms), std::future_status::ready);
        EXPECT_TRUE(waiter.get()); // Raw ack_received_ deliberately remains zero.
        EXPECT_TRUE(publisher.WaitUntilAcked(3)); // Completion before entering wait.
        auto stopped = std::async(std::launch::async, [&] { return publisher.WaitUntilAcked(4); });
        PublisherTestPeer::StopAckWait(publisher);
        EXPECT_EQ(stopped.wait_for(500ms), std::future_status::ready);
        EXPECT_FALSE(stopped.get());
    }
}
