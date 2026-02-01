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
#include "../common/performance_utils.h"

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

		// [[PHASE_2]] Accessors for GOI and CompletionVector
		GOIEntry* GetGOI() { return goi_; }
		CompletionVectorEntry* GetCompletionVector() { return completion_vector_; }
		ControlBlock* GetControlBlock() { return control_block_; }

		void RegisterGetRegisteredBrokersCallback(GetRegisteredBrokersCallback callback){
			get_registered_brokers_callback_ = callback;
		}
		std::function<void(void*, size_t)> GetCXLBuffer(BatchHeader &batch_header, const char topic[TOPIC_NAME_SIZE],
				void* &log, void* &segment_header, size_t &logical_offset, SequencerType &seq_type, 
				BatchHeader* &batch_header_location);
		// [[RECV_DIRECT_TO_CXL]] Split allocation for zero-copy receive path
		bool ReserveBLogSpace(const char* topic, size_t size, void*& log);
		bool IsPBRAboveHighWatermark(const char* topic, int high_pct);
		bool IsPBRBelowLowWatermark(const char* topic, int low_pct);
		bool ReservePBRSlotAndWriteEntry(const char* topic, BatchHeader& batch_header, void* log,
				void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location);
		// [[PERF]] Get Topic* once per connection to avoid 3× topics_mutex_ per batch
		Topic* GetTopicPtr(const char* topic);
		bool IsPBRAboveHighWatermark(Topic* topic_ptr, int high_pct);
		bool ReserveBLogSpace(Topic* topic_ptr, size_t size, void*& log, bool epoch_already_checked = false);
		bool ReservePBRSlotAndWriteEntry(Topic* topic_ptr, BatchHeader& batch_header, void* log,
				void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);
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

			// Flush cache line after TInode metadata update for CXL visibility
			// Paper §4.2 - Flush & Poll principle: Writers must flush after writes to non-coherent CXL
			// Note: DEV-002 (batched flushes) planned - could batch if multiple fields in same cache line
			CXL::flush_cacheline(const_cast<const void*>(static_cast<volatile void*>(&tinode->offsets[broker])));
			CXL::store_fence();
		}

	private:
		int broker_id_;
		std::string head_ip_;
		size_t cxl_size_;
		std::vector<std::thread> sequencerThreads_;

		TopicManager *topic_manager_;
		NetworkManager *network_manager_;

		void* cxl_addr_;
		ControlBlock* control_block_;                 // [[PHASE_1A]] At offset 0; epoch-based fencing
		CompletionVectorEntry* completion_vector_;    // [[PHASE_2]] At offset 0x1000; ACK path (32 × 128B)
		GOIEntry* goi_;                               // [[PHASE_2]] At offset 0x2000; 256M entries × 64B = 16GB
		void* base_for_regions_;                      // [[PHASE_1A]] cxl_addr_ + sizeof(ControlBlock); TInode/bitmap/batchHeaders/segments base
		void* bitmap_;
		void* batchHeaders_;
		void* segments_;
		void* current_log_addr_;
		volatile bool stop_threads_ = false;
		GetRegisteredBrokersCallback get_registered_brokers_callback_;
		
		// [[DEVIATION_005]] Future Multi-Node CXL Support: SegmentAllocator Abstraction
		// Uncomment and implement when multi-node non-coherent CXL is available
		/*
		// Abstract allocation protocol for pluggable strategies
		class SegmentAllocator {
		public:
			virtual ~SegmentAllocator() = default;
			virtual void* AllocateSegment() = 0;
			virtual void DeallocateSegment(void* segment) = 0;
		};
		
		// Current implementation (cache-coherent atomic) - Phase 1
		class AtomicBitmapAllocator : public SegmentAllocator {
		private:
			uint64_t* bitmap_;
			void* segments_;
			size_t total_segments_;
			size_t bitmap_words_;
			thread_local static size_t hint_;
			
		public:
			AtomicBitmapAllocator(void* bitmap, void* segments, size_t total_segments)
				: bitmap_(static_cast<uint64_t*>(bitmap))
				, segments_(segments)
				, total_segments_(total_segments)
				, bitmap_words_((total_segments + 63) / 64) {}
			
			void* AllocateSegment() override {
				// Current implementation (see GetNewSegment() above)
				// Uses __atomic_fetch_or for lock-free allocation
			}
			
			void DeallocateSegment(void* segment) override {
				// Calculate segment index from address
				// Clear corresponding bit in bitmap
			}
		};
		
		// Future implementation: Partitioned Bitmap (Option A)
		// Each broker gets its own bitmap region (segments 0-31, 32-63, etc.)
		// No cross-broker coordination needed
		// Works on non-coherent CXL (no shared cache lines)
		// Trade-off: One broker can't borrow from others if it runs out
		class PartitionedBitmapAllocator : public SegmentAllocator {
		private:
			int broker_id_;
			int max_brokers_;
			uint64_t* bitmap_;  // Points to this broker's partition
			void* segments_;
			size_t segments_per_broker_;
			
		public:
			PartitionedBitmapAllocator(int broker_id, int max_brokers, 
			                          void* bitmap, void* segments, 
			                          size_t total_segments)
				: broker_id_(broker_id)
				, max_brokers_(max_brokers)
				, segments_per_broker_(total_segments / max_brokers) {
				// Calculate this broker's bitmap partition
				size_t bitmap_words_per_broker = (segments_per_broker_ + 63) / 64;
				bitmap_ = static_cast<uint64_t*>(bitmap) + (broker_id * bitmap_words_per_broker);
				segments_ = static_cast<uint8_t*>(segments) + (broker_id * segments_per_broker_ * SEGMENT_SIZE);
			}
			
			void* AllocateSegment() override {
				// Same atomic bitmap logic, but only scans this broker's partition
				// No cross-broker coordination needed
			}
		};
		
		// Future implementation: Leader-Based Allocation (Option B)
		// One broker (leader) allocates segments via network RPC
		// Simple coordination model
		// Network overhead (~30μs per allocation)
		// Single point of failure (needs leader election)
		class LeaderBasedAllocator : public SegmentAllocator {
		private:
			bool is_leader_;
			int leader_broker_id_;
			// Network client for RPC to leader
			
		public:
			void* AllocateSegment() override {
				if (is_leader_) {
					return AllocateLocal();  // Use atomic bitmap
				} else {
					return RequestFromLeader();  // Network RPC
				}
			}
		};
		
		// Future implementation: CXL 3.0 Hardware-Assisted Atomics (Option C)
		// Hardware-assisted atomic operations (if available)
		// Best of both worlds (fast + non-coherent)
		// Requires CXL 3.0 hardware support
		class CXL3AtomicAllocator : public SegmentAllocator {
		public:
			void* AllocateSegment() override {
				// Use CXL 3.0 atomic operations (if available)
				// Hardware handles cache coherence
			}
		};
		
		// CXLManager would use abstraction:
		// std::unique_ptr<SegmentAllocator> allocator_;
		// 
		// In constructor:
		// if (is_single_node_) {
		//     allocator_ = std::make_unique<AtomicBitmapAllocator>(bitmap_, segments_, total_segments);
		// } else if (use_partitioned_) {
		//     allocator_ = std::make_unique<PartitionedBitmapAllocator>(broker_id_, max_brokers_, bitmap_, segments_, total_segments);
		// } else {
		//     allocator_ = std::make_unique<LeaderBasedAllocator>(is_leader_, leader_broker_id_);
		// }
		*/
		


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
