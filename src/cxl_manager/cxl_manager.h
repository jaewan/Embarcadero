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
#include <grpcpp/grpcpp.h>
#include <scalog_sequencer.grpc.pb.h>
#include <boost/asio.hpp>
#include "../embarlet/heartbeat.h"

namespace Embarcadero{

class TopicManager;
class NetworkManager;
class HeartBeatManager;

class ScalogSequencerService : public ScalogSequencer::Service {
	public:
		ScalogSequencerService(CXLManager* cxl_manager, int broker_id, const char* topic, HeartBeatManager *broker, void* cxl_addr, std::string scalog_seq_address);

		absl::flat_hash_map<int, int> GetGlobalCut() {
			return global_cut_;
		}

		/// Create a new rpc client to communicate with a peer broker
        /// @param peer_url URL of the peer broker
        /// @return rpc client
        std::unique_ptr<ScalogSequencer::Stub> GetRpcClient(std::string peer_url);

    	/// Receives a local cut from a local sequencer
        /// @param context
        /// @param request Request containing the local cut and the epoch
        /// @param response Empty for now
		grpc::Status HandleSendLocalCut(grpc::ServerContext* context, const SendLocalCutRequest* request, SendLocalCutResponse* response, std::function<void(grpc::Status)> callback);

    	/// Receives the global cut from global sequencer
        /// @param context
        /// @param request Request containing the global cut and topic
        /// @param response Empty for now
		grpc::Status HandleSendGlobalCut(grpc::ServerContext* context, const SendGlobalCutRequest* request, SendGlobalCutResponse* response);

	private:
		CXLManager* cxl_manager_;
		HeartBeatManager *broker_;
		int broker_id_;
		void* cxl_addr_;
		std::unique_ptr<ScalogSequencer::Stub> stub_;

		absl::flat_hash_map<int, std::function<void(grpc::Status)>> follower_callbacks_;

		absl::flat_hash_map<int, SendLocalCutResponse*> follower_responses_;

		int local_epoch_;

		int global_epoch_;

		int local_cuts_count_;

		bool received_global_seq_;

		absl::flat_hash_map<int, int> global_cut_;

		bool has_global_sequencer_;

		bool received_gobal_seq_after_interval_;

        /// Thread to run the io_service in a loop
        std::unique_ptr<std::thread> io_service_thread_;

        /// IO context that peers use to post tasks
        boost::asio::io_context io_service_;

		/// Timer used to perform async wait before sending the next local cut
		using Timer = boost::asio::deadline_timer;
		Timer timer_;

		void LocalSequencer(const char* topic);
		void GlobalSequencer(const char* topic);
		void SendLocalCut(int epoch, int local_cut, const char* topic);
		void ReceiveGlobalCut(absl::flat_hash_map<int, int>  global_cut, const char* topic);
		void ReceiveLocalCut(int epoch, const char* topic, int broker_id);
		void UpdateTotalOrdering(absl::flat_hash_map<int, int>  global_cut, struct TInode *tinode);
};

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

struct alignas(64) MessageHeader{
	int client_id;
	size_t client_order;
	volatile size_t size;
	volatile size_t paddedSize; // This include message+padding+header size
	void* segment_header;
	volatile size_t total_order;
	size_t logical_offset;
	unsigned long long int next_msg_diff; // Relative to message_header, not cxl_addr_
};

class CXLManager {
	public:
		CXLManager(size_t queueCapacity, int broker_id, CXL_Type cxl_type, std::string head_address, int num_io_threads);
		~CXLManager();
		void SetBroker(HeartBeatManager *broker){
			broker_ = broker;
		}
		void SetTopicManager(TopicManager *topic_manager){topic_manager_ = topic_manager;}
		void SetNetworkManager(NetworkManager* network_manager){network_manager_ = network_manager;}
		void EnqueueRequest(struct PublishRequest req);
		void* GetNewSegment();
		void* GetTInode(const char* topic);
		bool GetMessageAddr(const char* topic, size_t &last_offset,
				void* &last_addr, void* &messages, size_t &messages_size);
		void RunSequencer(char topic[TOPIC_NAME_SIZE], int order, SequencerType sequencerType);
		void StartScalogLocalSequencer(std::string topic_str);
//#define InternalTest 1
		void* GetCXLAddr(){return cxl_addr_;}
		void RegisterGetRegisteredBrokersCallback(GetRegisteredBrokersCallback callback){
			get_registered_brokers_callback_ = callback;
		}

	private:
		folly::MPMCQueue<std::optional<struct PublishRequest>> requestQueue_;
		int broker_id_;
		std::string head_ip_;
		int num_io_threads_;
		std::vector<std::thread> threads_;
		std::vector<std::thread> sequencerThreads_;

		TopicManager *topic_manager_;
		NetworkManager *network_manager_;
		HeartBeatManager *broker_;

		void* cxl_addr_;
		void* bitmap_;
		void* segments_;
		void* current_log_addr_;
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};
		GetRegisteredBrokersCallback get_registered_brokers_callback_;

		void CXLIOThread();
		void Sequencer1(char* topic);
		void Sequencer2(char* topic);

		std::unique_ptr<ScalogSequencerService> scalog_sequencer_service_;
		std::unique_ptr<grpc::Server> scalog_server_;
};

} // End of namespace Embarcadero
#endif
