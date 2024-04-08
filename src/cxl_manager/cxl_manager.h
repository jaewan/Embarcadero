#ifndef INCLUDE_CXL_MANGER_H_
#define INCLUDE_CXL_MANGER_H_


#include <thread>
#include <iostream>
#include <optional>
#include "folly/MPMCQueue.h"
#include "common/config.h"
#include "../embarlet/topic_manager.h"
#include "../embarlet/req_queue.h"
#include "../embarlet/ack_queue.h"

namespace Embarcadero{

class TopicManager;

enum CXL_Type {Emul, Real};

/* CXL memory layout
 *
 * CXL is composed of three components; TINode, Bitmap, Segments
 * TINode region: First sizeof(TINode) * MAX_TOPIC
 * + Padding to make each region be aligned in cacheline
 * Bitmap region: Cacheline_size * NUM_BROKERS
 * Segment region: Rest. It is allocated to each brokers equally according to broker_id
 * 		Segment: 8Byte of segment metadata to store address of last ordered_offset from the segment, messages
 * 			Message: Header + paylod
 */

// Align and pad to 64B cacheline
struct alignas(64) offset_entry {
	int ordered;
	int written;
	void* log_addr;
	char _padding[64 - (sizeof(size_t) * 2 + sizeof(void*))]; 
};

struct TInode{
	char topic[32];
	volatile offset_entry offsets[NUM_BROKERS];
};

struct NonCriticalMessageHeader{
	int client_id;
	size_t client_order;
	size_t logical_offset;
	volatile size_t total_order;
	volatile size_t size;
	void* segment_header;
};

struct MessageHeader{
	int client_id;
	size_t client_order;
	size_t logical_offset;
	volatile size_t total_order;
	volatile size_t size;
	void* segment_header;
	void* next_message;
};

class CXLManager{
	public:
		CXLManager(std::shared_ptr<AckQueue> ack_queue, std::shared_ptr<ReqQueue> req_queue, int broker_id, CXL_Type cxl_type, int num_io_threads=NUM_CXL_IO_THREADS);
		~CXLManager();
		void SetTopicManager(TopicManager *topic_manager){
			topic_manager_ = topic_manager;
		}
		void* GetNewSegment();
		void* GetTInode(const char* topic);
		bool GetMessageAddr(const char* topic, size_t &last_offset,
												void* &last_addr, void* messages, size_t &messages_size);
		void Sequencer();

	private:
		std::shared_ptr<ReqQueue> reqQueue_;
		std::shared_ptr<AckQueue> ackQueue_;
		int broker_id_;
		int num_io_threads_;
		std::vector<std::thread> threads_;

		TopicManager *topic_manager_;

		CXL_Type cxl_type_;
		int cxl_fd_;
		void* cxl_addr_;
		void* bitmap_;
		void* segments_;
		void* current_log_addr_;
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};

		void CXL_io_thread();
};

} // End of namespace Embarcadero
#endif
