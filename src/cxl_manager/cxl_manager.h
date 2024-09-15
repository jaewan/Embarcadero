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
class ScalogSequencerService;

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

struct alignas(64) BatchHeader{
	size_t total_size;
	size_t num_msg;
	size_t batch_seq;
};


// Orders are very important to avoid race conditions. 
// If you change orders of elements, change how sequencers and combiner check written messages
struct alignas(64) MessageHeader{
	void* segment_header;
	size_t logical_offset;
	unsigned long long int next_msg_diff; // Relative to message_header, not cxl_addr_
	volatile size_t total_order;
	uint32_t client_id;
	uint32_t client_order;
	size_t size;
	volatile size_t paddedSize; // This include message+padding+header size
	uint32_t complete;
};

class CXLManager{
	public:
		CXLManager(int broker_id, CXL_Type cxl_type, std::string head_ip);
		~CXLManager();
		void SetTopicManager(TopicManager *topic_manager){topic_manager_ = topic_manager;}
		void SetNetworkManager(NetworkManager* network_manager){network_manager_ = network_manager;}
		void* GetNewSegment();
		void* GetTInode(const char* topic);
		bool GetMessageAddr(const char* topic, size_t &last_offset,
				void* &last_addr, void* &messages, size_t &messages_size);
		void RunSequencer(char topic[TOPIC_NAME_SIZE], int order, SequencerType sequencerType);
		void* GetCXLAddr(){return cxl_addr_;}
		void RegisterGetRegisteredBrokersCallback(GetRegisteredBrokersCallback callback){
			get_registered_brokers_callback_ = callback;
		}
		std::function<void(void*, size_t)> GetCXLBuffer(PublishRequest &req, void* &log, void* &segment_header, size_t &logical_offset);
		void GetRegisteredBrokers(absl::btree_set<int> &registered_brokers,
														struct MessageHeader** msg_to_order, struct TInode *tinode);

		/// Initializes the scalog sequencer service and starts the grpc server
		void StartScalogLocalSequencer(std::string topic_str);

		/// Receives the global cut from the head node
		/// This function is called in the callback of the send local cut grpc call
		void ScalogSequencer(int epoch, const char* topic, absl::Mutex &global_cut_mu, 
		absl::flat_hash_map<int, absl::btree_map<int, int>> &global_cut);
	private:
		int broker_id_;
		size_t cxl_size_;
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


		// Scalog
		std::string head_ip_;
		std::unique_ptr<ScalogSequencerService> scalog_sequencer_service_;
		std::unique_ptr<grpc::Server> scalog_server_;
		std::atomic<int> scalog_sequencer_service_port_offset_{0};

		void CXLIOThread(int tid);
		void Sequencer1(char* topic);
		void Sequencer2(char* topic);
		void Sequencer3(char* topic);
};

class ScalogSequencerService : public ScalogSequencer::Service {
	public:
		ScalogSequencerService(CXLManager* cxl_manager, int broker_id, void* cxl_addr, std::string scalog_seq_address);

    	/// Receives a local cut from a local sequencer
			/// @param request Request containing the local cut and the epoch
			/// @param response Empty for now
		virtual grpc::Status HandleSendLocalCut(grpc::ServerContext* context, const SendLocalCutRequest* request, SendLocalCutResponse* response);

		/// Called when first starting the scalog local sequencer. It manages
		/// the timing between each local cut
		virtual void LocalSequencer(std::string topic_str);

		/// Sends a local cut to the head node	
		virtual void SendLocalCut(int local_cut, const char* topic);

		/// Keep track of the global cut and if all the local cuts have been received
		virtual void ReceiveLocalCut(int epoch, const char* topic, int broker_id);

	private:
		CXLManager* cxl_manager_;
		int broker_id_;
		void* cxl_addr_;
		std::unique_ptr<ScalogSequencer::Stub> stub_;

		/// Used in ReceiveLocalCut() so we receive local cuts one at a time
		std::mutex mutex_;

		/// Used in ReceiveLocalCut() to wait for all local cuts to be received
		std::condition_variable cv_;
		std::condition_variable reset_cv_;

		/// Time between each local cut
		std::chrono::microseconds local_cut_interval_ = std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL);

		/// Used to keep track of how many grpc threads are waiting for the notification
		/// If it's greater than 0, we can't reset the local epoch
		int waiting_threads_count_ = 0;

		/// The head node keeps track of the global epoch and increments it whenever we complete a round of local cuts
		int global_epoch_;

		/// The key is the current epoch and it contains another map of broker_id to local cut
		absl::Mutex global_cut_mu_;
		absl::flat_hash_map<int, absl::btree_map<int, int>> global_cut_ ABSL_GUARDED_BY(global_cut_mu_);

		/// Used to keep track of # messages of each epoch so we can calculate the global cut
		/// Key is the current epoch and it contains another map of broker_id to logical offset
		absl::flat_hash_map<int, absl::btree_map<int, int>> logical_offsets_ ABSL_GUARDED_BY(global_cut_mu_);
	
		bool has_global_sequencer_;
};

} // End of namespace Embarcadero
#endif
