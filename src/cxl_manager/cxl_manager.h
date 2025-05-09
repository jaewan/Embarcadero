#ifndef INCLUDE_CXL_MANGER_H_
#define INCLUDE_CXL_MANGER_H_

#include <thread>
#include <iostream>
#include <optional>

#include "folly/MPMCQueue.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/btree_map.h"
#include <grpcpp/grpcpp.h>

#include <heartbeat.grpc.pb.h>
#include "../embarlet/heartbeat.h"
#include "cxl_datastructure.h"
#include "scalog_local_sequencer.h"
#include "../embarlet/topic_manager.h"
#include "../network_manager/network_manager.h"

namespace Embarcadero{

class TopicManager;
class NetworkManager;
class HeartBeatManager;
class ScalogLocalSequencer;

enum CXL_Type {Emul, Real};

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
		void RunScalogSequencer(const char topic[TOPIC_NAME_SIZE]);
		void* GetCXLAddr(){return cxl_addr_;}
		void RegisterGetRegisteredBrokersCallback(GetRegisteredBrokersCallback callback){
			get_registered_brokers_callback_ = callback;
		}
		std::function<void(void*, size_t)> GetCXLBuffer(BatchHeader &batch_header, const char topic[TOPIC_NAME_SIZE],
				void* &log, void* &segment_header, size_t &logical_offset, SequencerType &seq_type);
		void GetRegisteredBrokers(absl::btree_set<int> &registered_brokers,
				struct MessageHeader** msg_to_order, struct TInode *tinode);
		void GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers, struct TInode *tinode);

		/// Initializes the scalog sequencer service and starts the grpc server
		void StartScalogLocalSequencer(std::string topic_str);

		/// Receives the global cut from the head node
		/// This function is called in the callback of the send local cut grpc call
		void ScalogSequencer(const char* topic, absl::flat_hash_map<int, absl::btree_map<int, int>> &global_cut);

		void SetEpochToOrder(int epoch){
			epoch_to_order_ = epoch;
		}
		bool GetStopThreads(){
			return stop_threads_;
		}

		inline void UpdateTinodeOrder(char *topic, TInode* tinode, int broker, size_t msg_logical_off, size_t ordered_offset){
			if(tinode->replicate_tinode){
				struct TInode *replica_tinode = GetReplicaTInode(topic);
				replica_tinode->offsets[broker].ordered = msg_logical_off;
				replica_tinode->offsets[broker].ordered_offset = ordered_offset;
			}

			tinode->offsets[broker].ordered = msg_logical_off;
			tinode->offsets[broker].ordered_offset = ordered_offset;
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


		void Sequencer1(std::array<char, TOPIC_NAME_SIZE> topic);
		void Sequencer2(std::array<char, TOPIC_NAME_SIZE> topic);
		void Sequencer3(std::array<char, TOPIC_NAME_SIZE> topic);

		size_t global_seq_ = 0;
		// Map: client_id -> next expected batch_seq
		absl::flat_hash_map<size_t, size_t> next_expected_batch_seq_;
		absl::Mutex global_seq_batch_seq_mu_;;
		folly::MPMCQueue<BatchHeader*> ready_batches_queue_{1024*8};


		class SequentialOrderTracker{
			public:
				SequentialOrderTracker()= default;
				SequentialOrderTracker(int broker_id): broker_id_(broker_id){}
				size_t InsertAndGetSequentiallyOrdered(size_t batch_start_offset, size_t size);


				// Current Order 4 logic only assigns order with one thread per broker
				// Thus, no coordination(lock) is needed within a broker with a single thread
				void StorePhysicalOffset(size_t logical_offset , size_t physical_offset){
					//absl::MutexLock lock(&offset_mu_);
					end_offset_logical_to_physical_.emplace(logical_offset, physical_offset);
				}

				size_t GetSequentiallyOrdered(){
					// Find the lateset squentially ordered message offset
					if (ordered_ranges_.empty() || ordered_ranges_.begin()->first > 0) {
						return 0;
					}

					return ordered_ranges_.begin()->second;
				}

				size_t GetPhysicalOffset(size_t logical_offset) {
					//absl::MutexLock lock(&offset_mu_);
					auto itr = end_offset_logical_to_physical_.find(logical_offset);
					if(itr == end_offset_logical_to_physical_.end()){
						return 0;
					}else{
						return itr->second;
					}
				}

			private:
				int broker_id_;
				absl::Mutex range_mu_;
				absl::Mutex offset_mu_;
				std::map<size_t, size_t> ordered_ranges_ ABSL_GUARDED_BY(range_mu_); //start --> end logical_offset
				absl::flat_hash_map<size_t, size_t> end_offset_logical_to_physical_ ABSL_GUARDED_BY(offset_mu_);
		};
		absl::flat_hash_map<size_t, std::unique_ptr<SequentialOrderTracker>> trackers_;

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
