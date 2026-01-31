#pragma once

#include <thread>
#include <atomic>
#include <utility>
#include <bitset>
#include <map>
#include <vector>
#include <deque>

#include "../disk_manager/corfu_replication_client.h"
#include "../disk_manager/scalog_replication_client.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "common/config.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"
#include "absl/container/btree_set.h"
#include <glog/logging.h>
#include "folly/MPMCQueue.h"

namespace Embarcadero {

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

// [[PHASE_1B]] Epoch-batched sequencing + Level 5 hold buffer (design §3.2)
struct PendingBatch5 {
	struct BatchHeader* hdr{nullptr};
	int broker_id{0};
	uint32_t num_msg{0};
	size_t client_id{0};
	size_t batch_seq{0};
	size_t slot_offset{0};  // For commit order (consumed_through per broker)
	uint16_t epoch_created{0};  // [[PHASE_1A]] For sequencer-side epoch validation (§4.2)
};
struct ClientState5 {
	uint64_t next_expected{0};
	uint64_t highest_sequenced{0};
	std::bitset<128> recent_window;  // Duplicate detection
};
struct HoldEntry5 {
	PendingBatch5 batch;
	int wait_epochs{0};
};

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
		for (std::thread& thread : delegationThreads_) {
			if (thread.joinable()) {
				thread.join();
			}
		}

			if(sequencerThread_.joinable()){
				sequencerThread_.join();
			}

			// [[PHASE_3]] Join GOI recovery thread if running
			if(goi_recovery_thread_.joinable()){
				goi_recovery_thread_.join();
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
		bool GetBatchToExportWithMetadata(
				size_t &expected_batch_offset,
				void* &batch_addr,
				size_t &batch_size,
				size_t &batch_total_order,
				uint32_t &num_messages);
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
				size_t& logical_offset,
				BatchHeader*& batch_header_location) {
			return (this->*GetCXLBufferFunc)(batch_header, topic, log, segment_header, logical_offset, batch_header_location);
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
	 * DelegationThread: Stage 2 (Local Ordering)
	 * Purpose: Assign local per-broker sequence numbers to messages
	 * This is for Corfu, Scalog, and Embarcadero weak ordering
	 * 
	 * Processing pipeline:
	 * 1. Poll received flag (set by Receiver)
	 * 2. Assign local counter (per-broker sequence)
	 * 3. Update Bmeta.local.processed_ptr
	 * 4. Flush cache line (bytes 16-31 only)
	 */
	void DelegationThread();

		/**
		 * Check and handle segment boundary crossing
		 */
		void CheckSegmentBoundary(void* log, size_t msgSize, unsigned long long int segment_metadata);

		/**
		 * [[PHASE_1A_EPOCH_FENCING]] Refresh broker's view of ControlBlock.epoch from CXL.
		 * When force_full_read is true: flush CXL cache line, load fence, read epoch, update broker_epoch_.
		 * When false: return current broker_epoch_ without CXL read (periodic check optimization).
		 *
		 * @param force_full_read If true, read from CXL; if false, use cached broker_epoch_.
		 * @return (epoch_to_use_for_epoch_created, was_stale). If was_stale, caller must refuse batch (§4.2.1).
		 */
		std::pair<uint64_t, bool> RefreshBrokerEpochFromCXL(bool force_full_read);

		void StartScalogLocalSequencer();

		// Function pointer type for GetCXLBuffer implementations
		using GetCXLBufferFuncPtr = std::function<void(void*, size_t)> (Topic::*)(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location);

		// Pointer to the appropriate GetCXLBuffer implementation
		GetCXLBufferFuncPtr GetCXLBufferFunc;

		// Different implementations of GetCXLBuffer for different sequencer types
		std::function<void(void*, size_t)> KafkaGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location);

		std::function<void(void*, size_t)> CorfuGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location);

		std::function<void(void*, size_t)> ScalogGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location);

		std::function<void(void*, size_t)> Order3GetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location);

		std::function<void(void*, size_t)> Order4GetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location);

		std::function<void(void*, size_t)> EmbarcaderoGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location);

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
		/** Raw CXL base (ControlBlock at offset 0). Same as CXLManager::GetCXLAddr(); Topic receives this from TopicManager. */
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

		// Ring buffer metrics
		std::atomic<uint64_t> ring_full_count_{0};
		std::atomic<uint64_t> ring_full_last_log_time_{0};

		// TInode cache
		int replication_factor_;
		void* ordered_offset_addr_;
		void* current_segment_;
		// [[SEQUENCER_ONLY_HEAD_NODE]] True when this Topic runs only the sequencer (no local data path)
		// Detected from segment_metadata==nullptr && broker_id==0. Must be declared after current_segment_
		// to match initializer list order.
		bool is_sequencer_only_;
		size_t ordered_offset_;

		// Thread control
		bool stop_threads_ = false;
		std::vector<std::thread> delegationThreads_;

		std::thread sequencerThread_;

		// [[FIX: B3=0 ACKs]] Dynamic scanner management for late-registering brokers
		// Tracks which brokers have BrokerScannerWorker5 threads to enable dynamic addition
		absl::flat_hash_set<int> brokers_with_scanners_ ABSL_GUARDED_BY(scanner_management_mu_);
		std::vector<std::thread> scanner_threads_ ABSL_GUARDED_BY(scanner_management_mu_);
		absl::Mutex scanner_management_mu_;
		void CheckAndSpawnNewScanners();  // Called periodically to add scanners for new brokers
		
		uint32_t local_counter_ = 0; // threading: single thread (DelegationThread)

		// Sequencing
		// Ordered batch vector for efficient subscribe
		void GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers);
		void Sequencer4();
		void Sequencer5();  // Batch-level sequencer (Phase 1b: epoch pipeline + Level 5 hold buffer)
		void BrokerScannerWorker(int broker_id);
		void BrokerScannerWorker5(int broker_id);  // Batch-level scanner (Phase 1b: pushes to epoch buffer)
		void EpochDriverThread();   // [[PHASE_1B]] Advances epoch_index_ every kEpochUs
		void EpochSequencerThread(); // [[PHASE_1B]] Processes closed epochs: one fetch_add per epoch, Level 5, commit
		void ProcessLevel5Batches(std::vector<PendingBatch5>& level5, std::vector<PendingBatch5>& ready);
	bool ProcessSkipped(
			absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>>& skipped_batches,
			BatchHeader* &header_for_sub);
	void AssignOrder(BatchHeader *header, size_t start_total_order, BatchHeader* &header_for_sub);
	void AssignOrder5(BatchHeader *header, size_t start_total_order, BatchHeader* &header_for_sub);  // Batch-level version

	std::atomic<size_t> global_seq_{0};  // Message-level sequence (total messages sequenced)
	std::atomic<uint64_t> global_batch_seq_{0};  // [[PHASE_2_FIX]] Batch-level sequence for GOI indexing (0, 1, 2, 3...)
		absl::flat_hash_map<size_t, size_t> next_expected_batch_seq_;// client_id -> next expected batch_seq
		absl::Mutex global_seq_batch_seq_mu_;;

		// [[PHASE_1A_EPOCH_FENCING]] Broker's view of ControlBlock.epoch; updated in RefreshBrokerEpochFromCXL
		// [[THREADING]] Written by PublishReceiveThreads in GetCXLBuffer paths; must be atomic (no mutex on hot path)
		std::atomic<uint64_t> broker_epoch_{0};
		// [[PHASE_1A_EPOCH_FENCING]] Counter for periodic epoch check (every kEpochCheckInterval batches; design §4.2.1 "e.g. every 100 batches")
		std::atomic<uint64_t> epoch_check_counter_{0};
		static constexpr uint64_t kEpochCheckInterval = 100;

		// [[PHASE_1B]] Epoch-batched sequencing (§3.2): one atomic per epoch
		static constexpr unsigned kEpochUs = 500;
		std::atomic<uint64_t> epoch_index_{0};
		std::atomic<uint64_t> last_sequenced_epoch_{0};
		std::vector<PendingBatch5> epoch_buffers_[3];
		absl::Mutex epoch_buffer_mutex_;
		std::thread epoch_driver_thread_;
		// [[PHASE_1B]] Level 5 hold buffer + per-client state (§3.2)
		absl::flat_hash_map<size_t, ClientState5> level5_client_state_;
		absl::Mutex level5_state_mu_;
		std::map<size_t, std::map<size_t, HoldEntry5>> hold_buffer_;  // client_id -> (client_seq -> entry)
		size_t hold_buffer_size_{0};
		static constexpr size_t kHoldBufferMaxEntries = 10000;
		static constexpr int kMaxWaitEpochs = 3;
		uint64_t current_epoch_for_hold_{0};  // Epoch counter for hold_buffer_.tick_epoch()
		// [[PHASE_1B]] Export chain: per-broker subscription pointer (set by EpochSequencerThread on commit)
		absl::flat_hash_map<int, BatchHeader*> phase1b_header_for_sub_;
		absl::Mutex phase1b_header_for_sub_mu_;

		// [[PHASE_2_FIX]] Per-broker absolute PBR counters (never wrap, for CV tracking)
		std::array<std::atomic<uint64_t>, NUM_MAX_BROKERS> broker_pbr_counters_{};

	// [[PHASE_3]] Sequencer-driven recovery (§4.2.2): Detect stalled chain replication
	/**
	 * GOITimestampEntry tracks when a GOI entry was written (for timeout detection).
	 * Lock-free ring buffer - no mutex on sequencer hot path.
	 * @paper_ref Design §4.2.2 Sequencer-Driven Replica Recovery
	 */
	struct alignas(64) GOITimestampEntry {
		std::atomic<uint64_t> goi_index{0};      // GOI index written
		std::atomic<uint64_t> timestamp_ns{0};   // When written (CLOCK_MONOTONIC)
	};

	// Lock-free circular buffer for tracking recent GOI writes
	// Recovery thread scans this periodically; sequencer writes without mutex
	static constexpr size_t kGOITimestampRingSize = 65536;  // Track last 64K GOI writes (32ms at 2M/sec)
	std::array<GOITimestampEntry, kGOITimestampRingSize> goi_timestamps_;
	std::atomic<uint64_t> goi_timestamp_write_pos_{0};

	std::thread goi_recovery_thread_;
	void GOIRecoveryThread();  // [[PHASE_3]] Monitor num_replicated and recover stalled chains

	// [[PHASE_3]] Recovery parameters (§4.2.2)
	static constexpr uint64_t kChainReplicationTimeoutNs = 10'000'000;  // 10ms (10× expected ~1ms chain latency)
	static constexpr uint64_t kRecoveryScanIntervalNs = 1'000'000;      // 1ms scan interval
};
} // End of namespace Embarcadero
