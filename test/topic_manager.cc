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

void TopicManager::PublishToCXL(char topic[32], void* message, size_t size){
	auto topic_itr = topics_.find(topic);
	if (topic_itr == topics_.end()){
		perror("Topic not found");
	}
	topic_itr->second.PublishToCXL(message, size);
}

Topic::Topic(CXLManager &cxl_manager, char topic_name[32], int broker_id):
cxl_manager_(cxl_manager),
topic_name_(topic_name),
broker_id_(broker_id){
	tinode_ = (struct TInode*)cxl_manager_.Get_tinode(topic_name_, broker_id_);
	logical_offset_ = 0;
	written_logical_offset_ = 0;
	log_addr_ = tinode_->per_broker_log[broker_id_];

	//TODO(Jae)
	// have cache on disk as well
}

void Topic::PublishToCXL(void* message, size_t size){
	//TODO(Jae) skip list impl
	void* log;
	int logical_offset;
	{
		//absl::MutexLock lock(&mu_);
		std::unique_lock<std::mutex> lock(mu_);
		logical_offset = logical_offset_;
		logical_offset_++;
		remaining_size_ -= size;
		if(remaining_size_ < 0){
			log = log_addr_;
			log_addr_ = (uint8_t*)log_addr_ + size;
		}else{
			log = cxl_manager_.GetNewSegment();
			log_addr_ = (uint8_t*)log + size;
			remaining_size_ = SEGMENT_SIZE;
		}
		writing_offsets_.insert(logical_offset);
	}

	nt_memcpy(log, message, size);

	{
		//absl::MutexLock lock(&mu_);
		std::unique_lock<std::mutex> lock(mu_);
		if (*(writing_offsets_.begin()++) == logical_offset){
			written_logical_offset_ = logical_offset;
			while(!not_contigous_.empty() && not_contigous_.top() == written_logical_offset_ + 1){
				not_contigous_.pop();
				written_logical_offset_++;
			}
			//TODO(Jae) write to cxl written offset
		}else{
			not_contigous_.push(logical_offset);
		}
		writing_offsets_.erase(logical_offset);
	}
}

} // End of namespace Embarcadero
