#pragma once

#include <thread>
#include <atomic>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <array>
#include <map>
#include <memory>
#include <vector>
#include <deque>
#include <queue>
#include <chrono>

#include "../disk_manager/corfu_replication_client.h"
#include "../disk_manager/scalog_replication_client.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "common/config.h"
#include "sequencer_utils.h"

#include "absl/container/flat_hash_set.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"
#include "absl/container/btree_set.h"
#include <glog/logging.h>
#include "folly/MPMCQueue.h"

#include <functional>

namespace Embarcadero {

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

#ifdef NDEBUG
static constexpr bool kEnableDiagnostics = false;
#else
static constexpr bool kEnableDiagnostics = true;
#endif

// [[PHASE_1B]] Epoch-batched sequencing + Level 5 hold buffer (design §3.2)
/** Copy of batch metadata at hold time; ring slot may be reused so we never read p.hdr when from_hold. */
struct HoldBatchMetadata {
	size_t log_idx{0};
	size_t total_size{0};
	uint64_t batch_id{0};
	size_t batch_seq{0};
	uint64_t pbr_absolute_index{0};
	size_t client_id{0};
	uint16_t epoch_created{0};
	int broker_id{0};
	uint32_t num_msg{0};
	size_t start_logical_offset{0};
};
struct PendingBatch5 {
	struct BatchHeader* hdr{nullptr};
	int broker_id{0};
	uint32_t num_msg{0};
	size_t client_id{0};
	size_t batch_seq{0};
	size_t slot_offset{0};  // For commit order (consumed_through per broker)
	uint16_t epoch_created{0};  // [[PHASE_1A]] For sequencer-side epoch validation (§4.2)
	// [[CONSUMED_THROUGH_SKIP]] When true, scanner skipped this slot (in-flight); sequencer only advances consumed_through
	bool skipped{false};
	// [[HOLD_MARKER]] Placeholder when batch moved to hold; sequencer advances consumed_through only
	bool is_held_marker{false};
	// [[FROM_HOLD]] Batch was drained from hold; use hold_meta (not hdr) for GOI/export; ring slot may be reused
	bool from_hold{false};
	// [[FROM_HOLD]] When from_hold is true, use this copy (filled at hold time) instead of hdr
	HoldBatchMetadata hold_meta;
	// [PHASE-5] Metadata cached at scanner time (L1-hot after invalidation); avoids cold CXL reads in hold/GOI path
	size_t cached_log_idx{0};
	size_t cached_total_size{0};
	uint64_t cached_batch_id{0};
	uint64_t cached_pbr_absolute_index{0};
	size_t cached_start_logical_offset{0};
};
/** Export metadata for batches ordered from hold (ring slot already advanced). */
struct OrderedHoldExportEntry {
	size_t log_idx{0};
	size_t batch_size{0};
	size_t total_order{0};
	uint32_t num_messages{0};
};


using ClientState5 = OptimizedClientState;
struct HoldEntry5 {
	PendingBatch5 batch;
	HoldBatchMetadata meta;  // Copy at hold time so we never read ring after consumed_through advances
	uint64_t hold_start_ns{0};
};
struct ExpiredHoldEntry {
	size_t client_id{0};
	size_t seq{0};
	PendingBatch5 batch;
	HoldBatchMetadata meta;
};

// [[PHASE_1B]] Epoch buffer state machine to prevent concurrent merge/write.
struct alignas(64) EpochBuffer5 {
	enum class State : uint32_t { IDLE, COLLECTING, SEALED };

	std::atomic<State> state{State::IDLE};
	std::atomic<bool> broker_active[NUM_MAX_BROKERS];
	std::mutex seal_mutex;
	std::condition_variable seal_cv;
	std::array<std::deque<PendingBatch5>, NUM_MAX_BROKERS> per_broker;

	EpochBuffer5() {
		for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
			broker_active[i].store(false, std::memory_order_relaxed);
		}
	}

	bool enter_collection(int broker_id) {
		if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS) return false;

		State cur = state.load(std::memory_order_acquire);
		if (cur != State::COLLECTING) return false;

		broker_active[broker_id].store(true, std::memory_order_release);

		// Double-check state after marking active (prevent race where state changed)
		if (state.load(std::memory_order_acquire) != State::COLLECTING) {
			broker_active[broker_id].store(false, std::memory_order_release);
			std::lock_guard<std::mutex> lk(seal_mutex);
			seal_cv.notify_all();
			return false;
		}
		return true;
	}

	void exit_collection(int broker_id) {
		if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS) return;
		broker_active[broker_id].store(false, std::memory_order_release);

		// Only notify sealer if we're in SEALED state (optimization to avoid unnecessary lock contention)
		if (state.load(std::memory_order_acquire) == State::SEALED) {
			std::lock_guard<std::mutex> lk(seal_mutex);
			seal_cv.notify_all();
		}
	}

	bool seal() {
		State expected = State::COLLECTING;
		if (!state.compare_exchange_strong(expected, State::SEALED, std::memory_order_acq_rel)) {
			return false;
		}
		std::unique_lock<std::mutex> lk(seal_mutex);
		constexpr auto kSealWaitBudget = std::chrono::milliseconds(2);
		bool quiesced = seal_cv.wait_for(lk, kSealWaitBudget, [this] {
			for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
				if (broker_active[i].load(std::memory_order_acquire)) return false;
			}
			return true;
		});
		if (!quiesced) {
			// Liveness-first rollback: avoid permanent SEALED dead zone when a collector
			// flag gets stuck. Driver/scanners will retry sealing.
			state.store(State::COLLECTING, std::memory_order_release);
			seal_cv.notify_all();
			return false;
		}
		return true;
	}

	void reset_and_start() {
		for (auto& v : per_broker) v.clear();
		for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
			broker_active[i].store(false, std::memory_order_relaxed);
		}
		state.store(State::COLLECTING, std::memory_order_release);
	}

	bool is_available() const {
		State s = state.load(std::memory_order_acquire);
		return s == State::IDLE;
	}
};

/**
 * Lock-free PBR allocation state (128-bit for CMPXCHG16B on x86-64).
 * Atomically allocates (slot_seq, logical_offset) to avoid ordering violations.
 * @paper_ref docs/LOCKFREE_PBR_DESIGN.md §4.1
 */
struct alignas(16) PBRProducerState {
	uint64_t next_slot_seq;   // Monotonic slot sequence (not byte offset)
	uint64_t logical_offset; // Cumulative message count
};
static_assert(sizeof(PBRProducerState) == 16, "Must be 16 bytes for CMPXCHG16B");

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
		 * Start the topic's threads after construction is complete.
		 * This method creates and starts all necessary threads for the topic.
		 */
		void Start();

		/**
		 * Destructor - ensures all threads are stopped and joined
		 */
	~Topic() {
		stop_threads_.store(true, std::memory_order_release);  // Atomic assignment for shutdown
		stop_threads_volatile_ = true;  // Volatile copy for legacy interfaces
		committed_seq_updater_stop_.store(true, std::memory_order_release);
		committed_seq_updater_cv_.notify_all();
		
		for (auto& shard : level5_shards_) {
			if (shard) {
				std::lock_guard<std::mutex> lock(shard->mu);
				shard->stop = true;
				shard->has_work = true;
			}
			if (shard) shard->cv.notify_one();
		}
		for (std::thread& thread : delegationThreads_) {
			if (thread.joinable()) {
				thread.join();
			}
		}

		// [[PHASE_5]] Join Corfu callback threads
		for (std::thread& thread : corfu_callback_threads_) {
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
			if (committed_seq_updater_thread_.joinable()) {
				committed_seq_updater_thread_.join();
			}
			for (std::thread& thread : level5_shard_threads_) {
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
				BatchHeader*& batch_header_location,
				bool epoch_already_checked = false) {
			return (this->*GetCXLBufferFunc)(batch_header, topic, log, segment_header, logical_offset, batch_header_location, epoch_already_checked);
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

		/** [[ORDER_0_SKIP_PBR]] For order 0 we do not write to PBR. Returns start logical offset for this batch and advances by num_msg. */
		size_t GetAndAdvanceOrder0LogicalOffset(uint32_t num_msg);

		/** [[ORDER_0_SUBSCRIBE]] Update written_logical_offset_ and written_physical_addr_ so GetMessageAddr can serve subscribers. Call after next_msg_diff is set for the batch. */
		void SetOrder0Written(size_t cumulative_logical_offset, size_t blog_offset, uint32_t num_msg);

		/** [[ORDER_0_TAIL]] When publish connection closes, advance written_* to order0_next_logical_offset_ so subscribers see full tail (fixes out-of-order batch completion leaving written behind). */
		void FinalizeOrder0WrittenIfNeeded();

		/** [[Issue #3]] Single epoch check per batch: do once at batch start, pass epoch_already_checked to ReserveBLogSpace/ReservePBRSlotAndWriteEntry. Returns true if stale (caller must not allocate). */
		bool CheckEpochOnce();
		void* ReserveBLogSpace(size_t size, bool epoch_already_checked = false);
		void RefreshPBRConsumedThroughCache();
		bool IsPBRAboveHighWatermark(int high_pct);
		bool IsPBRBelowLowWatermark(int low_pct);
		/**
		 * @brief Reserves one PBR slot and writes BatchHeader to CXL; caller then writes payload and flushes.
		 * @threading Thread-safe: lock-free (128-bit CAS) when pbr_state_.is_lock_free(), else mutex-protected.
		 * @ownership batch_header_location points into CXL; caller must flush before signalling completion.
		 * @paper_ref docs/LOCKFREE_PBR_DESIGN.md §4; RECEIVE_PATH §3.1 (atomic PBR reserve).
		 */
		bool ReservePBRSlotAndWriteEntry(BatchHeader& batch_header, void* log,
				void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);
		/**
		 * @brief Reserves one PBR slot after payload recv; caller writes BatchHeader once after.
		 * @threading Thread-safe: lock-free (128-bit CAS) when pbr_state_.is_lock_free(), else mutex-protected.
		 * @ownership batch_header_location points into CXL; caller must write header + flush.
		 * @paper_ref docs/EMBARCADERO_DEFINITIVE_DESIGN.md §3.1 (receive then PBR commit).
		 */
		bool ReservePBRSlotAfterRecv(BatchHeader& batch_header, void* log,
				void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);
		/**
		 * @brief Publish a fully received batch into its reserved PBR slot.
		 * Writes BatchHeader, marks batch_complete, and flushes both cachelines for CXL visibility.
		 * @return true on success, false if inputs are invalid.
		 */
		bool PublishPBRSlotAfterRecv(const BatchHeader& batch_header, BatchHeader* batch_header_location);

	private:
		/** Lock-free PBR slot reservation (128-bit CAS). Returns false if ring full. @threading Concurrent. */
		bool ReservePBRSlotLockFree(uint32_t num_msg, size_t& out_byte_offset, size_t& out_logical_offset);
		/** [[P2.3]] Shared core: epoch check, slot allocation, CheckSegmentBoundary, segment_header, batch_header metadata. Caller writes slot (minimal or full). */
		bool ReservePBRSlotCore(BatchHeader& batch_header, void* log, bool epoch_already_checked,
				void*& batch_headers_log, size_t& logical_offset, void*& segment_header);
		/**
		 * Update the TInode's written offset and address
		 */
		inline void UpdateTInodeWritten(size_t written, size_t written_addr);
		/**
		 * [[PHASE_2_CV_EXPORT]] Advance CompletionVector for this broker so ack_level=1 export can proceed without waiting for replication.
		 * Sequencer calls after writing GOI entry; uses atomic max so tail replica (ack_level=2) can also advance CV.
		 * @param broker_id Broker ID
		 * @param pbr_index Absolute PBR index of the completed batch
		 * @param cumulative_msg_count Cumulative message count (ACK offset) for this batch
		 */
		void AdvanceCVForSequencer(uint16_t broker_id, uint64_t pbr_index, uint64_t cumulative_msg_count);
		// [PHASE-3] Per-broker CV accumulation for single-fence commit
		void AccumulateCVUpdate(
				uint16_t broker_id,
				uint64_t pbr_index,
				uint64_t cumulative_msg_count,
				std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
				std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index);
		void FlushAccumulatedCV(
				const std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
				const std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index);
		void EnqueueCompletedRange(uint64_t start, uint64_t end);
		void CommittedSeqUpdaterThread();
		void ResetCompletedRangeQueue();

	/**
	 * DelegationThread: Stage 2 (Local Ordering)
	 * Purpose: Assign local per-broker sequence numbers to messages
	 * This is for Corfu, Scalog, and Embarcadero weak ordering
	 * 
	 * Processing pipeline:
	 * 1. Poll batch_complete on BatchHeader (gating; BlogMessageHeader::received is not used)
	 * 2. Assign local counter (per-broker sequence)
	 * 3. Update Bmeta.local.processed_ptr
	 * 4. Flush cache line (bytes 16-31 only)
	 */
	void DelegationThread();

		/**
		 * [[RECV_DIRECT_TO_CXL]] Returns PBR ring utilization 0..100, or -1 if sequencer-only.
		 * Uses cached consumed_through; refreshes every kPBRCacheRefreshInterval.
		 */
		int GetPBRUtilizationPct();

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
				BatchHeader*& batch_header_location,
				bool epoch_already_checked);

		// Pointer to the appropriate GetCXLBuffer implementation
		GetCXLBufferFuncPtr GetCXLBufferFunc;

		// Different implementations of GetCXLBuffer for different sequencer types
		std::function<void(void*, size_t)> KafkaGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);

		std::function<void(void*, size_t)> CorfuGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);

		std::function<void(void*, size_t)> ScalogGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);

		std::function<void(void*, size_t)> Order3GetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);

	// CORFU Order 3 implementation (external sequencer, no DelegationThread)
	std::function<void(void*, size_t)> CorfuOrder3GetCXLBuffer(
			BatchHeader& batch_header,
			const char topic[TOPIC_NAME_SIZE],
			void*& log,
			void*& segment_header,
			size_t& logical_offset,
			BatchHeader*& batch_header_location,
			bool epoch_already_checked = false);

		std::function<void(void*, size_t)> EmbarcaderoGetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);

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
		size_t written_logical_offset_ = static_cast<size_t>(-1);  // Sentinel: "not set"
		void* written_physical_addr_ = nullptr;
		void* order0_first_physical_addr_ = nullptr;  // [[ORDER_0]] Start of message chain (set on first SetOrder0Written); first_message_addr_ may be wrong for Order 0
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

		// [[RECV_DIRECT_TO_CXL]] Cached PBR consumed_through and next_slot for watermark checks (avoids mutex in GetPBRUtilizationPct)
		alignas(64) std::atomic<size_t> cached_pbr_consumed_through_{0};
		alignas(64) std::atomic<size_t> cached_next_slot_offset_{0};
		alignas(64) std::atomic<uint64_t> pbr_cache_refresh_counter_{0};
		// Refresh consumed_through every 100 batches (~10–100μs at high throughput). Balances staleness (100 slots ≈ 20% of 512-slot ring) vs CXL read overhead. [[CODE_REVIEW Issue #7]]
		static constexpr uint64_t kPBRCacheRefreshInterval = 100;

		// [[LOCKFREE_PBR]] 128-bit CAS state; fallback to mutex when not lock-free (docs/LOCKFREE_PBR_DESIGN.md)
		alignas(64) std::atomic<PBRProducerState> pbr_state_{{0, 0}};
		alignas(64) std::atomic<uint64_t> cached_consumed_seq_{0};  // Slot sequence from consumed_through / sizeof(BatchHeader)
		const size_t num_slots_;                         // BATCHHEADERS_SIZE / sizeof(BatchHeader), set in ctor
		bool use_lock_free_pbr_{false};                  // True when pbr_state_.is_lock_free()

		// [[ORDER_0_SKIP_PBR]] Monotonic logical offset for order 0; no PBR slot written, only written count updated
		std::atomic<size_t> order0_next_logical_offset_{0};

		// TInode cache
		int replication_factor_;
		void* ordered_offset_addr_;
		void* current_segment_;
		size_t ordered_offset_;

		// Thread control
		alignas(64) std::atomic<bool> stop_threads_{false};  // Atomic for thread-safe shutdown
		volatile bool stop_threads_volatile_{false};  // Volatile copy for legacy interfaces
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
	void Sequencer5();  // Batch-level sequencer (Phase 1b: epoch pipeline + Level 5 hold buffer)
	void Sequencer2();  // Order 2: Total order, no per-client state;
	void BrokerScannerWorker5(int broker_id);  // Batch-level scanner (Phase 1b: pushes to epoch buffer)
	void EpochDriverThread();   // [[PHASE_1B]] Advances epoch_index_ every kEpochUs
	void EpochSequencerThread(); // [[PHASE_1B]] Processes closed epochs: one fetch_add per epoch, Level 5, commit
	void CommitEpoch(std::vector<PendingBatch5>& ready,
		std::vector<const PendingBatch5*>& by_slot,
		std::array<size_t, NUM_MAX_BROKERS>& contiguous_consumed_per_broker,
		std::array<bool, NUM_MAX_BROKERS>& broker_seen_in_epoch,
		std::array<uint64_t, NUM_MAX_BROKERS>& cv_max_cumulative,
		std::array<uint64_t, NUM_MAX_BROKERS>& cv_max_pbr_index,
		std::vector<PendingBatch5>& batch_list,
		bool is_drain_mode);
	struct Level5ShardState;  // forward declaration; defined below
	void ProcessLevel5Batches(std::vector<PendingBatch5>& level5, std::vector<PendingBatch5>& ready);
	void ProcessLevel5BatchesShard(Level5ShardState& shard,
		std::vector<PendingBatch5>& level5,
		std::vector<PendingBatch5>& ready);
	void ClientGc(Level5ShardState& shard);
	bool CheckAndInsertBatchId(Level5ShardState& shard, uint64_t batch_id);
	void Level5ShardWorker(size_t shard_id);
	void InitLevel5Shards();
	size_t GetTotalHoldBufferSize();
	void AssignOrder5(BatchHeader *header, size_t start_total_order, BatchHeader* &header_for_sub);  // Batch-level version

	// [[NAMING]] total_order space: next message sequence to assign (design: message-level order for subscribers).
	alignas(64) std::atomic<size_t> global_seq_{0};
	// [[NAMING]] GOI index: next slot in GOI array (0, 1, 2, ...). Used as GOIEntry index; committed_seq tracks max written.
	alignas(64) std::atomic<uint64_t> global_batch_seq_{0};
		absl::flat_hash_map<size_t, size_t> next_expected_batch_seq_;// client_id -> next expected batch_seq
		absl::Mutex global_seq_batch_seq_mu_;;

		// [[PHASE_1A_EPOCH_FENCING]] Broker's view of ControlBlock.epoch; updated in RefreshBrokerEpochFromCXL
		// [[THREADING]] Written by PublishReceiveThreads in GetCXLBuffer paths; must be atomic (no mutex on hot path)
		alignas(64) std::atomic<uint64_t> broker_epoch_{0};
		// [[Issue #3]] Epoch from last CheckEpochOnce() for use when epoch_already_checked=true in ReservePBRSlotAndWriteEntry
		alignas(64) std::atomic<uint64_t> last_checked_epoch_{0};
		// [PHASE-2B] Shared atomic for sequencer to avoid cold CXL read of ControlBlock.epoch
		alignas(64) std::atomic<uint64_t> cached_epoch_{0};
		// [[PHASE_1A_EPOCH_FENCING]] Counter for periodic epoch check (every kEpochCheckInterval batches; design §4.2.1 "e.g. every 100 batches")
		alignas(64) std::atomic<uint64_t> epoch_check_counter_{0};
		static constexpr uint64_t kEpochCheckInterval = 100;

		// [[PHASE_1B]] Epoch-batched sequencing (§3.2): one atomic per epoch
		static constexpr unsigned kEpochUs = 500;
		alignas(64) std::atomic<uint64_t> epoch_index_{0};
		alignas(64) std::atomic<uint64_t> last_sequenced_epoch_{0};
		std::atomic<bool> epoch_driver_done_{false}; // [[SHUTDOWN_SYNC]] Cross-thread flag: EpochDriverThread finished sealing
		// ORDER=5 phase diagnostics (scanner push vs sequencer commit), enabled via EMBARCADERO_ORDER5_PHASE_DIAG.
		std::array<std::atomic<uint64_t>, NUM_MAX_BROKERS> scanner_pushed_batches_{};
		std::array<std::atomic<uint64_t>, NUM_MAX_BROKERS> scanner_pushed_msgs_{};
		std::array<std::atomic<uint64_t>, NUM_MAX_BROKERS> sequencer_committed_batches_{};
		std::array<std::atomic<uint64_t>, NUM_MAX_BROKERS> sequencer_committed_msgs_{};
		// [[B0_TAIL_FIX]] When set, next ProcessLevel5BatchesShard treats all hold entries as expired (shutdown drain).
		std::atomic<bool> force_expire_hold_on_next_process_{false};
	// Epoch buffers for safe collection/sequencing (no concurrent merge/write).
	EpochBuffer5 epoch_buffers_[3];
		std::thread epoch_driver_thread_;
		// [[PHASE_1B]] Level 5 hold buffer + per-client state (§3.2)
		struct Level5ShardState {
			std::mutex mu;
			std::condition_variable cv;
			bool has_work{false};
			bool done{false};
			bool stop{false};
			std::vector<PendingBatch5> input;
			std::vector<PendingBatch5> ready;
			absl::flat_hash_map<size_t, ClientState5> client_state;
			absl::flat_hash_map<size_t, uint64_t> client_highest_committed;
			absl::flat_hash_map<size_t, std::map<size_t, HoldEntry5>> hold_buffer;  // client_id -> ordered(client_seq -> entry)
			size_t hold_buffer_size{0};
			absl::flat_hash_set<size_t> clients_with_held_batches;
			std::vector<ExpiredHoldEntry> expired_hold_buffer;
			std::vector<std::pair<size_t, size_t>> expired_hold_keys_buffer;
			std::vector<PendingBatch5> deferred_level5;
			std::vector<std::vector<PendingBatch5>> per_shard_cache;
			FastDeduplicator dedup;
			RadixSorter<PendingBatch5> radix_sorter;
			// [PHASE-3] Per-shard CV accumulation for late/expired batches
			absl::flat_hash_map<int, uint64_t> cv_max_cumulative;
			absl::flat_hash_map<int, uint64_t> cv_max_pbr_index;
		};
		size_t level5_num_shards_{1};
		std::vector<std::unique_ptr<Level5ShardState>> level5_shards_;
		std::vector<std::thread> level5_shard_threads_;
		std::atomic<bool> level5_shards_started_{false};
		static constexpr size_t kHoldBufferMaxEntries = 100000;
		static constexpr uint64_t kOrder5BaseTimeoutNs = 5ULL * 1000 * 1000;  // 5ms wall-clock timeout
		static constexpr uint64_t kClientTtlEpochs = 20000;
		static constexpr uint64_t kClientGcEpochInterval = 1024;
		static constexpr size_t kDeferredL5MaxEntries = kHoldBufferMaxEntries * 2;
		alignas(64) std::atomic<uint64_t> current_epoch_for_hold_{0};  // Epoch for hold buffer expiry; atomic for shard workers
		// Export chain: per-broker cursor into batch header ring (set by EpochSequencerThread on commit).
		// Fixed array for O(1) indexed access; nullptr = no ring for that broker (e.g. sequencer-only B0).
		std::array<BatchHeader*, NUM_MAX_BROKERS> export_cursor_by_broker_{};
		absl::Mutex export_cursor_mu_;

		/** Initializes the export chain cursor for a specific broker (ring_start). Idempotent. */
		void InitExportCursorForBroker(int broker_id);
		std::vector<std::vector<PendingBatch5>> level5_per_shard_cache_;
		// [PHASE-8-REVISED] Per-broker unbounded queues for from-hold batch export.
		// Replaces SPSC ring to avoid deadlock when no subscriber consumes (throughput test).
		// Uses per-broker mutex to minimize contention (vs global mutex).
		struct PerBrokerHoldQueue {
			absl::Mutex mu;
			std::deque<OrderedHoldExportEntry> q;
		};
		std::array<PerBrokerHoldQueue, NUM_MAX_BROKERS> hold_export_queues_{};

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

	// Helper method to advance consumed_through for processed slots
	// [[WRAP_FIX]] Handle ring wrap: when next_expected==BATCHHEADERS_SIZE, slot 0 is the next expected.
	void AdvanceConsumedThroughForProcessedSlots(
		const std::vector<PendingBatch5>& batch_list,
		std::array<size_t, NUM_MAX_BROKERS>& contiguous_consumed_per_broker,
		const std::array<bool, NUM_MAX_BROKERS>& broker_seen_in_epoch,
		const std::array<uint64_t, NUM_MAX_BROKERS>* cv_max_cumulative,
		const std::array<uint64_t, NUM_MAX_BROKERS>* cv_max_pbr_index);

	// [[PHASE_3]] Recovery parameters (§4.2.2)
	static constexpr uint64_t kChainReplicationTimeoutNs = 10'000'000;  // 10ms (10× expected ~1ms chain latency)
	static constexpr uint64_t kRecoveryScanIntervalNs = 1'000'000;      // 1ms scan interval

	struct CompletedRange {
		uint64_t start{0};  // inclusive
		uint64_t end{0};    // exclusive
		bool operator>(const CompletedRange& other) const { return start > other.start; }
	};
	struct CompletedRangeSlot {
		CompletedRange data{};
		std::atomic<bool> ready{false};
	};
	static constexpr size_t kCompletedRangesRingCap = 65536;  // Power of 2
	static constexpr size_t kCompletedRangesRingMask = kCompletedRangesRingCap - 1;
	std::thread committed_seq_updater_thread_;
	std::mutex committed_seq_updater_mu_;  // Event wait mutex (ring data itself is lock-free)
	std::condition_variable committed_seq_updater_cv_;
	std::array<CompletedRangeSlot, kCompletedRangesRingCap> completed_ranges_ring_{};
	alignas(64) std::atomic<uint64_t> completed_ranges_head_{0};
	alignas(64) std::atomic<uint64_t> completed_ranges_tail_{0};
	std::atomic<bool> committed_seq_updater_stop_{false};
	std::atomic<uint64_t> completed_ranges_enqueue_retries_{0};
	std::atomic<uint64_t> completed_ranges_enqueue_wait_ns_{0};
	std::atomic<uint64_t> completed_ranges_max_depth_{0};
	std::atomic<uint64_t> committed_updater_pending_peak_{0};

	// Per-topic sequencers: avoid static locals that cause shared state between topics
	std::atomic<size_t> scalog_batch_offset_{0};  // For ScalogGetCXLBuffer (was static local)
		size_t cached_num_brokers_{0};  // For Order3GetCXLBuffer (was static local)

		// [[PHASE_5]] Corfu Order 3 Callback Thread Pool
		// Offloads callback work (spin-wait, replication, CV advance) from NetworkManager threads
		// to avoid starvation under high concurrency.
		struct CorfuCallbackTask {
			std::function<void(void*, size_t)> callback;
			void* log_ptr;
			size_t size;
		};
		folly::MPMCQueue<CorfuCallbackTask> corfu_callback_queue_{1024};
		std::vector<std::thread> corfu_callback_threads_;
		void CorfuCallbackWorker();

		// [[PHASE_7]] Corfu Order 3 Completion Tracking
		// Tracks contiguous completion of PBR slots to advance CV monotonically.
		// Replaces the race-prone direct CV advance in Phase 5.
		std::array<std::atomic<uint64_t>, NUM_MAX_BROKERS> next_cv_advance_pbr_{};
		struct PBRCompletionSlot {
			std::atomic<bool> completed{false};
			uint64_t cumulative_msg_count{0};
		};
		// Use a large enough ring to handle in-flight batches (e.g., 64K slots)
		static constexpr size_t kPBRCompletionRingSize = 65536;
		std::array<std::unique_ptr<std::array<PBRCompletionSlot, kPBRCompletionRingSize>>, NUM_MAX_BROKERS> pbr_completion_rings_;
		void AdvanceCVContiguous(int broker_id);
};
} // End of namespace Embarcadero
