#include "topic_manager.h"

#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <cstdint>
#include <algorithm>
#include <immintrin.h>
#include "folly/ConcurrentSkipList.h"

namespace Embarcadero{

#define NT_THRESHOLD 128

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

struct TInode* TopicManager::CreateNewTopicInternal(char topic[TOPIC_NAME_SIZE]){
	struct TInode* tinode = cxl_manager_.GetTInode(topic);
	TInode* replica_tinode = nullptr;
	{
	absl::WriterMutexLock lock(&mutex_);
	CHECK_LT(num_topics_, MAX_TOPIC_SIZE) << "Creating too many topics, increase MAX_TOPIC_SIZE";
	if(topics_.find(topic)!= topics_.end()){
		return nullptr;
	}
	static void* cxl_addr = cxl_manager_.GetCXLAddr();
	void* segment_metadata = cxl_manager_.GetNewSegment();
	void* batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();
	if(tinode->replicate_tinode){
		replica_tinode = cxl_manager_.GetReplicaTInode(topic);
		replica_tinode->offsets[broker_id_].ordered = -1;
		replica_tinode->offsets[broker_id_].written = -1;
		replica_tinode->offsets[broker_id_].log_offset = (size_t)((uint8_t*)segment_metadata + CACHELINE_SIZE - (uint8_t*)cxl_addr);
		replica_tinode->offsets[broker_id_].batch_headers_offset = (size_t)((uint8_t*)batch_headers_region - (uint8_t*)cxl_addr);
	}
	tinode->offsets[broker_id_].ordered = -1;
	tinode->offsets[broker_id_].written = -1;
	tinode->offsets[broker_id_].log_offset = (size_t)((uint8_t*)segment_metadata + CACHELINE_SIZE - (uint8_t*)cxl_addr);
	tinode->offsets[broker_id_].batch_headers_offset = (size_t)((uint8_t*)batch_headers_region - (uint8_t*)cxl_addr);

/*
#ifdef __INTEL__
    _mm_clflushopt(&tinode->offsets[broker_id_]);
#elif defined(__AMD__)
    _mm_clwb(&tinode->offsets[broker_id_]);
#else
		LOG(ERROR) << "Neither Intel nor AMD processor detected. If you see this and you either Intel or AMD, change cmake";
#endif
*/
	topics_[topic] = std::make_unique<Topic>([this](){return cxl_manager_.GetNewSegment();},
			tinode, replica_tinode, topic, broker_id_, tinode->order, tinode->seq_type, cxl_manager_.GetCXLAddr(), segment_metadata);
	}

	int replication_factor = tinode->replication_factor;
	if(replication_factor > 0){
		disk_manager_.Replicate(tinode, replica_tinode, replication_factor);
	}

	if(tinode->seq_type != KAFKA && tinode->order != 4)
		topics_[topic]->Combiner();

	if (broker_id_ != 0 && tinode->seq_type == SCALOG){
		cxl_manager_.RunSequencer(topic, tinode->order, tinode->seq_type);
	}

	return tinode;
}

struct TInode* TopicManager::CreateNewTopicInternal(char topic[TOPIC_NAME_SIZE], int order, int replication_factor, bool replicate_tinode, SequencerType seq_type){
	struct TInode* tinode = cxl_manager_.GetTInode(topic);
	struct TInode* replica_tinode = nullptr;
	bool no_collision = std::all_of(
    reinterpret_cast<const unsigned char*>(tinode->topic),
    reinterpret_cast<const unsigned char*>(tinode->topic) + TOPIC_NAME_SIZE,
    [](unsigned char c) { return c == 0; }
);
	if(!no_collision){
		LOG(ERROR) << "Jae Topic name collides: " << tinode->topic << " handle collision";
		exit(1);
	}
	{
	absl::WriterMutexLock lock(&mutex_);
	CHECK_LT(num_topics_, MAX_TOPIC_SIZE) << "Creating too many topics, increase MAX_TOPIC_SIZE";
	if(topics_.find(topic)!= topics_.end()){
		return nullptr;
	}
	static void* cxl_addr = cxl_manager_.GetCXLAddr();
	void* segment_metadata = cxl_manager_.GetNewSegment();
	void* batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();
	tinode->offsets[broker_id_].ordered = -1;
	tinode->offsets[broker_id_].written = -1;
	tinode->offsets[broker_id_].log_offset = (size_t)((uint8_t*)segment_metadata + CACHELINE_SIZE - (uint8_t*)cxl_addr);
	tinode->offsets[broker_id_].batch_headers_offset = (size_t)((uint8_t*)batch_headers_region - (uint8_t*)cxl_addr);
	tinode->order = order;
	tinode->replication_factor = replication_factor;
	tinode->replicate_tinode = replicate_tinode;
	tinode->seq_type = seq_type;
	memcpy(tinode->topic, topic, TOPIC_NAME_SIZE);

	if(replicate_tinode){
		char replica_topic[TOPIC_NAME_SIZE];
		memcpy(replica_topic, topic, TOPIC_NAME_SIZE);
		memcpy((uint8_t*)replica_topic + (TOPIC_NAME_SIZE-7), "replica", 7); 
		replica_tinode = cxl_manager_.GetReplicaTInode(topic);
		no_collision = std::all_of(
			reinterpret_cast<const unsigned char*>(replica_tinode->topic),
			reinterpret_cast<const unsigned char*>(replica_tinode->topic) + TOPIC_NAME_SIZE,
			[](unsigned char c) { return c == 0; }
		);
		if(!no_collision){
			LOG(ERROR) << "Jae replica Topic name collides: " << replica_tinode->topic << " handle collision";
			exit(1);
		}
		replica_tinode->offsets[broker_id_].ordered = -1;
		replica_tinode->offsets[broker_id_].written = -1;
		replica_tinode->offsets[broker_id_].log_offset = (size_t)((uint8_t*)segment_metadata + CACHELINE_SIZE - (uint8_t*)cxl_addr);
		replica_tinode->offsets[broker_id_].batch_headers_offset = (size_t)((uint8_t*)batch_headers_region - (uint8_t*)cxl_addr);
		replica_tinode->order = order;
		replica_tinode->replication_factor = replication_factor;
		replica_tinode->replicate_tinode = replicate_tinode;
		replica_tinode->seq_type = seq_type;
		memcpy(replica_tinode->topic, replica_topic, TOPIC_NAME_SIZE);
	}

/*
#ifdef __INTEL__
    _mm_clflushopt(&tinode->offsets[broker_id_]);
#elif defined(__AMD__)
    _mm_clwb(&tinode->offsets[broker_id_]);
#else
		LOG(ERROR) << "Neither Intel nor AMD processor detected. If you see this and you either Intel or AMD, change cmake";
#endif
*/
	topics_[topic] = std::make_unique<Topic>([this](){return cxl_manager_.GetNewSegment();},
			tinode, replica_tinode, topic, broker_id_, order, tinode->seq_type, cxl_manager_.GetCXLAddr(), segment_metadata);
	}

	if(replication_factor > 0){
		disk_manager_.Replicate(tinode, replica_tinode, replication_factor);
	}

	if(tinode->seq_type != KAFKA && tinode->order != 4)
		topics_[topic]->Combiner();
	return tinode;
}

bool TopicManager::CreateNewTopic(char topic[TOPIC_NAME_SIZE], int order, int replication_factor, bool replicate_tinode, SequencerType seq_type){
	if(CreateNewTopicInternal(topic, order, replication_factor, replicate_tinode, seq_type)){
		cxl_manager_.RunSequencer(topic, order, seq_type);
		return true;
	}else{
		LOG(ERROR)<< "Topic already exists!!!";
	}
	return false;
}

void TopicManager::DeleteTopic(char topic[TOPIC_NAME_SIZE]){
}

std::function<void(void*, size_t)> TopicManager::GetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset){
	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()){
		if(memcmp(topic, cxl_manager_.GetTInode(topic)->topic, TOPIC_NAME_SIZE) == 0){
			// The topic was created from another broker
			CreateNewTopicInternal(topic);
			topic_itr = topics_.find(topic);
			if(topic_itr == topics_.end()){
				LOG(ERROR) << "Topic Entry was not created Something is wrong";
				return nullptr;
			}
		}else{
			LOG(ERROR) << "[GetCXLBuffer] Topic:" << topic << " was not created before:" << cxl_manager_.GetTInode(topic)->topic
			<< " memcmp:" << memcmp(topic, cxl_manager_.GetTInode(topic)->topic, TOPIC_NAME_SIZE);
			return nullptr;
		}
	}
	return topic_itr->second->GetCXLBuffer(batch_header, topic, log, segment_header, logical_offset);
}

bool TopicManager::GetMessageAddr(const char* topic, size_t &last_offset,
		void* &last_addr, void* &messages, size_t &messages_size){
	absl::ReaderMutexLock lock(&mutex_);
	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()){
		//LOG(ERROR) << "Topic not found";
		// Not throwing error as subscribe can be called before the topic is created
		return false;
	}
	return topic_itr->second->GetMessageAddr(last_offset, last_addr, messages, messages_size);
}

Topic::Topic(GetNewSegmentCallback get_new_segment, void* TInode_addr, TInode* replica_tinode, const char* topic_name,
		int broker_id, int order, SequencerType seq_type, void* cxl_addr, void* segment_metadata):
		get_new_segment_callback_(get_new_segment),
		tinode_(static_cast<struct TInode*>(TInode_addr)),
		replica_tinode_(replica_tinode),
		topic_name_(topic_name),
		broker_id_(broker_id),
		order_(order),
		seq_type_(seq_type),
		cxl_addr_(cxl_addr),
		logical_offset_(0),
		written_logical_offset_((size_t)-1),
		current_segment_(segment_metadata){
	log_addr_.store((unsigned long long int)((uint8_t*)cxl_addr_ + tinode_->offsets[broker_id_].log_offset));
	batch_headers_ = (unsigned long long int)((uint8_t*)cxl_addr_ + tinode_->offsets[broker_id_].batch_headers_offset);
	first_message_addr_ = (uint8_t*)cxl_addr_ + tinode_->offsets[broker_id_].log_offset;
	first_batch_headers_addr_ = (uint8_t*)cxl_addr_ + tinode_->offsets[broker_id_].batch_headers_offset;
	ordered_offset_addr_ = nullptr;
	ordered_offset_ = 0;
	if(seq_type == KAFKA){
		GetCXLBufferFunc = &Topic::KafkaGetCXLBuffer;
	}else if(seq_type == CORFU){
		GetCXLBufferFunc = &Topic::CorfuGetCXLBuffer;
	}else{
		if(order_ == 3){
			GetCXLBufferFunc = &Topic::Order3GetCXLBuffer;
		}else if (order_ == 4){
			GetCXLBufferFunc = &Topic::Order4GetCXLBuffer;
		}else{
			GetCXLBufferFunc = &Topic::EmbarcaderoGetCXLBuffer;
		}
	}
}

inline void Topic::UpdateTInodeWritten(size_t written, size_t written_addr){
	if(tinode_->replicate_tinode){
		replica_tinode_->offsets[broker_id_].written = written;
		replica_tinode_->offsets[broker_id_].written_addr = written_addr;
	}
	tinode_->offsets[broker_id_].written = written;
	tinode_->offsets[broker_id_].written_addr = written_addr;
}

void Topic::CombinerThread(){
	void* segment_header = (uint8_t*)first_message_addr_ - CACHELINE_SIZE;
	MessageHeader *header = (MessageHeader*)first_message_addr_;
	while(!stop_threads_){
		while(header->complete == 0){
			if(stop_threads_){
				return;
			}
			std::this_thread::yield();
		}
#ifdef MULTISEGMENT
		if(header->next_msg_diff!= 0){ // Moved to new segment
			header = (int8_t*)header + header->next_msg_diff;
			segment_header = (uint8_t*)header - CACHELINE_SIZE;
			continue;
		}
#else
	// This check is only true besides CORFU which does not change log_addr_
	 //CHECK_LT((unsigned long long int)header, log_addr_) << "header calculated wrong";
#endif
		header->segment_header = segment_header;
		header->logical_offset = logical_offset_;
		header->next_msg_diff = header->paddedSize;
/*
#ifdef __INTEL__
    _mm_clflushopt(header);
#elif defined(__AMD__)
    _mm_clwb(header);
#else
		LOG(ERROR) << "Neither Intel nor AMD processor detected. If you see this and you either Intel or AMD, change cmake";
    // Fallback or error handling
#endif
*/
		std::atomic_thread_fence(std::memory_order_release);
		UpdateTInodeWritten(logical_offset_, (unsigned long long int)((uint8_t*)header - (uint8_t*)cxl_addr_));
		(*(unsigned long long int*)segment_header) =
			(unsigned long long int)((uint8_t*)header - (uint8_t*)segment_header);
		written_logical_offset_ = logical_offset_;
		written_physical_addr_ = (void*)header;
		header = (MessageHeader*)((uint8_t*)header + header->next_msg_diff);
		logical_offset_++;
	}
}

// Give logical order, not total order to messages. 
// Order=0 can export these ordeerd messages. 1 and 2 should wait for sequencer
void Topic::Combiner(){
	combiningThreads_.emplace_back(&Topic::CombinerThread, this);
}

std::function<void(void*, size_t)> Topic::KafkaGetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset){
	size_t start_logical_offset;
	{
	absl::MutexLock lock(&mutex_);
	log = (void*)(log_addr_.fetch_add(batch_header.total_size));
	logical_offset = logical_offset_;
	segment_header = current_segment_;
	start_logical_offset = logical_offset_;
	logical_offset_+= batch_header.num_msg;
	//TODO(Jae) This does not work with dynamic message size
	(void*)(log_addr_.load() - ((MessageHeader*)log)->paddedSize);
	if((unsigned long long int)current_segment_ + SEGMENT_SIZE <= log_addr_){
		LOG(ERROR)<< "!!!!!!!!! Increase the Segment Size:" << SEGMENT_SIZE;
		//TODO(Jae) Finish below segment boundary crossing code
	}
	}
	return [this, start_logical_offset](void* log, size_t logical_offset)
	{
		absl::MutexLock lock(&written_mutex_);
		if(kafka_logical_offset_.load() != start_logical_offset){
			written_messages_range_[start_logical_offset] = logical_offset;
		}else{
			size_t start = start_logical_offset;
			bool has_next_messages_written = false;
			do{
				has_next_messages_written = false;
				written_logical_offset_ = logical_offset;
				written_physical_addr_ = (void*)log;
				//tinode_->offsets[broker_id_].written = logical_offset;
				((MessageHeader*)log)->logical_offset = (size_t)-1;
				//tinode_->offsets[broker_id_].written_addr = (unsigned long long int)((uint8_t*)log - (uint8_t*)cxl_addr_);
				UpdateTInodeWritten(logical_offset, (unsigned long long int)((uint8_t*)log - (uint8_t*)cxl_addr_));
				(*(unsigned long long int*)current_segment_) =
					(unsigned long long int)((uint8_t*)log - (uint8_t*)current_segment_);
				kafka_logical_offset_.store(logical_offset+1);
				if(written_messages_range_.contains(logical_offset+1)){
					start = logical_offset+1;
					logical_offset = written_messages_range_[start];
					written_messages_range_.erase(start);
					has_next_messages_written = true;
				}
			}while(has_next_messages_written);
		}
	};
}

std::function<void(void*, size_t)> Topic::CorfuGetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset){
	unsigned long long int segment_metadata = (unsigned long long int)current_segment_;
	size_t msgSize = batch_header.total_size;
	log = (void*)((uint8_t*)log_addr_.load() +  batch_header.log_idx);
	if(segment_metadata + SEGMENT_SIZE <= (unsigned long long int)log + msgSize){
		LOG(ERROR)<< "!!!!!!!!! Increase the Segment Size:" << SEGMENT_SIZE;
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
	return nullptr;
}

std::function<void(void*, size_t)> Topic::Order3GetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset){
	absl::MutexLock lock(&mutex_);
	if(skipped_batch_.contains(batch_header.client_id)){
		auto it = skipped_batch_[batch_header.client_id].find(batch_header.batch_seq);
		if(it != skipped_batch_[batch_header.client_id].end()){
			log = it->second;
			skipped_batch_[batch_header.client_id].erase(it);
			return nullptr;
		}
	}
	auto it = order3_client_batch_.find(batch_header.client_id);
	if (it == order3_client_batch_.end()) {
		order3_client_batch_.emplace(batch_header.client_id, broker_id_);
	}
	while(order3_client_batch_[batch_header.client_id] < batch_header.batch_seq){
		skipped_batch_[batch_header.client_id].emplace(order3_client_batch_[batch_header.client_id],(void*)log_addr_.load());
		log_addr_ += batch_header.total_size; // This assumes the batch sizes are identical. Change this later
		order3_client_batch_[batch_header.client_id] += batch_header.num_brokers;
	}
	log = (void*)(log_addr_.load());
	log_addr_ += batch_header.total_size;
	order3_client_batch_[batch_header.client_id] += batch_header.num_brokers;
	return nullptr;
}

std::function<void(void*, size_t)> Topic::Order4GetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset){
	unsigned long long int segment_metadata = (unsigned long long int)current_segment_;
	size_t msgSize = batch_header.total_size;
	void *batch_headers_log;
	{
	absl::MutexLock lock(&mutex_);
	log = (void*)(log_addr_.fetch_add(msgSize));
	batch_headers_log = (void*)(batch_headers_);
	batch_headers_ += sizeof(BatchHeader);
	}
	if(segment_metadata + SEGMENT_SIZE <= (unsigned long long int)log + msgSize){
		LOG(ERROR)<< "!!!!!!!!! Increase the Segment Size:" << SEGMENT_SIZE;
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
	batch_header.log_idx = (size_t)((uint8_t*)log - (uint8_t*)cxl_addr_);
	VLOG(3) << "Receive batch:" << batch_header.batch_seq << " num_msg:" << batch_header.num_msg;
	memcpy(batch_headers_log, &batch_header, sizeof(BatchHeader));
	return nullptr;
}

std::function<void(void*, size_t)> Topic::EmbarcaderoGetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset){
	unsigned long long int segment_metadata = (unsigned long long int)current_segment_;
	size_t msgSize = batch_header.total_size;
	log = (void*)(log_addr_.fetch_add(msgSize));
	if(segment_metadata + SEGMENT_SIZE <= (unsigned long long int)log + msgSize){
		LOG(ERROR)<< "!!!!!!!!! Increase the Segment Size:" << SEGMENT_SIZE;
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
	return nullptr;
}

// Current implementation depends on the subscriber knowing the physical address of last fetched message
// This is only true if the messages were exported from CXL. If we implement disk cache optimization, 
// we need to fix it. Probably need to have some sort of indexing or call this method to get indexes
// even if at cache hit (without sending the messages)
//
// arguments: 
// if the messages to export go over the segment boundary (not-contiguous), 
// we should call this functiona again. Try calling it if it returns true
bool Topic::GetMessageAddr(size_t &last_offset,
		void* &last_addr, void* &messages, size_t &messages_size){
	size_t combined_offset = written_logical_offset_;
	void* combined_addr = written_physical_addr_;

	if(order_ > 0){
		combined_offset = tinode_->offsets[broker_id_].ordered;
		combined_addr = (uint8_t*)cxl_addr_ + tinode_->offsets[broker_id_].ordered_offset;
	}

	if(combined_offset == (size_t)-1 || ((last_addr != nullptr) && (combined_offset <= last_offset))){
		return false;
	}

	struct MessageHeader *start_msg_header = (struct MessageHeader*)last_addr;
	if(last_addr != nullptr){
		while((struct MessageHeader*)start_msg_header->next_msg_diff == 0){
			//LOG(INFO) << "[GetMessageAddr] waiting for the message to be combined ";
			std::this_thread::yield();
		}
		start_msg_header = (struct MessageHeader*)((uint8_t*)start_msg_header + start_msg_header->next_msg_diff);
	}else{
		//TODO(Jae) this is only true in a single segment setup
		if(combined_addr <= last_addr){
			LOG(ERROR) << "[GetMessageAddr] Wrong!!";
			return false;
		}
		start_msg_header = (struct MessageHeader*)first_message_addr_;
	}

	if(start_msg_header->paddedSize == 0){
		return false;
	}

	messages = (void*)start_msg_header;
#ifdef MULTISEGMENT
	//TODO(Jae) use relative addr here for multi-node
	unsigned long long int* last_msg_off = (unsigned long long int*)start_msg_header->segment_header;
	struct MessageHeader *last_msg_of_segment = (MessageHeader*)((uint8_t*)last_msg_off + *last_msg_off);

	if(combined_addr < last_msg_of_segment){ // last msg is not ordered yet
		messages_size = (uint8_t*)combined_addr - (uint8_t*)start_msg_header + ((MessageHeader*)combined_addr)->paddedSize; 
		last_offset = ((MessageHeader*)combined_addr)->logical_offset;
		last_addr = (void*)combined_addr;
	}else{
		messages_size = (uint8_t*)last_msg_of_segment - (uint8_t*)start_msg_header + last_msg_of_segment->paddedSize; 
		last_offset = last_msg_of_segment->logical_offset;
		last_addr = (void*)last_msg_of_segment;
	}
#else
	messages_size = (uint8_t*)combined_addr - (uint8_t*)start_msg_header + ((MessageHeader*)combined_addr)->paddedSize; 
	last_offset = ((MessageHeader*)combined_addr)->logical_offset;
	last_addr = (void*)combined_addr;
#endif
	return true;
}

} // End of namespace Embarcadero
