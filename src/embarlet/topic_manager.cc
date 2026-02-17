#include "topic_manager.h"
#include <glog/logging.h>
#include <cstring>
#include <algorithm>
#include "common/performance_utils.h"
#include <immintrin.h>
#include <xmmintrin.h>  // For _mm_pause()

// Project includes
#include "../cxl_manager/cxl_manager.h"
#include "../disk_manager/disk_manager.h"

namespace Embarcadero {

constexpr size_t NT_THRESHOLD = 4096; // [[P5]] Increase threshold to 4KB to avoid cache pollution for small batches

/** 32 bytes = one AVX2 vector; two per 64-byte cache line. */
static constexpr size_t AVX2_VECTOR_SIZE = 32;

/**
 * AVX2 inner loop: 2 x 32-byte streaming stores per 64-byte cache line.
 * Only called when __builtin_cpu_supports("avx2") is true.
 * Compiled with target("avx2") so the rest of nt_memcpy can be built without -mavx2 for fallback.
 */
#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2")))
static void nt_memcpy_avx2_loop(uint8_t* __restrict dst, const uint8_t* __restrict src, size_t num_lines) {
	constexpr size_t CACHE_LINE_SIZE = 64;
	// 2 x 32-byte vectors per cache line
	for (size_t i = 0; i < num_lines; i++) {
		__m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
		__m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + AVX2_VECTOR_SIZE));
		_mm256_stream_si256(reinterpret_cast<__m256i*>(dst), a);
		_mm256_stream_si256(reinterpret_cast<__m256i*>(dst + AVX2_VECTOR_SIZE), b);
		src += CACHE_LINE_SIZE;
		dst += CACHE_LINE_SIZE;
	}
}
#endif

/**
 * Non-temporal memory copy optimized for large data transfers.
 * Uses streaming stores to bypass cache. Prefers AVX2 (32-byte) when available,
 * otherwise SSE2 (16-byte). Works on all x86-64 CPUs; AVX2 used on Intel Haswell+
 * and AMD Excavator+ (2013+).
 */
void nt_memcpy(void* __restrict dst, const void* __restrict src, size_t size) {
	static constexpr size_t CACHE_LINE_SIZE = 64;

	if (size < NT_THRESHOLD) {
		memcpy(dst, src, size);
		return;
	}

	const uintptr_t dst_addr = reinterpret_cast<uintptr_t>(dst);
	const size_t unaligned_bytes = (CACHE_LINE_SIZE - dst_addr % CACHE_LINE_SIZE) % CACHE_LINE_SIZE;
	const size_t initial_bytes = std::min(unaligned_bytes, size);

	if (initial_bytes > 0) {
		memcpy(dst, src, initial_bytes);
	}

	uint8_t* aligned_dst = static_cast<uint8_t*>(dst) + initial_bytes;
	const uint8_t* aligned_src = static_cast<const uint8_t*>(src) + initial_bytes;
	size_t remaining = size - initial_bytes;
	const size_t num_lines = remaining / CACHE_LINE_SIZE;
	remaining -= num_lines * CACHE_LINE_SIZE;

#if defined(__x86_64__) || defined(_M_X64)
	// One-time runtime detection: use AVX2 if available (most modern servers have it).
	static bool avx2_checked = false;
	static bool use_avx2 = false;
	if (!avx2_checked) {
		__builtin_cpu_init();
		use_avx2 = __builtin_cpu_supports("avx2");
		avx2_checked = true;
	}

	if (use_avx2 && num_lines > 0) {
		nt_memcpy_avx2_loop(aligned_dst, aligned_src, num_lines);
		aligned_dst += num_lines * CACHE_LINE_SIZE;
		aligned_src += num_lines * CACHE_LINE_SIZE;
	} else
#endif
	{
		// SSE2 fallback: 4 x 16-byte per cache line (works on all x86-64).
		const size_t vectors_per_line = CACHE_LINE_SIZE / sizeof(__m128i);
		for (size_t i = 0; i < num_lines; i++) {
			for (size_t j = 0; j < vectors_per_line; j++) {
				const __m128i data = _mm_loadu_si128(
						reinterpret_cast<const __m128i*>(aligned_src + j * sizeof(__m128i)));
				_mm_stream_si128(
						reinterpret_cast<__m128i*>(aligned_dst + j * sizeof(__m128i)), data);
			}
			aligned_src += CACHE_LINE_SIZE;
			aligned_dst += CACHE_LINE_SIZE;
		}
	}

	if (remaining > 0) {
		memcpy(aligned_dst, aligned_src, remaining);
	}
}

void TopicManager::Shutdown() {
	if (shutdown_done_.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	shutting_down_.store(true, std::memory_order_release);
	absl::WriterMutexLock lock(&topics_mutex_);
	topics_.clear();
}

/**
 * Helper function to initialize TInode offsets
 */
void TopicManager::InitializeTInodeOffsets(TInode* tinode, 
		void* segment_metadata,
		void* batch_headers_region, 
		void* cxl_addr) {
	if (!tinode) return;

	// Initialize offset values
	// Start from 0 instead of -1 to allow initial acknowledgments
	tinode->offsets[broker_id_].ordered = 0;
	tinode->offsets[broker_id_].written = 0;
	for ( int i = 0; i < NUM_MAX_BROKERS; i++ ) {
		tinode->offsets[broker_id_].replication_done[i] = 0;
	}

	// Calculate log offset using pointer difference plus alignment for O_DIRECT
	const uintptr_t segment_addr = reinterpret_cast<uintptr_t>(segment_metadata);
	const uintptr_t cxl_base_addr = reinterpret_cast<uintptr_t>(cxl_addr);
	// [[FIX: O_DIRECT Alignment]] Align log start to 4KB.
	// Segment header is at segment_addr; skip it and align.
	// Assuming SegmentHeader fits in 4KB (it's small).
	size_t start_offset = CACHELINE_SIZE;
	size_t aligned_start = (start_offset + 4095) & ~4095;
	tinode->offsets[broker_id_].log_offset = 
		static_cast<size_t>(segment_addr + aligned_start - cxl_base_addr);

	// Calculate batch headers offset using pointer difference
	const uintptr_t batch_headers_addr = reinterpret_cast<uintptr_t>(batch_headers_region);
	tinode->offsets[broker_id_].batch_headers_offset = 
		static_cast<size_t>(batch_headers_addr - cxl_base_addr);

	//  Initialize consumed_through so producer can allocate slot 2+ without blocking.
	// Semantics: "first byte past last consumed slot". BATCHHEADERS_SIZE means "all slots [0, size)
	// are available" so GetCXLBuffer's check consumed >= slot_offset + sizeof(BatchHeader) passes
	// until the ring fills. Sequencer overwrites this when it processes each batch.
	tinode->offsets[broker_id_].batch_headers_consumed_through = BATCHHEADERS_SIZE;

	// [[ROOT_CAUSE_B_FIX]] - Flush broker-specific offset initialization
	// After each broker initializes its offsets[broker_id_] entries (log_offset/batch_headers_offset/written_addr)
	// Flush the broker region (first 256B of offset_entry) so other threads see the values
	const void* broker_region = const_cast<const void*>(static_cast<const volatile void*>(&tinode->offsets[broker_id_].log_offset));
	CXL::flush_cacheline(broker_region);
	// Flush sequencer region so producer (on any broker) sees batch_headers_consumed_through
	const void* sequencer_region = const_cast<const void*>(static_cast<const volatile void*>(&tinode->offsets[broker_id_].batch_headers_consumed_through));
	CXL::flush_cacheline(sequencer_region);
	CXL::store_fence();
}

struct TInode* TopicManager::CreateNewTopicInternal(const char topic[TOPIC_NAME_SIZE]) {
	if (shutting_down_.load(std::memory_order_acquire)) return nullptr;
	struct TInode* tinode = cxl_manager_.GetTInode(topic);
	TInode* replica_tinode = nullptr;

	// Validate that TInode has been initialized by head node
	if (!tinode || tinode->topic[0] == 0) {
		LOG(ERROR) << "TInode not properly initialized for topic: " << topic;
		return nullptr;
	}

	{
		absl::WriterMutexLock lock(&topics_mutex_);

		CHECK_LT(num_topics_, MAX_TOPIC_SIZE)
			<< "Creating too many topics, increase MAX_TOPIC_SIZE";

		if (topics_.find(topic) != topics_.end()) {
			return nullptr;
		}

		void* cxl_addr = cxl_manager_.GetCXLAddr();
		void* segment_metadata = nullptr;
		void* batch_headers_region = nullptr;

		segment_metadata = cxl_manager_.GetNewSegment();
		batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();

		// Validate all pointers before using them
		if (!segment_metadata) {
			LOG(ERROR) << "Failed to allocate segment for topic: " << topic;
			return nullptr;
		}
		if (!batch_headers_region) {
			LOG(ERROR) << "Failed to allocate batch headers for topic: " << topic;
			return nullptr;
		}

		// [[CRITICAL FIX: STALE_RING_DATA]] Zero out batch header ring to prevent false in-flight slots
		// CXL memory may contain garbage data that looks like batches (num_msg>0, batch_complete=0).
		// Without zeroing, the scanner will think these are in-flight batches and stall.
		memset(batch_headers_region, 0, BATCHHEADERS_SIZE);
		// Flush the zeroed memory to CXL (non-coherent)
		for (size_t i = 0; i < BATCHHEADERS_SIZE; i += 64) {
			CXL::flush_cacheline(reinterpret_cast<uint8_t*>(batch_headers_region) + i);
		}
		CXL::store_fence();
		LOG(INFO) << "Zeroed batch header ring at " << batch_headers_region << " (" << BATCHHEADERS_SIZE << " bytes)";

		// Handle replica if needed
		if (tinode->replicate_tinode) {
			replica_tinode = cxl_manager_.GetReplicaTInode(topic);
			// Initialize this broker's offsets in the replica TInode
			InitializeTInodeOffsets(replica_tinode, segment_metadata,
					batch_headers_region, cxl_addr);
		}

		// Initialize this broker's offsets in the main TInode
		// Each broker needs its own entry in the offsets[NUM_MAX_BROKERS] array
		InitializeTInodeOffsets(tinode, segment_metadata, batch_headers_region, cxl_addr);

		LOG(INFO) << "[TopicManager] CreateNewTopicInternal: broker_id=" << broker_id_
		          << " batch_headers_offset=" << tinode->offsets[broker_id_].batch_headers_offset
		          << " log_offset=" << tinode->offsets[broker_id_].log_offset;

		// Create the topic
		// [[DEVIATION_004]] - Using TInode.offset_entry instead of separate Bmeta region
		topics_[topic] = std::make_unique<Topic>(
				[this]() { return cxl_manager_.GetNewSegment(); },
				[this]() { return get_num_brokers_callback_(); },
				GetRegisteredBrokersCallback([this](absl::btree_set<int> &registered_brokers,
														MessageHeader** msg_to_order, TInode *tinode) -> int {
				return get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode); }),
				static_cast<void*>(tinode),
				replica_tinode,
				topic,
				broker_id_,
				tinode->order,
				tinode->seq_type,
				cxl_addr,
				segment_metadata
				);
	}

	// Handle replication if needed
	{
		int replication_factor = tinode->replication_factor;
		if (tinode->seq_type == EMBARCADERO && replication_factor > 0) {
			disk_manager_.Replicate(tinode, replica_tinode, replication_factor);
		}

		// Run sequencer if needed
		if (tinode->seq_type == SCALOG) {
			if (replication_factor > 0) {
				disk_manager_.StartScalogReplicaLocalSequencer();
			}
		}
	}

	return tinode;
}

struct TInode* TopicManager::CreateNewTopicInternal(
		const char topic[TOPIC_NAME_SIZE],
		int order,
		int replication_factor,
		bool replicate_tinode,
		int ack_level,
		SequencerType seq_type) {
	if (shutting_down_.load(std::memory_order_acquire)) return nullptr;

	LOG(INFO) << "CreateNewTopicInternal: topic=" << topic << " order=" << order 
	          << " replication_factor=" << replication_factor << " ack_level=" << ack_level;

	struct TInode* tinode = cxl_manager_.GetTInode(topic);
	struct TInode* replica_tinode = nullptr;

	// Check for name collision in tinode: if already set to a different name, abort
	if (tinode->topic[0] != 0 && strncmp(tinode->topic, topic, TOPIC_NAME_SIZE) != 0) {
		LOG(ERROR) << "Topic name collides: " << tinode->topic;
		return nullptr;
	}

	{
		absl::WriterMutexLock lock(&topics_mutex_);

		CHECK_LT(num_topics_, MAX_TOPIC_SIZE)
			<< "Creating too many topics, increase MAX_TOPIC_SIZE";

		if (topics_.find(topic) != topics_.end()) {
			return nullptr;
		}

		void* cxl_addr = cxl_manager_.GetCXLAddr();
		void* segment_metadata = nullptr;
		void* batch_headers_region = nullptr;

		segment_metadata = cxl_manager_.GetNewSegment();
		batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();

		// Validate all pointers before using them
		if (!segment_metadata) {
			LOG(ERROR) << "Failed to allocate segment for topic: " << topic;
			return nullptr;
		}
		if (!batch_headers_region) {
			LOG(ERROR) << "Failed to allocate batch headers for topic: " << topic;
			return nullptr;
		}

		// [[CRITICAL FIX: STALE_RING_DATA]] Zero out batch header ring to prevent false in-flight slots
		memset(batch_headers_region, 0, BATCHHEADERS_SIZE);
		for (size_t i = 0; i < BATCHHEADERS_SIZE; i += 64) {
			CXL::flush_cacheline(reinterpret_cast<uint8_t*>(batch_headers_region) + i);
		}
		CXL::store_fence();
		LOG(INFO) << "Zeroed batch header ring at " << batch_headers_region << " (" << BATCHHEADERS_SIZE << " bytes)";

		// Initialize tinode metadata
		tinode->order = order;
		tinode->replication_factor = replication_factor;
		tinode->ack_level = ack_level;
		tinode->replicate_tinode = replicate_tinode;
		tinode->seq_type = seq_type;
		memset(tinode->topic, 0, TOPIC_NAME_SIZE);
		memcpy(tinode->topic, topic, std::min<size_t>(TOPIC_NAME_SIZE - 1, strlen(topic)));

		// [[ROOT_CAUSE_B_FIX]] - Flush TInode metadata after head broker initialization
		CXL::flush_cacheline(tinode);
		CXL::store_fence();

		// Initialize tinode offsets for this broker
		InitializeTInodeOffsets(tinode, segment_metadata, batch_headers_region, cxl_addr);

		// Handle replica if needed
		if (replicate_tinode) {
			char replica_topic[TOPIC_NAME_SIZE] = {0};
			memcpy(replica_topic, topic, std::min<size_t>(TOPIC_NAME_SIZE - 1, strlen(topic)));
			const char* suffix = "replica";
			size_t rep_len = strlen(replica_topic);
			size_t suffix_len = strlen(suffix);
			if (rep_len + suffix_len < TOPIC_NAME_SIZE) {
				memcpy(replica_topic + rep_len, suffix, suffix_len);
			} else {
				memcpy(replica_topic + (TOPIC_NAME_SIZE - 1 - suffix_len), suffix, suffix_len);
			}

			replica_tinode = cxl_manager_.GetReplicaTInode(topic);

			if (replica_tinode->topic[0] != 0 && strncmp(replica_tinode->topic, replica_topic, TOPIC_NAME_SIZE) != 0) {
				LOG(ERROR) << "Replica topic name collides: " << replica_tinode->topic;
				return nullptr;
			}

			InitializeTInodeOffsets(replica_tinode, segment_metadata,
					batch_headers_region, cxl_addr);
			replica_tinode->order = order;
			replica_tinode->replication_factor = replication_factor;
			replica_tinode->ack_level = ack_level;
			replica_tinode->replicate_tinode = replicate_tinode;
			replica_tinode->seq_type = seq_type;
			memset(replica_tinode->topic, 0, TOPIC_NAME_SIZE);
			memcpy(replica_tinode->topic, replica_topic, std::min<size_t>(TOPIC_NAME_SIZE - 1, strlen(replica_topic)));

			// [[ROOT_CAUSE_B_FIX]] - Also flush replica TInode metadata
			CXL::flush_cacheline(replica_tinode);
			CXL::store_fence();
		}

		// Create the topic
		// [[DEVIATION_004]] - Using TInode.offset_entry instead of separate Bmeta region
		topics_[topic] = std::make_unique<Topic>(
				[this]() { return cxl_manager_.GetNewSegment(); },
				[this]() { return get_num_brokers_callback_(); },
				GetRegisteredBrokersCallback([this](absl::btree_set<int> &registered_brokers,
									MessageHeader** msg_to_order, TInode *tinode) -> int {
			return get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode); }),
				static_cast<void*>(tinode),
				replica_tinode,
				topic,
				broker_id_,
				order,
				seq_type,
				cxl_addr,
				segment_metadata
				);
	}

	// Handle replication if needed
	if (tinode->seq_type == EMBARCADERO && replication_factor > 0) {
		disk_manager_.Replicate(tinode, replica_tinode, replication_factor);
	}
	// Run sequencer if needed
	if (tinode->seq_type == SCALOG) {
		if (replication_factor > 0) {
			disk_manager_.StartScalogReplicaLocalSequencer();
		}
	}

	return tinode;
}

bool TopicManager::CreateNewTopic(
        const char topic[TOPIC_NAME_SIZE], 
        int order, 
        int replication_factor,
        bool replicate_tinode,
        int ack_level,
        heartbeat_system::SequencerType seq_type) {
	if (shutting_down_.load(std::memory_order_acquire)) return false;
	
	// Direct call without string interning overhead
	struct TInode* tinode = CreateNewTopicInternal(
		topic, order, replication_factor, 
		replicate_tinode, ack_level, seq_type);
		
	if (tinode) {
		return true;
	} else {
		LOG(ERROR) << "Topic already exists!";
		return false;
	}
}

void TopicManager::DeleteTopic(const char topic[TOPIC_NAME_SIZE]) {
	// Implementation placeholder
}

std::function<void(void*, size_t)> TopicManager::GetCXLBuffer(
		BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE], 
		void* &log, 
		void* &segment_header, 
		size_t &logical_offset, 
		SequencerType &seq_type,
		BatchHeader* &batch_header_location,
		bool epoch_already_checked) {
	if (shutting_down_.load(std::memory_order_acquire)) return nullptr;
	
	// DEADLOCK FIX: Only head broker creates topics to prevent concurrent creation deadlocks.
	// [[B0_ACK_BUG1_ROOT_CAUSE]] Do NOT create topic here with order=0. If we do, the topic is
	// created with Order 0 semantics; when the client wanted Order 5, Sequencer5 never starts,
	// B0 scanner is never spawned, and CV[0] never advances -> B0=0 ACKs. Client must call
	// CreateNewTopic (with desired order) first; we return nullptr so the client retries until
	// the topic exists (created by the heartbeat CreateNewTopic RPC).
	struct TInode* tinode = cxl_manager_.GetTInode(topic);
	if (!tinode || tinode->topic[0] == 0) {
		// Topic not yet created - client should have called CreateNewTopic first; retry will succeed
		VLOG(1) << "Topic " << topic << " not yet created (broker_id=" << broker_id_
		        << "). Client should call CreateNewTopic first; returning nullptr for retry.";
		return nullptr;
	}
	
	// Fast path: try to find topic without locking first
	auto topic_itr = topics_.end();
	{
		absl::ReaderMutexLock lock(&topics_mutex_);
		topic_itr = topics_.find(topic);
	}

	if (topic_itr == topics_.end()) {
		// Topic not found locally, but should exist in CXL if head broker created it
		// Create local reference to the existing CXL topic
		tinode = CreateNewTopicInternal(topic);
		if (tinode) {
			absl::ReaderMutexLock lock(&topics_mutex_);
			topic_itr = topics_.find(topic);
		} else {
			LOG(ERROR) << "Failed to create local topic reference for: " << topic;
			return nullptr;
		}
	}

	// Final lookup with proper locking
	{
		absl::ReaderMutexLock lock(&topics_mutex_);
		topic_itr = topics_.find(topic);
		if (topic_itr == topics_.end()) {
			LOG(ERROR) << "Topic disappeared: " << topic;
			return nullptr;
		}
		
		auto& topic_obj = topic_itr->second;
		seq_type = topic_obj->GetSeqtype();
		return topic_obj->GetCXLBuffer(
				batch_header, topic, log, segment_header, logical_offset, batch_header_location, epoch_already_checked);
	}
}

bool TopicManager::ReserveBLogSpace(const char* topic, size_t size, void*& log) {
	if (shutting_down_.load(std::memory_order_acquire)) return false;
	// [[RECV_DIRECT_TO_CXL]] Topic must already exist (created by CreateNewTopic with correct order).
	// Do not create here with order=0 - see ACK_PIPELINE_BUG1_B0_ROOT_CAUSE.md
	struct TInode* tinode = cxl_manager_.GetTInode(topic);
	if (!tinode || tinode->topic[0] == 0) {
		return false;  // Client retry until CreateNewTopic has run
	}
	{
		absl::ReaderMutexLock lock(&topics_mutex_);
		auto topic_itr = topics_.find(topic);
		if (topic_itr != topics_.end()) {
			log = topic_itr->second->ReserveBLogSpace(size);
			return (log != nullptr);
		}
	}
	// Topic not in map yet; create and single lookup
	tinode = CreateNewTopicInternal(topic);
	if (!tinode) return false;
	absl::ReaderMutexLock lock(&topics_mutex_);
	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()) return false;
	log = topic_itr->second->ReserveBLogSpace(size);
	return (log != nullptr);
}

bool TopicManager::ReserveBLogSpace(Topic* topic_ptr, size_t size, void*& log, bool epoch_already_checked) {
	if (!topic_ptr) return false;
	log = topic_ptr->ReserveBLogSpace(size, epoch_already_checked);
	return (log != nullptr);
}

Topic* TopicManager::GetTopic(const std::string& topic_name) {
	if (shutting_down_.load(std::memory_order_acquire)) return nullptr;
	absl::ReaderMutexLock lock(&topics_mutex_);
	auto it = topics_.find(topic_name);
	return (it == topics_.end()) ? nullptr : it->second.get();
}

bool TopicManager::IsPBRAboveHighWatermark(const char* topic, int high_pct) {
	absl::ReaderMutexLock lock(&topics_mutex_);
	auto it = topics_.find(topic);
	if (it == topics_.end()) return false;
	return it->second->IsPBRAboveHighWatermark(high_pct);
}

bool TopicManager::IsPBRAboveHighWatermark(Topic* topic_ptr, int high_pct) {
	return topic_ptr ? topic_ptr->IsPBRAboveHighWatermark(high_pct) : false;
}

bool TopicManager::IsPBRBelowLowWatermark(const char* topic, int low_pct) {
	absl::ReaderMutexLock lock(&topics_mutex_);
	auto it = topics_.find(topic);
	if (it == topics_.end()) return true;
	return it->second->IsPBRBelowLowWatermark(low_pct);
}

bool TopicManager::ReservePBRSlotAndWriteEntry(const char* topic, BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location) {
	// Topic must already exist (created by ReserveBLogSpace or prior GetCXLBuffer)
	absl::ReaderMutexLock lock(&topics_mutex_);
	auto it = topics_.find(topic);
	if (it == topics_.end()) return false;
	return it->second->ReservePBRSlotAndWriteEntry(batch_header, log, segment_header, logical_offset, batch_header_location);
}

bool TopicManager::ReservePBRSlotAfterRecv(const char* topic, BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location) {
	// Topic must already exist (created by ReserveBLogSpace or prior GetCXLBuffer)
	absl::ReaderMutexLock lock(&topics_mutex_);
	auto it = topics_.find(topic);
	if (it == topics_.end()) return false;
	return it->second->ReservePBRSlotAfterRecv(batch_header, log, segment_header, logical_offset, batch_header_location);
}

bool TopicManager::PublishPBRSlotAfterRecv(const char* topic, const BatchHeader& batch_header, BatchHeader* batch_header_location) {
	absl::ReaderMutexLock lock(&topics_mutex_);
	auto it = topics_.find(topic);
	if (it == topics_.end()) return false;
	return it->second->PublishPBRSlotAfterRecv(batch_header, batch_header_location);
}

bool TopicManager::ReservePBRSlotAndWriteEntry(Topic* topic_ptr, BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	return topic_ptr && topic_ptr->ReservePBRSlotAndWriteEntry(batch_header, log, segment_header, logical_offset, batch_header_location, epoch_already_checked);
}

bool TopicManager::ReservePBRSlotAfterRecv(Topic* topic_ptr, BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	return topic_ptr && topic_ptr->ReservePBRSlotAfterRecv(batch_header, log, segment_header, logical_offset, batch_header_location, epoch_already_checked);
}

bool TopicManager::PublishPBRSlotAfterRecv(Topic* topic_ptr, const BatchHeader& batch_header, BatchHeader* batch_header_location) {
	return topic_ptr && topic_ptr->PublishPBRSlotAfterRecv(batch_header, batch_header_location);
}

bool TopicManager::GetBatchToExport(
		const char* topic,
		size_t &expected_batch_offset,
		void* &batch_addr,
		size_t &batch_size) {

	absl::ReaderMutexLock lock(&topics_mutex_);

	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()) {
		// Not throwing error as subscribe can be called before topic creation
		return false;
	}

	return topic_itr->second->GetBatchToExport(expected_batch_offset, batch_addr, batch_size);
}

bool TopicManager::GetBatchToExportWithMetadata(
		const char* topic,
		size_t &expected_batch_offset,
		void* &batch_addr,
		size_t &batch_size,
		size_t &batch_total_order,
		uint32_t &num_messages) {

	absl::ReaderMutexLock lock(&topics_mutex_);

	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()) {
		// Not throwing error as subscribe can be called before topic creation
		return false;
	}

	return topic_itr->second->GetBatchToExportWithMetadata(expected_batch_offset, batch_addr, batch_size, batch_total_order, num_messages);
}

bool TopicManager::GetMessageAddr(
		const char* topic, 
		size_t &last_offset,
		void* &last_addr, 
		void* &messages, 
		size_t &messages_size) {

	absl::ReaderMutexLock lock(&topics_mutex_);

	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()) {
		// Not throwing error as subscribe can be called before topic creation
		return false;
	}

	return topic_itr->second->GetMessageAddr(last_offset, last_addr, messages, messages_size);
}

int TopicManager::GetTopicOrder(const char* topic){
	topics_mutex_.ReaderLock();

	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()) {
		const TInode* tinode = cxl_manager_.GetTInode(topic);
		// Relax creation criteria: if remote TInode has any non-empty name, create locally
		bool has_remote_topic = (tinode != nullptr) && (tinode->topic[0] != 0);
		if (has_remote_topic) {
			// Topic was created from another broker, create it locally
			topics_mutex_.ReaderUnlock();
			CreateNewTopicInternal(topic);
			topics_mutex_.ReaderLock();
			topic_itr = topics_.find(topic);

			if (topic_itr == topics_.end()) {
				LOG(ERROR) << "Topic:" << topic << " Does not Exist!!";
				topics_mutex_.ReaderUnlock();
				return 0;
			}
		} else {
			LOG(ERROR) << "[GetTopicOrder] Topic: " << topic 
				<< " was not created before: " << (tinode ? tinode->topic : "null");
			topics_mutex_.ReaderUnlock();
			return 0;
		}
	}

	topics_mutex_.ReaderUnlock();
	return topic_itr->second->GetOrder();
}

} // End of namespace Embarcadero
