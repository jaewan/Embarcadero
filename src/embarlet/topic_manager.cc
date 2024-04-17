#include "topic_manager.h"

#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <immintrin.h>
#include <glog/logging.h>


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

void TopicManager::CreateNewTopic(char topic[31], int order){
	// Get and initialize tinode
	void* segment_metadata = cxl_manager_.GetNewSegment();
	static void* cxl_addr = cxl_manager_.GetCXLAddr();
	struct TInode* tinode = (struct TInode*)cxl_manager_.GetTInode(topic);
	memcpy(tinode->topic, topic, 31);
	tinode->order= (uint8_t)order;
	tinode->offsets[broker_id_].ordered = -1;
	tinode->offsets[broker_id_].written = -1;
	tinode->offsets[broker_id_].log_offset = (size_t)((uint8_t*)segment_metadata + CACHELINE_SIZE - (uint8_t*)cxl_addr);
  DLOG(INFO) << "Created topic: \"" << topic << "\"";

	//TODO(Jae) topics_ should be in a critical section
	// But addition and deletion of a topic in our case is rare
	// We will leave it this way for now but this needs to be fixed
	topics_[topic] = std::make_unique<Topic>([this](){return cxl_manager_.GetNewSegment();},
			tinode, topic, broker_id_, order, cxl_addr, segment_metadata);
	topics_[topic]->Combiner();
}

void TopicManager::DeleteTopic(char topic[31]){
}

void TopicManager::PublishToCXL(struct RequestData *req_data){
	auto topic_itr = topics_.find(req_data->request_.topic().c_str());
	//TODO(Jae) if not found from topics_, inspect CXL TInode region too
	if (topic_itr == topics_.end()){
		if(memcmp(req_data->request_.topic().c_str(), ((struct TInode*)(cxl_manager_.GetTInode(req_data->request_.topic().c_str())))->topic, 31));
		perror("Topic not found");
	}
	topic_itr->second->PublishToCXL(req_data);
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
					int broker_id, int order, void* cxl_addr, void* segment_metadata):
						get_new_segment_callback_(get_new_segment),
						tinode_(static_cast<struct TInode*>(TInode_addr)),
						topic_name_(topic_name),
						broker_id_(broker_id),
						order_(order),
						cxl_addr_(cxl_addr),
						current_segment_(segment_metadata){
	logical_offset_ = 0;
	written_logical_offset_ = (size_t)-1;
	log_addr_.store((unsigned long long int)((uint8_t*)cxl_addr_ + tinode_->offsets[broker_id_].log_offset));
	first_message_addr_ = (uint8_t*)cxl_addr_ + tinode_->offsets[broker_id_].log_offset;
	ordered_offset_addr_ = nullptr;
	ordered_offset_ = 0;
}

void Topic::CombinerThread(){
	const static size_t header_size = sizeof(MessageHeader);
	void* segment_header = (uint8_t*)first_message_addr_ - CACHELINE_SIZE;
	MessageHeader *header = (MessageHeader*)first_message_addr_;
	while(!stop_threads_){
		while(header->paddedSize == 0){
			if(stop_threads_){
				LOG(INFO) << "Stopping CombinerThread" ;
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
		VLOG(3) << "Stopping CombinerThread" ;
		
		header->segment_header = segment_header;
		header->logical_offset = logical_offset_;
		header->next_message = (uint8_t*)header + header->paddedSize + header_size;
		tinode_->offsets[broker_id_].written = logical_offset_;
		(*(unsigned long long int*)segment_header) +=
		(unsigned long long int)((uint8_t*)header - (uint8_t*)segment_header);
		written_logical_offset_ = logical_offset_;
		written_physical_addr_ = (void*)header;
		logical_offset_++;
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
void Topic::PublishToCXL(struct RequestData *req_data){
	unsigned long long int segment_metadata = (unsigned long long int)current_segment_;
	static const size_t msg_header_size = sizeof(struct MessageHeader);

	size_t reqSize = req_data->request_.payload_size() + msg_header_size;
	size_t padding = req_data->request_.payload_size()%CACHELINE_SIZE;
	if(padding)
		padding = (CACHELINE_SIZE - padding);
	size_t msgSize = reqSize + padding;

	unsigned long long int log = log_addr_.fetch_add(msgSize);
	if(segment_metadata + SEGMENT_SIZE <= log + msgSize){
		LOG(INFO) << "!!!!!!!!! Increase the Segment Size:" << SEGMENT_SIZE;
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
	VLOG(2) << "log offset:" << log - (unsigned long long int)first_message_addr_;
	VLOG(2) << "payload addr:" << (void *)(req_data->request_.payload().c_str());
	const std::string& payload = req_data->request_.payload();

	nt_memcpy((void*)log, payload.data(), msgSize);
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
	static const size_t header_size = sizeof(struct MessageHeader);
	size_t digested_offset = written_logical_offset_;
	void* digested_addr = written_physical_addr_;
	if(order_ > 0){
		digested_offset = tinode_->offsets[broker_id_].ordered;
		digested_addr = (uint8_t*)cxl_addr_ + tinode_->offsets[broker_id_].ordered_offset;
	}
	if(digested_offset == (size_t)-1 || ((last_addr != nullptr) && (digested_offset <= last_offset))){
		std::cout<< "No messages to export digested_offset:" << digested_offset << std::endl;
		return false;
	}

	struct MessageHeader *start_msg_header = (struct MessageHeader*)last_addr;
	if(last_addr != nullptr){
		while((struct MessageHeader*)start_msg_header->next_message == nullptr){
			std::cout<< "[GetMessageAddr] waiting for the message to be combined " << std::endl;
			std::this_thread::yield();
		}
		start_msg_header = (struct MessageHeader*)start_msg_header->next_message;
	}else{
		if(digested_addr <= last_addr){
			perror("[GetMessageAddr] Wrong!!\n");
			return false;
		}
		start_msg_header = (struct MessageHeader*)first_message_addr_;
	}

	messages = (void*)start_msg_header;
	unsigned long long int* last_msg_off = (unsigned long long int*)start_msg_header->segment_header;
	struct MessageHeader *last_msg_of_segment = (MessageHeader*)((uint8_t*)last_msg_off + *last_msg_off);
	if(last_msg_of_segment >= written_physical_addr_){
		last_addr = nullptr; 
		messages_size = (uint8_t*)written_physical_addr_ - (uint8_t*)start_msg_header + ((MessageHeader*)written_physical_addr_)->paddedSize + header_size;
	}else{
		messages_size = (uint8_t*)last_msg_of_segment - (uint8_t*)start_msg_header + last_msg_of_segment->paddedSize + header_size; 
		last_offset = last_msg_of_segment->logical_offset;
		last_addr = (void*)last_msg_of_segment;
	}

#ifdef DEBUG
	struct MessageHeader *m = (struct MessageHeader*)messages;
	size_t len = messages_size;
	while(len>0){
		char* msg = (char*)((uint8_t*)m + header_size);
		len -= header_size;
		DLOG(INFO) << " total_order:" << m->total_order<< " logical_order:" <<
		m->logical_offset << " msg:" << msg;
		len -= m->paddedSize;
		m =  (struct MessageHeader*)m->next_message;
	}
#endif

	return true;
}


} // End of namespace Embarcadero
