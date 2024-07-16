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

enum CXLType {Emul, Real};
enum SequencerType {Embarcadero, Scalog, Corfu};

/* CXL memory layout
 *
 * CXL is composed of three components; TINode, Bitmap, Segments
 * TINode region: First sizeof(TINode) * MAX_TOPIC
 * + Padding to make each region be aligned in cacheline
 * Bitmap region: Cacheline_size * NUM_MAX_BROKERS
 * Segment region: Rest. It is allocated to each brokers equally according to broker_id
 * 		Segment: 8Byte of segment metadata to store address of last ordered_offset from the segment, messages
 * 			Message: Header + paylod
 */

struct alignas(32) offset_entry {
	volatile int ordered;
	volatile int written;
	//Since each broker will have different virtual adddress on the CXL memory, access it via CXL_addr_ + off
	volatile size_t ordered_offset; //relative offset to last ordered message header
	volatile size_t log_offset;
};

struct alignas(64) TInode{
	char topic[TOPIC_NAME_SIZE];
	volatile uint8_t order;
	volatile offset_entry offsets[NUM_MAX_BROKERS];
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
	volatile size_t paddedSize; // This include message+padding+header size
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
		void CreateNewTopic(char topic[31], int order, SequencerType seqType = Embarcadero);
		void* GetCXLAddr(){
			return cxl_addr_;
		}
//#define InternalTest 1

#ifdef InternalTest
		std::atomic<bool> startInternalTest_{false};
		std::atomic<size_t> reqCount_{0};;
		std::vector<std::thread> testThreads_;
		std::chrono::high_resolution_clock::time_point start;
		void DummyReq();
		void WriteDummyReq();
		void StartInternalTest();
#endif

		void Wait1(){
			while(DEBUG_1_passed_==false){
				std::this_thread::yield();
			}
			return;
		}
		void Wait2(){
			while(DEBUG_2_passed_ == false){
				std::this_thread::yield();
			}
			return;
		}

		private:
		folly::MPMCQueue<std::optional<struct PublishRequest>> requestQueue_;
		int broker_id_;
		int num_io_threads_;
		std::vector<std::thread> threads_;
		std::vector<std::thread> sequencerThreads_;

		TopicManager *topic_manager_;
		NetworkManager *network_manager_;

		CXLType cxl_type_;
		void* cxl_addr_;
		void* bitmap_;
		void* segments_;
		void* current_log_addr_;
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};
		bool DEBUG_1_passed_ = false;
		bool DEBUG_2_passed_ = false;

		void CXL_io_thread();
		void Sequencer1(char* topic);
		void Sequencer2(char* topic);
};

} // End of namespace Embarcadero
#endif
