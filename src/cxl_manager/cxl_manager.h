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
#include "../embarlet/topic_manager.h"
#include "../network_manager/network_manager.h"

namespace Embarcadero{

class TopicManager;
class NetworkManager;
class HeartBeatManager;

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
		void* GetCXLAddr(){return cxl_addr_;}
		void RegisterGetRegisteredBrokersCallback(GetRegisteredBrokersCallback callback){
			get_registered_brokers_callback_ = callback;
		}
		std::function<void(void*, size_t)> GetCXLBuffer(BatchHeader &batch_header, const char topic[TOPIC_NAME_SIZE],
				void* &log, void* &segment_header, size_t &logical_offset, SequencerType &seq_type, 
				BatchHeader* &batch_header_location);
		void GetRegisteredBrokers(absl::btree_set<int> &registered_brokers,
				MessageHeader** msg_to_order, TInode *tinode);
		void GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers, TInode *tinode);

		// Phase 1.2: Memory layout calculation functions for PBR and GOI
		static size_t CalculatePBROffset(int broker_id, int max_brokers);
		static size_t CalculateGOIOffset(int max_brokers);
		static size_t CalculateBrokerLogOffset(int broker_id, int max_brokers);
		static size_t GetTotalMemoryRequirement(int max_brokers);

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

} // End of namespace Embarcadero
#endif
