#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "../src/client/publisher.h"
#include "../src/client/queue_buffer.h"
#include "../src/common/order_level.h"

using namespace std::chrono_literals;

struct PublisherTestPeer {
	static void ConfigureOrder5Session(Publisher& publisher, uint32_t epoch) {
		publisher.ack_level_ = 1;
		publisher.session_epoch_.store(epoch, std::memory_order_release);
		publisher.requested_session_epoch_.store(epoch, std::memory_order_release);
		publisher.pubQue_.SetActiveQueues(1);
		ASSERT_TRUE(publisher.pubQue_.AddBuffers(0));
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
		return publisher.RecordUnackedBatch(*batch, batch, batch->total_size, 0, 0);
	}

	static void Fence(Publisher& publisher, uint64_t committed_batch_seq) {
		embarcadero::session::SessionFenced fenced;
		fenced.set_committed_batch_seq(committed_batch_seq);
		fenced.set_committed_msg_hwm(0);
		fenced.set_control_epoch(2);
		fenced.set_reason(embarcadero::session::SessionFenced::HOLD_EXPIRY);
		publisher.HandleSessionFenced(fenced, 0);
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

}  // namespace
