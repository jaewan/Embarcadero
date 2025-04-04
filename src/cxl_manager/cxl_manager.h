#ifndef INCLUDE_CXL_MANGER_H_
#define INCLUDE_CXL_MANGER_H_

#include <thread>
#include <iostream>
#include <optional>
#include "folly/MPMCQueue.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/btree_map.h"
#include "common/config.h"
#include "../embarlet/topic_manager.h"
#include "../network_manager/network_manager.h"
#include <heartbeat.grpc.pb.h>
#include "../embarlet/heartbeat.h"
#include <grpcpp/grpcpp.h>
#include <scalog_sequencer.grpc.pb.h>

namespace Embarcadero{

class TopicManager;
class NetworkManager;
class HeartBeatManager;
class ScalogLocalSequencer;

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
 * BatchHeaders region: NUM_MAX_BROKERS * BATCHHEADERS_SIZE * MAX_TOPIC
 * Segment region: Rest. It is allocated to each brokers equally according to broker_id
 * 		Segment: 8Byte of segment metadata to store address of last ordered_offset from the segment, messages
 * 			Message: Header + paylod
 */

struct alignas(64) offset_entry {
	struct {
		volatile size_t log_offset;
		volatile size_t batch_headers_offset;
		volatile size_t written;
		volatile unsigned long long int written_addr;
		volatile int replication_done[NUM_MAX_BROKERS];
	}__attribute__((aligned(64)));
	struct {
		volatile int ordered;
		volatile size_t ordered_offset; //relative offset to last ordered message header
	}__attribute__((aligned(64)));
};

struct alignas(64) TInode{
	struct {
		char topic[TOPIC_NAME_SIZE];
		volatile bool replicate_tinode = false;
		volatile int order;
		volatile int replication_factor;
		SequencerType seq_type;
	}__attribute__((aligned(64)));

	volatile offset_entry offsets[NUM_MAX_BROKERS];
};

struct alignas(64) BatchHeader{
	size_t client_id;
	// Fill from buffer
	size_t next_reader_head; // used in publish buffer write, logical_offset in Sequencer4
	size_t batch_seq;
	size_t total_size;
	size_t num_msg;
	// Corfu variables
	uint32_t broker_id; 	// Sequencer4: batch_ordered
	uint32_t num_brokers;	// Sequencer4: all prev batches are ordered
	size_t total_order;	// Sequencer4: relative offset to previous batch header
	size_t log_idx;	// Sequencer4: relative log offset to the payload of the batch and elative offset to last message
};


// Orders are very important to avoid race conditions. 
// If you change orders of elements, change how sequencers and combiner check written messages
struct alignas(64) MessageHeader{
	volatile size_t paddedSize; // This include message+padding+header size
	void* segment_header;
	size_t logical_offset;
	volatile unsigned long long int next_msg_diff; // Relative to message_header, not cxl_addr_
	volatile size_t total_order;
	size_t client_order;
	uint32_t client_id;
	volatile uint32_t complete;
	size_t size;
};

class CXLManager{
	public:
		CXLManager(int broker_id, CXL_Type cxl_type, std::string head_ip);
		~CXLManager();
		void SetTopicManager(TopicManager *topic_manager){topic_manager_ = topic_manager;}
		void SetNetworkManager(NetworkManager* network_manager){network_manager_ = network_manager;}
		void* GetNewSegment();
		void* GetNewBatchHeaderLog();
		TInode* GetTInode(const char* topic);
		TInode* GetReplicaTInode(const char* topic);
		bool GetMessageAddr(const char* topic, size_t &last_offset,
				void* &last_addr, void* &messages, size_t &messages_size);
		void RunSequencer(const char topic[TOPIC_NAME_SIZE], int order, SequencerType sequencerType);
		void* GetCXLAddr(){return cxl_addr_;}
		void RegisterGetRegisteredBrokersCallback(GetRegisteredBrokersCallback callback){
			get_registered_brokers_callback_ = callback;
		}
		std::function<void(void*, size_t)> GetCXLBuffer(BatchHeader &batch_header, const char topic[TOPIC_NAME_SIZE],
				void* &log, void* &segment_header, size_t &logical_offset, SequencerType &seq_type);
		void GetRegisteredBrokers(absl::btree_set<int> &registered_brokers,
														struct MessageHeader** msg_to_order, struct TInode *tinode);

		/// Initializes the scalog sequencer service and starts the grpc server
		void StartScalogLocalSequencer(std::string topic_str);

		/// Receives the global cut from the head node
		/// This function is called in the callback of the send local cut grpc call
		void ScalogSequencer(const char* topic, absl::flat_hash_map<int, absl::btree_map<int, int>> &global_cut);

		void SetEpochToOrder(int epoch){
			epoch_to_order_ = epoch;
		}
	private:
		int broker_id_;
		std::string head_ip_;
		size_t cxl_size_;
		std::vector<std::thread> sequencerThreads_;

		TopicManager *topic_manager_;
		NetworkManager *network_manager_;

		void* cxl_addr_;
		void* bitmap_;
		void* batchHeaders_;
		void* segments_;
		void* current_log_addr_;
		volatile bool stop_threads_ = false;
		GetRegisteredBrokersCallback get_registered_brokers_callback_;
		std::vector<std::thread> sequencer4_threads_;

		// Scalog
		std::string scalog_global_sequencer_ip_ = "192.168.60.172";
		std::unique_ptr<ScalogLocalSequencer> scalog_local_sequencer_;
		// std::atomic<int> scalog_local_sequencer_port_offset_{0};

		/// Epoch to order
		int epoch_to_order_ = 0;

		void CXLIOThread(int tid);
		inline void UpdateTinodeOrder(char *topic, TInode* tinode, int broker, size_t msg_logical_off,
																		size_t msg_to_order);
		void Sequencer1(std::array<char, TOPIC_NAME_SIZE> topic);
		void Sequencer2(std::array<char, TOPIC_NAME_SIZE> topic);
		void Sequencer3(std::array<char, TOPIC_NAME_SIZE> topic);
		void Sequencer4(std::array<char, TOPIC_NAME_SIZE> topic);
		void Sequencer4Worker(std::array<char, TOPIC_NAME_SIZE> topic, int broker, MessageHeader* msg_to_order, 
					absl::Mutex* mutex, size_t &seq, absl::flat_hash_map<size_t, size_t> &batch_seq);
};

class ScalogLocalSequencer {
	public:
		ScalogLocalSequencer(CXLManager* cxl_manager, int broker_id, void* cxl_addr, std::string scalog_seq_address);

		/// Called when first starting the scalog local sequencer. It manages
		/// the timing between each local cut
		void LocalSequencer(std::string topic_str);

		/// Sends a local cut to the global sequencer
		void SendLocalCut(int local_cut, const char* topic);

		/// Sends a register request to the global sequencer
		void Register();

		/// Sends a request to global sequencer to terminate itself
		void TerminateGlobalSequencer();
	private:
		CXLManager* cxl_manager_;
		int broker_id_;
		void* cxl_addr_;
		std::unique_ptr<ScalogSequencer::Stub> stub_;

		/// Time between each local cut
		std::chrono::microseconds local_cut_interval_ = std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL);

		/// The key is the current epoch and it contains another map of broker_id to local cut
		absl::flat_hash_map<int, absl::btree_map<int, int>> global_cut_;

		/// Local epoch
		int local_epoch_ = 0;
};

} // End of namespace Embarcadero
#endif
