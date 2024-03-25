#include "topic_manager.h"

#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <immintrin.h>

namespace Embarcadero{

#define NT_THRESHOLD 128


void nt_memcpy(void *__restrict dst, const void * __restrict src, size_t n)
{
	static size_t CACHE_LINE_SIZE = sysconf (_SC_LEVEL1_DCACHE_LINESIZE);
	if (n < NT_THRESHOLD) {
		memcpy(dst, src, n);
		return;
	}

	size_t n_unaligned = CACHE_LINE_SIZE - (uintptr_t)dst % CACHE_LINE_SIZE;

	if (n_unaligned > n)
		n_unaligned = n;

	memcpy(dst, src, n_unaligned);
	dst = (void*)(((uint8_t*)dst) + n_unaligned);
	src = (void*)(((uint8_t*)src) + n_unaligned);
	n -= n_unaligned;

	size_t num_lines = n / CACHE_LINE_SIZE;

	size_t i;
	for (i = 0; i < num_lines; i++) {
		size_t j;
		for (j = 0; j < CACHE_LINE_SIZE / sizeof(__m128i); j++) {
			__m128i blk = _mm_loadu_si128((const __m128i *)src);
			/* non-temporal store */
			_mm_stream_si128((__m128i *)dst, blk);
			src = (void*)(((uint8_t*)src) + sizeof(__m128i));
			dst = (void*)(((uint8_t*)dst) + sizeof(__m128i));
		}
		n -= CACHE_LINE_SIZE;
	}

	if (num_lines > 0)
		_mm_sfence();

	memcpy(dst, src, n);
}

void TopicManager::CreateNewTopic(const char topic[32]){
	// Get and initialize tinode
	void* segment_metadata = cxl_manager_.GetNewSegment();
	struct TInode* tinode = (struct TInode*)cxl_manager_.GetTInode(topic, broker_id_);
	memcpy(tinode->topic, topic, 32);
	tinode->offsets[broker_id_].ordered = 0;
	tinode->offsets[broker_id_].written = 0;
	tinode->offsets[broker_id_].log_addr = (uint8_t*)segment_metadata + sizeof(void*);

	//TODO(Jae) topics_ should be in a critical section
	// But addition and deletion of a topic in our case is rare
	// We will leave it this way for now but this needs to be fixed
	topics_[topic] = std::make_unique<Topic>([this](){return cxl_manager_.GetNewSegment();},
			tinode, topic, broker_id_, segment_metadata);
}

void TopicManager::DeleteTopic(char topic[32]){
}

void TopicManager::PublishToCXL(char topic[32], void* message, size_t size){
	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()){
		perror("Topic not found");
	}
	topic_itr->second->PublishToCXL(message, size);
}

bool TopicManager::GetMessageAddr(const char* topic, size_t &last_offset,
																	void* last_addr, void* messages, size_t &messages_size){
	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()){
		perror("Topic not found");
	}
	return topic_itr->second->GetMessageAddr(last_offset, last_addr, messages, messages_size);
}

Topic::Topic(GetNewSegmentCallback get_new_segment, void* TInode_addr, const char* topic_name,
					int broker_id, void* segment_metadata):
						get_new_segment_callback_(get_new_segment),
						tinode_(static_cast<struct TInode*>(TInode_addr)),
						topic_name_(topic_name),
						broker_id_(broker_id),
						segment_metadata_(segment_metadata){
	logical_offset_ = 0;
	written_logical_offset_ = -1;
	remaining_size_ = SEGMENT_SIZE - sizeof(void*);
	log_addr_ = tinode_->offsets[broker_id_].log_addr;
	first_message_addr_ = tinode_->offsets[broker_id_].log_addr;
	ordered_offset_addr_ = nullptr;
	ordered_offset_ = 0;

	//TODO(Jae) have cache for disk as well
}

void Topic::PublishToCXL(void* message, size_t size){
	void* log;
	int logical_offset;
	static const size_t msg_header_size = sizeof(struct MessageHeader);
	{
		//absl::MutexLock lock(&mu_);
		std::unique_lock<std::mutex> lock(mu_);
		logical_offset = logical_offset_;
		logical_offset_++;
		remaining_size_ -= size;
		if(remaining_size_ >= 0){
			log = log_addr_;
			log_addr_ = (uint8_t*)log_addr_ + size + msg_header_size;
		}else{
			segment_metadata_ = get_new_segment_callback_();
			log = (uint8_t*)segment_metadata_ + sizeof(void*);
			if(prev_msg_header_ != nullptr)
				prev_msg_header_->next_message = log;
			else
				perror("Increase the segment size. Only one message fits in a segment");
			log_addr_ = (uint8_t*)log + size + msg_header_size;
			remaining_size_ = SEGMENT_SIZE - size - msg_header_size - sizeof(void*);
		}
		prev_msg_header_ = (struct MessageHeader*)log;
		writing_offsets_.insert(logical_offset);
	}

	struct NonCriticalMessageHeader msg_header;
	msg_header.logical_offset = logical_offset;
	msg_header.size = size;
	msg_header.segment_header = segment_metadata_;

	nt_memcpy(log, &msg_header, sizeof(msg_header));
	nt_memcpy((uint8_t*)log + msg_header_size, message, size);

	{
		//absl::MutexLock lock(&mu_);
		std::unique_lock<std::mutex> lock(mu_);
		if (*(writing_offsets_.begin()++) == logical_offset){
			written_logical_offset_ = logical_offset;
			while(!not_contigous_.empty() && not_contigous_.top() == written_logical_offset_ + 1){
				not_contigous_.pop();
				written_logical_offset_++;
			}
			tinode_->offsets[broker_id_].written = written_logical_offset_;
		}else{
			not_contigous_.push(logical_offset);
		}
		writing_offsets_.erase(logical_offset);
	}
}
bool Topic::GetMessageAddr(size_t &last_offset,
														void* last_addr, void* messages, size_t &messages_size){
	//TODO(Jae) replace this line after test
	//if(writing_offsets_ < tinode_->ordered)
	if(written_logical_offset_ < last_offset){
		return false;
	}
	size_t subscriber_offset = last_offset;
	if(last_offset == 0){
		last_addr = first_message_addr_;
	}

	struct MessageHeader *start_msg_header = (struct MessageHeader*)last_addr;
	if(start_msg_header->next_message != nullptr){
		start_msg_header = (struct MessageHeader*)start_msg_header->next_message;
	}else{
		perror("GetMessageAddr Something's wrong. Message header not set");
		return false;
	}
	messages = (void*)start_msg_header;
	messages_size = ((uint8_t*)written_physical_addr_ - (uint8_t*)start_msg_header);
	if(messages_size < (2*SEGMENT_SIZE)){
		return true;
	}else{
		struct MessageHeader* last_msg_header = (struct MessageHeader*)(start_msg_header->segment_header);
		messages_size =((uint8_t*)last_msg_header - (uint8_t*)start_msg_header) + last_msg_header->size; 
		last_offset = last_msg_header->logical_offset;
		last_addr = (void*)last_msg_header;
	}

	return true;
}

} // End of namespace Embarcadero
