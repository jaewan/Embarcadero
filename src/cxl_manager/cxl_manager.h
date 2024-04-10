#ifndef INCLUDE_CXL_MANGER_H_
#define INCLUDE_CXL_MANGER_H_


#include <thread>
#include <iostream>
#include <optional>
#include "folly/MPMCQueue.h"
#include "common/config.h"
#include "../embarlet/topic_manager.h"
#include "../network_manager/network_manager.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/btree_map.h"

namespace Embarcadero{

class TopicManager;
class NetworkManager;

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
	size_t size;
	size_t total_order;
	size_t paddedSize;
	void* segment_header;
	char _padding[64 - (sizeof(int) + sizeof(size_t) * 4 + sizeof(void*))]; 
};

struct alignas(64) MessageHeader{
	int client_id;
	size_t client_order;
	volatile size_t size;
	volatile size_t total_order;
	volatile size_t paddedSize;
	void* segment_header;
	size_t logical_offset;
	void* next_message;
};

class CXLManager{
	public:
		CXLManager(size_t queueCapacity, int broker_id, int num_io_threads=NUM_CXL_IO_THREADS);
		~CXLManager();
		void SetTopicManager(TopicManager *topic_manager){
			topic_manager_ = topic_manager;
		}
		void SetNetworkManager(NetworkManager* network_manager){
			network_manager_ = network_manager;
		}
		void EnqueueRequest(struct PublishRequest req);
		void* GetNewSegment();
		void* GetTInode(const char* topic);
		bool GetMessageAddr(const char* topic, size_t &last_offset,
												void* &last_addr, void* messages, size_t &messages_size);
		void Sequencer(const char* topic);
#define InternalTest 1

#ifdef InternalTest
		std::atomic<bool> startInternalTest_{false};
		std::atomic<size_t> reqCount_{0};;
		std::vector<std::thread> testThreads_;
		std::chrono::high_resolution_clock::time_point start;
		void DummyReq();
		void WriteDummyReq(){
			PublishRequest req;
			memset(req.topic, 0, 32);
			req.topic[0] = '0';
			req.counter = (std::atomic<int>*)malloc(sizeof(std::atomic<int>));
			req.counter->store(1);
			req.payload_address = malloc(1024);
			req.size = 1024;
			requestQueue_.blockingWrite(req);
		}
		void StartInternalTest();
#endif

		private:
		folly::MPMCQueue<std::optional<struct PublishRequest>> requestQueue_;
		int broker_id_;
		int num_io_threads_;
		std::vector<std::thread> threads_;

		TopicManager *topic_manager_;
		NetworkManager *network_manager_;

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
