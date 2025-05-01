#pragma once

#include <thread>

#include "../disk_manager/corfu_replication_client.h"
#include "../disk_manager/scalog_replication_client.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "common/config.h"

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "absl/container/btree_set.h"
#include <glog/logging.h>
#include "folly/MPMCQueue.h"

namespace Embarcadero {

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif


/**
 * Callback type for obtaining a new segment
 */
using GetNewSegmentCallback = std::function<void*()>;

/**
 * Class representing a message topic with storage and sequencing capabilities
 */
class Topic {
	public:
		/**
		 * Constructor for a new Topic
		 *
		 * @param get_new_segment_callback Callback function to get new storage segments
		 * @param TInode_addr Address of the topic inode
		 * @param replica_tinode Address of the replica inode (can be nullptr)
		 * @param topic_name Name of the topic
		 * @param broker_id ID of the broker handling this topic
		 * @param order Ordering level for the topic
		 * @param seq_type Type of sequencer to use
		 * @param cxl_addr Base address of CXL memory
		 * @param segment_metadata Pointer to segment metadata
		 */
		Topic(GetNewSegmentCallback get_new_segment_callback,
				GetNumBrokersCallback get_num_brokers_callback,
				GetRegisteredBrokersCallback get_registered_brokers_callback,
				void* TInode_addr, TInode* replica_tinode,
				const char* topic_name, int broker_id, int order,
				heartbeat_system::SequencerType seq_type,
				void* cxl_addr, void* segment_metadata);

		/**
		 * Destructor - ensures all threads are stopped and joined
		 */
		~Topic() {
			stop_threads_ = true;
			for (std::thread& thread : combiningThreads_) {
				if (thread.joinable()) {
					thread.join();
				}
			}
			VLOG(3) << "[Topic]: \tDestructed";
		}

		// Delete copy constructor and copy assignment operator
		Topic(const Topic&) = delete;
		Topic& operator=(const Topic&) = delete;

		bool GetBatchToExport(
				size_t &expected_batch_offset,
				void* &batch_addr,
				size_t &batch_size);
		/**
		 * Get the address and size of messages for a subscriber
		 *
		 * @param last_offset Reference to the last message offset seen by subscriber
		 * @param last_addr Reference to the last message address seen by subscriber
		 * @param messages Reference to store the messages pointer
		 * @param messages_size Reference to store the size of messages
		 * @return true if new messages were found, false otherwise
		 */
		bool GetMessageAddr(size_t& last_offset,
				void*& last_addr,
				void*& messages,
				size_t& messages_size);

		/**
		 * Get a buffer in CXL memory for a new batch of messages
		 *
		 * @param batch_header Reference to the batch header
		 * @param topic Topic name
		 * @param log Reference to store log pointer
		 * @param segment_header Reference to store segment header pointer
		 * @param logical_offset Reference to store logical offset
		 * @return Callback function to execute after writing to the buffer
		 */
		std::function<void(void*, size_t)> GetCXLBuffer(
				struct BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset) {
			return (this->*GetCXLBufferFunc)(batch_header, topic, log, segment_header, logical_offset);
		}

		/**
		 * Get the sequencer type for this topic
		 *
		 * @return The sequencer type
		 */
		heartbeat_system::SequencerType GetSeqtype() const {
			return seq_type_;
		}

		int GetOrder(){ return order_; }

	private:
		/**
		 * Update the TInode's written offset and address
		 */
		inline void UpdateTInodeWritten(size_t written, size_t written_addr);

		/**
		 * Thread function for the message combiner
		 */
		void CombinerThread();

		/**
		 * Check and handle segment boundary crossing
		 */
		void CheckSegmentBoundary(void* log, size_t msgSize, unsigned long long int segment_metadata);

		// Function pointer type for GetCXLBuffer implementations
		using GetCXLBufferFuncPtr = std::function<void(void*, size_t)> (Topic::*)(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset);

		// Pointer to the appropriate GetCXLBuffer implementation
		GetCXLBufferFuncPtr GetCXLBufferFunc;

		// Different implementations of GetCXLBuffer for different sequencer types
		std::function<void(void*, size_t)> KafkaGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset);

		std::function<void(void*, size_t)> CorfuGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset);

		std::function<void(void*, size_t)> ScalogGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset);

		std::function<void(void*, size_t)> Order3GetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset);

		std::function<void(void*, size_t)> Order4GetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset);

		std::function<void(void*, size_t)> EmbarcaderoGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset);

		// Core members
		const GetNewSegmentCallback get_new_segment_callback_;
		const GetNumBrokersCallback get_num_brokers_callback_;
		const GetRegisteredBrokersCallback get_registered_brokers_callback_;
		struct TInode* tinode_;
		struct TInode* replica_tinode_;
		std::string topic_name_;
		int broker_id_;
		struct MessageHeader* last_message_header_;
		int order_;
		int ack_level_;
		heartbeat_system::SequencerType seq_type_;
		void* cxl_addr_;

		// Replication
		std::unique_ptr<Corfu::CorfuReplicationClient> corfu_replication_client_;
		std::unique_ptr<Scalog::ScalogReplicationClient> scalog_replication_client_;

		// Offset tracking
		size_t logical_offset_;
		size_t written_logical_offset_;
		void* written_physical_addr_;
		std::atomic<unsigned long long int> log_addr_;
		unsigned long long int batch_headers_;

		// First message pointers (nullptr if segment is GC'd)
		void* first_message_addr_;
		void* first_batch_headers_addr_;

		// Order 3 specific data structures
		absl::flat_hash_map<size_t, absl::flat_hash_map<size_t, void*>> skipped_batch_ ABSL_GUARDED_BY(mutex_);
		absl::flat_hash_map<size_t, size_t> order3_client_batch_ ABSL_GUARDED_BY(mutex_);

		// Synchronization
		absl::Mutex mutex_;
		absl::Mutex written_mutex_;

		// Kafka specific
		std::atomic<size_t> kafka_logical_offset_{0};
		absl::flat_hash_map<size_t, size_t> written_messages_range_;

		// TInode cache
		int replication_factor_;
		void* ordered_offset_addr_;
		void* current_segment_;
		size_t ordered_offset_;

		// Thread control
		bool stop_threads_ = false;
		std::vector<std::thread> combiningThreads_;

		std::thread sequencerThread_;
		
		// Sequencing
		// Ordered batch vector for efficient subscribe
		std::vector<BatchHeader> ordered_batch_;

		void GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers);
		void Sequencer4();
		void BrokerScannerWorker(int broker_id);
		bool ProcessSkipped(
				absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>>& skipped_batches);
		void AssignOrder(BatchHeader *header, size_t start_total_order);

		size_t global_seq_ = 0;
		absl::flat_hash_map<size_t, size_t> next_expected_batch_seq_;// client_id -> next expected batch_seq
		absl::Mutex global_seq_batch_seq_mu_;;
		folly::MPMCQueue<BatchHeader*> ready_batches_queue_{1024*8};
};
} // End of namespace Embarcadero
