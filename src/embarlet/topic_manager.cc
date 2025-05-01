#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <immintrin.h>

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
	tinode->offsets[broker_id_].ordered = -1;
	tinode->offsets[broker_id_].written = -1;
	for ( int i = 0; i < NUM_MAX_BROKERS; i++ ) {
		tinode->offsets[broker_id_].replication_done[i] = -1;
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
}

struct TInode* TopicManager::CreateNewTopicInternal(const char topic[TOPIC_NAME_SIZE]) {
	struct TInode* tinode = cxl_manager_.GetTInode(topic);
	TInode* replica_tinode = nullptr;

	{
		absl::WriterMutexLock lock(&mutex_);

		CHECK_LT(num_topics_, MAX_TOPIC_SIZE) 
			<< "Creating too many topics, increase MAX_TOPIC_SIZE";

		if (topics_.find(topic) != topics_.end()) {
			return nullptr;
		}

		void* cxl_addr = cxl_manager_.GetCXLAddr();
		void* segment_metadata = cxl_manager_.GetNewSegment();
		void* batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();

		// Handle replica if needed
		if (tinode->replicate_tinode) {
			replica_tinode = cxl_manager_.GetReplicaTInode(topic);
			InitializeTInodeOffsets(replica_tinode, segment_metadata, 
					batch_headers_region, cxl_addr);
		}

		InitializeTInodeOffsets(tinode, segment_metadata, batch_headers_region, cxl_addr);

		// Create the topic
		topics_[topic] = std::make_unique<Topic>(
				[this]() { return cxl_manager_.GetNewSegment(); },
				[this]() { return get_num_brokers_callback_(); },
				[this](absl::btree_set<int> &registered_brokers, 
														struct MessageHeader** msg_to_order, struct TInode *tinode) { 
				return get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode); },
				tinode,
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
		cxl_manager_.RunScalogSequencer(topic);
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

	// Check for name collision in tinode
	const bool no_collision = std::all_of(
			reinterpret_cast<const unsigned char*>(tinode->topic),
			reinterpret_cast<const unsigned char*>(tinode->topic) + TOPIC_NAME_SIZE,
			[](unsigned char c) { return c == 0; }
			);

	if (!no_collision) {
		LOG(ERROR) << "Topic name collides: " << tinode->topic << " handle collision";
		exit(1);
	}

	{
		absl::WriterMutexLock lock(&mutex_);

		CHECK_LT(num_topics_, MAX_TOPIC_SIZE) 
			<< "Creating too many topics, increase MAX_TOPIC_SIZE";

		if (topics_.find(topic) != topics_.end()) {
			return nullptr;
		}

		void* cxl_addr = cxl_manager_.GetCXLAddr();
		void* segment_metadata = cxl_manager_.GetNewSegment();
		void* batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();

		// Initialize tinode
		InitializeTInodeOffsets(tinode, segment_metadata, batch_headers_region, cxl_addr);
		tinode->order = order;
		tinode->replication_factor = replication_factor;
		tinode->ack_level = ack_level;
		tinode->replicate_tinode = replicate_tinode;
		tinode->seq_type = seq_type;
		memcpy(tinode->topic, topic, TOPIC_NAME_SIZE);

		// Handle replica if needed
		if (replicate_tinode) {
			char replica_topic[TOPIC_NAME_SIZE] = {0};
			memcpy(replica_topic, topic, TOPIC_NAME_SIZE);
			memcpy(replica_topic + (TOPIC_NAME_SIZE - 7), "replica", 7);

			replica_tinode = cxl_manager_.GetReplicaTInode(topic);

			const bool replica_no_collision = std::all_of(
					reinterpret_cast<const unsigned char*>(replica_tinode->topic),
					reinterpret_cast<const unsigned char*>(replica_tinode->topic) + TOPIC_NAME_SIZE,
					[](unsigned char c) { return c == 0; }
					);

			if (!replica_no_collision) {
				LOG(ERROR) << "Replica topic name collides: " << replica_tinode->topic 
					<< " handle collision";
				exit(1);
			}

			InitializeTInodeOffsets(replica_tinode, segment_metadata, 
					batch_headers_region, cxl_addr);
			replica_tinode->order = order;
			replica_tinode->replication_factor = replication_factor;
			replica_tinode->ack_level = ack_level;
			replica_tinode->replicate_tinode = replicate_tinode;
			replica_tinode->seq_type = seq_type;
			memcpy(replica_tinode->topic, replica_topic, TOPIC_NAME_SIZE);
		}

		// Create the topic
		topics_[topic] = std::make_unique<Topic>(
				[this]() { return cxl_manager_.GetNewSegment(); },
				[this]() { return get_num_brokers_callback_(); },
				[this](absl::btree_set<int> &registered_brokers, 
														struct MessageHeader** msg_to_order, struct TInode *tinode) { 
				return get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode); },
				tinode,
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
		cxl_manager_.RunScalogSequencer(topic);
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
        SequencerType seq_type) {
    
    TInode* tinode = CreateNewTopicInternal(
        topic, order, replication_factor, replicate_tinode, ack_level, seq_type);
        
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
		SequencerType &seq_type) {

	auto topic_itr = topics_.find(topic);

	if (topic_itr == topics_.end()) {
		// Check if topic exists in CXL but not in local map
		const TInode* tinode = cxl_manager_.GetTInode(topic);
		if (tinode && memcmp(topic, tinode->topic, TOPIC_NAME_SIZE) == 0) {
			// Topic was created from another broker, create it locally
			CreateNewTopicInternal(topic);
			topic_itr = topics_.find(topic);

			if (topic_itr == topics_.end()) {
				LOG(ERROR) << "Topic Entry was not created, something is wrong";
				return nullptr;
			}
		} else {
			LOG(ERROR) << "[GetCXLBuffer] Topic: " << topic 
				<< " was not created before: " << tinode->topic
				<< " memcmp: " << memcmp(topic, tinode->topic, TOPIC_NAME_SIZE);
			return nullptr;
		}
	}

	seq_type = topic_itr->second->GetSeqtype();
	return topic_itr->second->GetCXLBuffer(
			batch_header, topic, log, segment_header, logical_offset);
}

bool TopicManager::GetBatchToExport(
		const char* topic,
		size_t &expected_batch_offset,
		void* &batch_addr,
		size_t &batch_size) {

	absl::ReaderMutexLock lock(&mutex_);

	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()) {
		// Not throwing error as subscribe can be called before topic creation
		return false;
	}

	return topic_itr->second->GetBatchToExport(expected_batch_offset, batch_addr, batch_size);
}

bool TopicManager::GetMessageAddr(
		const char* topic, 
		size_t &last_offset,
		void* &last_addr, 
		void* &messages, 
		size_t &messages_size) {

	absl::ReaderMutexLock lock(&mutex_);

	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()) {
		// Not throwing error as subscribe can be called before topic creation
		return false;
	}

	return topic_itr->second->GetMessageAddr(last_offset, last_addr, messages, messages_size);
}

int TopicManager::GetTopicOrder(const char* topic){
	absl::ReaderMutexLock lock(&mutex_);

	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()) {
		LOG(ERROR) << "Topic:" << topic << " Does not Exist!!";
		// Not throwing error as subscribe can be called before topic creation
		return 0;
	}

	return topic_itr->second->GetOrder();
}

} // End of namespace Embarcadero
