#include "topic_manager.h"

#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <immintrin.h>
#include "folly/ConcurrentSkipList.h"

namespace Embarcadero{

#define NT_THRESHOLD 128

void memcpy_nt(void* dst, const void* src, size_t size) {
    // Cast the input pointers to the appropriate types
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // Align the destination pointer to 16-byte boundary
    size_t alignment = reinterpret_cast<uintptr_t>(d) & 0xF;
    if (alignment) {
        alignment = 16 - alignment;
        size_t copy_size = (alignment > size) ? size : alignment;
        std::memcpy(d, s, copy_size);
        d += copy_size;
        s += copy_size;
        size -= copy_size;
    }

    // Copy the bulk of the data using non-temporal stores
    size_t block_size = size / 64;
    for (size_t i = 0; i < block_size; ++i) {
        _mm_stream_si64(reinterpret_cast<long long*>(d), *reinterpret_cast<const long long*>(s));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 8), *reinterpret_cast<const long long*>(s + 8));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 16), *reinterpret_cast<const long long*>(s + 16));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 24), *reinterpret_cast<const long long*>(s + 24));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 32), *reinterpret_cast<const long long*>(s + 32));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 40), *reinterpret_cast<const long long*>(s + 40));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 48), *reinterpret_cast<const long long*>(s + 48));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 56), *reinterpret_cast<const long long*>(s + 56));
        d += 64;
        s += 64;
    }

    // Copy the remaining data using standard memcpy
    std::memcpy(d, s, size % 64);
}
void nt_memcpy(void *__restrict dst, const void * __restrict src, size_t n){
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

void TopicManager::CreateNewTopic(char topic[32]){
	// Get and initialize tinode
	void* segment_metadata = cxl_manager_.GetNewSegment();
	struct TInode* tinode = (struct TInode*)cxl_manager_.GetTInode(topic);
	memcpy(tinode->topic, topic, 32);
	tinode->offsets[broker_id_].ordered = -1;
	tinode->offsets[broker_id_].written = -1;
	tinode->offsets[broker_id_].log_addr = (uint8_t*)segment_metadata + CACHELINE_SIZE;

	//TODO(Jae) topics_ should be in a critical section
	// But addition and deletion of a topic in our case is rare
	// We will leave it this way for now but this needs to be fixed
	topics_[topic] = std::make_unique<Topic>([this](){return cxl_manager_.GetNewSegment();},
			tinode, topic, broker_id_, segment_metadata);
	topics_[topic]->Combiner();
}

void TopicManager::DeleteTopic(char topic[32]){
}

void TopicManager::PublishToCXL(PublishRequest &req){
	auto topic_itr = topics_.find(req.topic);
	//TODO(Jae) if not found from topics_, inspect CXL TInode region too
	if (topic_itr == topics_.end()){
		if(memcmp(req.topic, ((struct TInode*)(cxl_manager_.GetTInode(req.topic)))->topic, 32));
		perror("Topic not found");
	}
	topic_itr->second->PublishToCXL(req);
}

bool TopicManager::GetMessageAddr(const char* topic, size_t &last_offset,
																	void* &last_addr, void* messages, size_t &messages_size){
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
						order_(0),
						segment_metadata_((struct MessageHeader**)segment_metadata){
	logical_offset_ = 0;
	written_logical_offset_ = -1;
	remaining_size_ = SEGMENT_SIZE - sizeof(void*);
	log_addr_.store((unsigned long long int)tinode_->offsets[broker_id_].log_addr);
	first_message_addr_ = tinode_->offsets[broker_id_].log_addr;
	ordered_offset_addr_ = nullptr;
	prev_msg_header_ = nullptr;
	ordered_offset_ = 0;
}

void Topic::CombinerThread(){
	const static size_t header_size = sizeof(MessageHeader);
	static void* segment_header = (uint8_t*)first_message_addr_ - CACHELINE_SIZE;
	std::cout << "Combiner called " << std::endl;
	MessageHeader *header = (MessageHeader*)first_message_addr_;
	while(true || !stop_threads_){
		while(header->paddedSize == 0){
			if(stop_threads_){
				return;
			}
			std::this_thread::yield();
		}
#ifdef MULTISEGMENT
		if(header->next_message != nullptr){ // Moved to new segment
			header = header->next_message;
			segment_header = (uint8_t*)header - CACHELINE_SIZE;
			continue;
		}
#endif
		header->segment_header = segment_header;
		header->logical_offset = logical_offset_;
		tinode_->offsets[broker_id_].written = logical_offset_;
		logical_offset_++;
		header->next_message = (uint8_t*)header + header->paddedSize + header_size;
		header = (MessageHeader*)header->next_message;
	}
}

// Give logical order, not total order to messages. 
// Order=0 can export these ordeerd messages. 1 and 2 should wait for sequencer
void Topic::Combiner(){
	combiningThreads_.emplace_back(&Topic::CombinerThread, this);
}

// MessageHeader is already included from network manager
// For performance (to not have any mutex) have a separate combiner to give logical offsets  to the messages
void Topic::PublishToCXL(PublishRequest &req){
	static unsigned long long int segment_metadata = (unsigned long long int)segment_metadata_;
	static const size_t msg_header_size = sizeof(struct MessageHeader);

	size_t reqSize = req.size + msg_header_size;
	size_t padding = CACHELINE_SIZE - (reqSize%CACHELINE_SIZE);
	size_t msgSize = req.size + padding;

	void* log = (void*)log_addr_.fetch_add(msgSize);
	if(segment_metadata + SEGMENT_SIZE <= (unsigned long long int)log + msgSize){
		std::cout << "!!!!!!!!! Increase the Segment Size" << std::endl;
		//TODO(Jae) Finish below segment boundary crossing code
		if(segment_metadata + SEGMENT_SIZE <= (unsigned long long int)log){
			// Allocate a new segment
			// segment_metadata_ = (struct MessageHeader**)get_new_segment_callback_();
			//segment_metadata = (unsigned long long int)segment_metadata_;
		}else{
			// Wait for the first thread that crossed the segment to allocate a new segment
			//segment_metadata = (unsigned long long int)segment_metadata_;
		}
	}
	memcpy_nt((uint8_t*)log + msg_header_size, req.payload_address, msgSize);
}

// Current implementation depends on the subscriber knows the physical address of last fetched message
// This is only true if the messages were exported from CXL. If we implement disk cache optimization, 
// we need to fix it. Probably need to have some sort of indexing or call this method to get indexes
// even if at cache hit (without sending the messages)
//
// arguments: do not call this function again if this variable is nullptr
// if the messages to export go over the segment boundary (not-contiguous), 
// we should call this functiona again
bool Topic::GetMessageAddr(size_t &last_offset,
						   void* &last_addr, void* messages, size_t &messages_size){
	static size_t header_size = sizeof(struct MessageHeader);
	//TODO(Jae) replace this line after test
	//if(writing_offsets_ < tinode_->ordered)
	if(written_logical_offset_ < (int)last_offset){
		return false;
	}

	struct MessageHeader *start_msg_header = (struct MessageHeader*)last_addr;
	if(last_addr != nullptr){
		start_msg_header = (struct MessageHeader*)start_msg_header->next_message;
	}else{
		start_msg_header = (struct MessageHeader*)first_message_addr_;
	}

	messages = (void*)start_msg_header;
	struct MessageHeader** segment_header = (struct MessageHeader**)start_msg_header->segment_header;
	struct MessageHeader *last_msg_of_segment =(*segment_header);
	if((last_msg_of_segment->size + header_size + (uint8_t*)last_msg_of_segment) ==  written_physical_addr_){
		last_addr = nullptr; 
		messages_size = ((uint8_t*)written_physical_addr_ - (uint8_t*)start_msg_header);
	}else{
		messages_size =((uint8_t*)last_msg_of_segment - (uint8_t*)start_msg_header) + last_msg_of_segment->size + header_size; 
		last_offset = last_msg_of_segment->logical_offset;
		last_addr = (void*)last_msg_of_segment;
	}

	struct MessageHeader *m = (struct MessageHeader*)messages;
	size_t len = messages_size;
	while(len>0){
		char* msg = (char*)((uint8_t*)m + header_size);
		len -= header_size;
		std::cout<< msg << std::endl;
		len -= m->size;
		m =  (struct MessageHeader*)m->next_message;
	}

	return true;
}


} // End of namespace Embarcadero
