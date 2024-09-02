#ifndef INCLUDE_CXL_MANGER_H_
#define INCLUDE_CXL_MANGER_H_

#include <thread>
#include <iostream>
#include <optional>
#include "folly/MPMCQueue.h"
#include "common/config.h"
#include "../embarlet/topic_manager.h"
#include "../network_manager/network_manager.h"
#include <heartbeat.grpc.pb.h>
#include "absl/container/flat_hash_map.h"
#include "absl/container/btree_map.h"

namespace Embarcadero{

class TopicManager;
class NetworkManager;

enum CXL_Type {Emul, Real};
using heartbeat_system::SequencerType;
using heartbeat_system::SequencerType::EMBARCADERO;
using heartbeat_system::SequencerType::KAFKA;
using heartbeat_system::SequencerType::SCALOG;
using heartbeat_system::SequencerType::CORFU;

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
	SequencerType seq_type;
	volatile offset_entry offsets[NUM_MAX_BROKERS];
};

struct NonCriticalMessageHeader{
	int client_id;
	size_t client_order;
	size_t size;
	size_t paddedSize;
	void* segment_header;
	char _padding[64 - (sizeof(int) + sizeof(size_t) * 3 + sizeof(void*))]; 
};


// Orders are very important to avoid race conditions. 
// If you change orders of elements, change how sequencers and combiner check written messages
struct alignas(64) MessageHeader{
	void* segment_header;
	size_t logical_offset;
	unsigned long long int next_msg_diff; // Relative to message_header, not cxl_addr_
	volatile size_t total_order;
	int client_id;
	size_t client_order;
	volatile size_t size;
	volatile size_t paddedSize; // This include message+padding+header size
};

class CXLManager{
	public:
		CXLManager(size_t queueCapacity, int broker_id, CXL_Type cxl_type, int num_io_threads);
		~CXLManager();
		void SetTopicManager(TopicManager *topic_manager){topic_manager_ = topic_manager;}
		void SetNetworkManager(NetworkManager* network_manager){network_manager_ = network_manager;}
		void EnqueueRequest(struct PublishRequest req);
		void* GetNewSegment();
		void* GetTInode(const char* topic);
		bool GetMessageAddr(const char* topic, size_t &last_offset,
				void* &last_addr, void* &messages, size_t &messages_size);
		void RunSequencer(char topic[TOPIC_NAME_SIZE], int order, SequencerType sequencerType);
		void* GetCXLAddr(){return cxl_addr_;}
		void RegisterGetRegisteredBrokersCallback(GetRegisteredBrokersCallback callback){
			get_registered_brokers_callback_ = callback;
		}

	private:
		std::vector<folly::MPMCQueue<std::optional<struct PublishRequest>>> requestQueues_;
		int broker_id_;
		int num_io_threads_;
    size_t cxl_size_;
    std::vector<std::thread> threads_;
		std::vector<std::thread> sequencerThreads_;

		TopicManager *topic_manager_;
		NetworkManager *network_manager_;

		void* cxl_addr_;
		void* bitmap_;
		void* segments_;
		void* current_log_addr_;
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};
		GetRegisteredBrokersCallback get_registered_brokers_callback_;

		void GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
														struct MessageHeader** msg_to_order, struct TInode *tinode);
		void CXLIOThread(int tid);
		void Sequencer1(char* topic);
		void Sequencer2(char* topic);
};

} // End of namespace Embarcadero
#endif
