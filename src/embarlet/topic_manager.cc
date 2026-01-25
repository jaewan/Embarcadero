#include "topic_manager.h"
#include <glog/logging.h>
#include <cstring>
#include <algorithm>
#include "common/performance_utils.h"
#include <immintrin.h>
#include <xmmintrin.h>  // For _mm_pause()

// Project includes
#include "topic_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include "../disk_manager/disk_manager.h"

namespace Embarcadero {

constexpr size_t NT_THRESHOLD = 128;

/**
 * Non-temporal memory copy function optimized for large data transfers
 * Uses streaming stores to bypass cache for large copies
 */
void nt_memcpy(void* __restrict dst, const void* __restrict src, size_t size) {
	static const size_t CACHE_LINE_SIZE = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	// For small copies, use standard memcpy
	if (size < NT_THRESHOLD) {
		memcpy(dst, src, size);
		return;
	}

	// Handle unaligned portion at the beginning
	const uintptr_t dst_addr = reinterpret_cast<uintptr_t>(dst);
	const size_t unaligned_bytes = (CACHE_LINE_SIZE - dst_addr % CACHE_LINE_SIZE) % CACHE_LINE_SIZE;
	const size_t initial_bytes = std::min(unaligned_bytes, size);

	if (initial_bytes > 0) {
		memcpy(dst, src, initial_bytes);
	}

	uint8_t* aligned_dst = static_cast<uint8_t*>(dst) + initial_bytes;
	const uint8_t* aligned_src = static_cast<const uint8_t*>(src) + initial_bytes;
	size_t remaining = size - initial_bytes;

	// Process cache-line-aligned data with non-temporal stores
	const size_t num_lines = remaining / CACHE_LINE_SIZE;
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
		remaining -= CACHE_LINE_SIZE;
	}

	// Copy any remaining bytes
	if (remaining > 0) {
		memcpy(aligned_dst, aligned_src, remaining);
	}
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

	// Calculate log offset using pointer difference plus CACHELINE_SIZE
	const uintptr_t segment_addr = reinterpret_cast<uintptr_t>(segment_metadata);
	const uintptr_t cxl_base_addr = reinterpret_cast<uintptr_t>(cxl_addr);
	tinode->offsets[broker_id_].log_offset = 
		static_cast<size_t>(segment_addr + CACHELINE_SIZE - cxl_base_addr);

	// Calculate batch headers offset using pointer difference
	const uintptr_t batch_headers_addr = reinterpret_cast<uintptr_t>(batch_headers_region);
	tinode->offsets[broker_id_].batch_headers_offset = 
		static_cast<size_t>(batch_headers_addr - cxl_base_addr);

	// [[ROOT_CAUSE_B_FIX]] - Flush broker-specific offset initialization
	// After each broker initializes its offsets[broker_id_] entries (log_offset/batch_headers_offset/written_addr)
	// Flush the broker region (first 256B of offset_entry) so other threads see the values
	const void* broker_region = const_cast<const void*>(static_cast<const volatile void*>(&tinode->offsets[broker_id_].log_offset));
	CXL::flush_cacheline(broker_region);
	CXL::store_fence();
}

struct TInode* TopicManager::CreateNewTopicInternal(const char topic[TOPIC_NAME_SIZE]) {
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
		void* segment_metadata = cxl_manager_.GetNewSegment();
		void* batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();
		
		// Validate all pointers before using them
		if (!segment_metadata) {
			LOG(ERROR) << "Failed to allocate segment for topic: " << topic;
			return nullptr;
		}
		if (!batch_headers_region) {
			LOG(ERROR) << "Failed to allocate batch headers for topic: " << topic;
			return nullptr;
		}

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

	return tinode;
}

struct TInode* TopicManager::CreateNewTopicInternal(
		const char topic[TOPIC_NAME_SIZE],
		int order,
		int replication_factor,
		bool replicate_tinode,
		int ack_level,
		SequencerType seq_type) {

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
		void* segment_metadata = cxl_manager_.GetNewSegment();
		void* batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();
		
		// Validate all pointers before using them
		if (!segment_metadata) {
			LOG(ERROR) << "Failed to allocate segment for topic: " << topic;
			return nullptr;
		}
		if (!batch_headers_region) {
			LOG(ERROR) << "Failed to allocate batch headers for topic: " << topic;
			return nullptr;
		}

		// Initialize tinode
		InitializeTInodeOffsets(tinode, segment_metadata, batch_headers_region, cxl_addr);
		tinode->order = order;
		tinode->replication_factor = replication_factor;
		tinode->ack_level = ack_level;
		tinode->replicate_tinode = replicate_tinode;
		tinode->seq_type = seq_type;
		memset(tinode->topic, 0, TOPIC_NAME_SIZE);
		memcpy(tinode->topic, topic, std::min<size_t>(TOPIC_NAME_SIZE - 1, strlen(topic)));

		// [[ROOT_CAUSE_B_FIX]] - Flush TInode metadata after head broker initialization
		// Non-head brokers must see topic/order/ack_level/seq_type reliably
		// Flush first 64B (cacheline) containing: topic, replicate_tinode, order, replication_factor, ack_level, seq_type
		CXL::flush_cacheline(tinode);
		CXL::store_fence();

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
		BatchHeader* &batch_header_location) {
	
	// DEADLOCK FIX: Only head broker creates topics to prevent concurrent creation deadlocks
	struct TInode* tinode = cxl_manager_.GetTInode(topic);
	if (!tinode || tinode->topic[0] == 0) {
		if (broker_id_ != 0) {
			// Non-head brokers wait for head to create the topic
			LOG(INFO) << "Broker " << broker_id_ << " waiting for head broker to create topic: " << topic;
			return nullptr;  // Return early, client will retry
		}
		// Only head broker creates new topics
		LOG(INFO) << "Head broker creating new topic: " << topic;
		tinode = CreateNewTopicInternal(topic, 0, 0, false, 0, EMBARCADERO);
		if (!tinode) {
			LOG(ERROR) << "Head broker failed to create topic: " << topic;
			return nullptr;
		}
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
				batch_header, topic, log, segment_header, logical_offset, batch_header_location);
	}
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
