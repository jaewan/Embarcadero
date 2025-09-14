#include "cxl_manager.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <queue>
#include <tuple>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <glog/logging.h>
#include "mimalloc.h"
#include "common/configuration.h"

namespace Embarcadero{

static inline void* allocate_shm(int broker_id, CXL_Type cxl_type, size_t cxl_size){
	void *addr = nullptr;
	int cxl_fd;
	bool dev = false;
	if(cxl_type == Real){
		if(std::filesystem::exists("/dev/dax0.0")){
			dev = true;
			cxl_fd = open("/dev/dax0.0", O_RDWR);
		}else{
			if(numa_available() == -1){
				LOG(ERROR) << "Cannot allocate from real CXL";
				return nullptr;
			}else{
				cxl_fd = shm_open("/CXL_SHARED_FILE", O_CREAT | O_RDWR, 0666);
			}
		}
	}else{
		cxl_fd = shm_open("/CXL_SHARED_FILE", O_CREAT | O_RDWR, 0666);
	}

	if (cxl_fd < 0){
		LOG(ERROR)<<"Opening CXL error";
		return nullptr;
	}
	if(broker_id == 0 && !dev){
		if (ftruncate(cxl_fd, cxl_size) == -1) {
			LOG(ERROR) << "ftruncate failed";
			close(cxl_fd);
			return nullptr;
		}
	}
	addr = mmap(NULL, cxl_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, cxl_fd, 0);
	close(cxl_fd);
	if(addr == MAP_FAILED){
		LOG(ERROR) << "Mapping CXL failed";
		return nullptr;
	}

	if(cxl_type == Real && !dev && broker_id == 0){
		// Create a bitmask for the NUMA node (numa node 2 should be the CXL memory)
		struct bitmask* bitmask = numa_allocate_nodemask();
		numa_bitmask_setbit(bitmask, 2);

		// Bind the memory to the specified NUMA node
		if (mbind(addr, cxl_size, MPOL_BIND, bitmask->maskp, bitmask->size, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
			LOG(ERROR)<< "mbind failed";
			numa_free_nodemask(bitmask);
			munmap(addr, cxl_size);
			return nullptr;
		}

		numa_free_nodemask(bitmask);
	}

	if(broker_id == 0){
		memset(addr, 0, cxl_size);
		VLOG(3) << "Cleared CXL:" << cxl_size;
	}
	return addr;
}

CXLManager::CXLManager(int broker_id, CXL_Type cxl_type, std::string head_ip):
	broker_id_(broker_id),
	head_ip_(head_ip){
		size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

		if (cxl_type == Real) {
			cxl_size_ = CXL_SIZE;
		} else {
			cxl_size_ = CXL_EMUL_SIZE;
		}

		// Initialize CXL
		cxl_addr_ = allocate_shm(broker_id, cxl_type, cxl_size_);
		if(cxl_addr_ == nullptr){
			return;
		}

		// Initialize CXL memory regions
		size_t TINode_Region_size = sizeof(TInode) * MAX_TOPIC_SIZE;
		size_t padding = TINode_Region_size - ((TINode_Region_size/cacheline_size) * cacheline_size);
		TINode_Region_size += padding;
		size_t Bitmap_Region_size = cacheline_size * MAX_TOPIC_SIZE;
		// Use configured max brokers consistently with GetNewSegment()
		const size_t configured_max_brokers = NUM_MAX_BROKERS_CONFIG;
		size_t BatchHeaders_Region_size = configured_max_brokers * BATCHHEADERS_SIZE * MAX_TOPIC_SIZE;
		size_t Segment_Region_size = (cxl_size_ - TINode_Region_size - Bitmap_Region_size - BatchHeaders_Region_size)/configured_max_brokers;
		padding = Segment_Region_size%cacheline_size;
		Segment_Region_size -= padding;

		bitmap_ = (uint8_t*)cxl_addr_ + TINode_Region_size;
		batchHeaders_ = (uint8_t*)bitmap_ + Bitmap_Region_size;
		segments_ = (uint8_t*)batchHeaders_ + BatchHeaders_Region_size + ((broker_id_)*Segment_Region_size);
		batchHeaders_ = (uint8_t*)batchHeaders_ + (broker_id_ * (BATCHHEADERS_SIZE * MAX_TOPIC_SIZE));

		VLOG(3) << "\t[CXLManager]: \t\tConstructed";
		return;
	}

CXLManager::~CXLManager(){
	stop_threads_ = true;
	for(std::thread& thread : sequencerThreads_){
		if(thread.joinable()){
			thread.join();
		}
	}

	if (munmap(cxl_addr_, cxl_size_) < 0)
		LOG(ERROR) << "Unmapping CXL error";

	VLOG(3) << "[CXLManager]: \t\tDestructed";
}

std::function<void(void*, size_t)> CXLManager::GetCXLBuffer(BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header,
		size_t &logical_offset, SequencerType &seq_type) {
	return topic_manager_->GetCXLBuffer(batch_header, topic, log, segment_header,
			logical_offset, seq_type);
}

inline int hashTopic(const char topic[TOPIC_NAME_SIZE]) {
	unsigned int hash = 0;

	for (int i = 0; i < TOPIC_NAME_SIZE; ++i) {
		hash = (hash * TOPIC_NAME_SIZE) + topic[i];
	}
	return hash % MAX_TOPIC_SIZE;
}

// This function returns TInode without inspecting if the topic exists
TInode* CXLManager::GetTInode(const char* topic){
	// Convert topic to tinode address
	//static const std::hash<std::string> topic_to_idx;
	//int TInode_idx = topic_to_idx(topic) % MAX_TOPIC_SIZE;
	int TInode_idx = hashTopic(topic);
	return (TInode*)((uint8_t*)cxl_addr_ + (TInode_idx * sizeof(struct TInode)));
}

TInode* CXLManager::GetReplicaTInode(const char* topic){
	char replica_topic[TOPIC_NAME_SIZE];
	memcpy(replica_topic, topic, TOPIC_NAME_SIZE);
	memcpy((uint8_t*)replica_topic + (TOPIC_NAME_SIZE-7), "replica", 7); 
	int TInode_idx = hashTopic(replica_topic);
	return (TInode*)((uint8_t*)cxl_addr_ + (TInode_idx * sizeof(struct TInode)));
}

void* CXLManager::GetNewSegment(){
	//TODO(Jae) Implement bitmap
	static std::atomic<size_t> segment_count{0};
	
	// Calculate max segments only once (static)
	static size_t max_segments = []() {
		size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
		size_t TINode_Region_size = sizeof(TInode) * MAX_TOPIC_SIZE;
		size_t padding = TINode_Region_size - ((TINode_Region_size/cacheline_size) * cacheline_size);
		TINode_Region_size += padding;
		size_t Bitmap_Region_size = cacheline_size * MAX_TOPIC_SIZE;
		// Get configuration values
		size_t cxl_size = Embarcadero::Configuration::getInstance().config().cxl.size.get();
		const size_t configured_max_brokers = NUM_MAX_BROKERS_CONFIG;
		size_t BatchHeaders_Region_size = configured_max_brokers * BATCHHEADERS_SIZE * MAX_TOPIC_SIZE;
		
		// Memory is divided among configured broker slots regardless of how many are actually used
		// This ensures consistent memory layout
		size_t Segment_Region_size = (cxl_size - TINode_Region_size - Bitmap_Region_size - BatchHeaders_Region_size)/configured_max_brokers;
		size_t max_segs = Segment_Region_size / SEGMENT_SIZE;
		
		LOG(INFO) << "GetNewSegment: CXL_size=" << cxl_size
		          << " CONFIGURED_MAX_BROKERS=" << configured_max_brokers
		          << " Segment_Region_size=" << Segment_Region_size 
		          << " SEGMENT_SIZE=" << SEGMENT_SIZE 
		          << " max_segments=" << max_segs;
		return max_segs;
	}();
	
	size_t offset = segment_count.fetch_add(1, std::memory_order_relaxed);
	
	if (offset >= max_segments) {
		LOG(ERROR) << "Segment allocation overflow: offset=" << offset 
		           << " max_segments=" << max_segments;
		return nullptr;
	}

	return (uint8_t*)segments_ + offset*SEGMENT_SIZE;
}

void* CXLManager::GetNewBatchHeaderLog(){
	static std::atomic<size_t> batch_header_log_count{0};
	CHECK_LT(batch_header_log_count, MAX_TOPIC_SIZE) << "You are creating too many topics";
	size_t offset = batch_header_log_count.fetch_add(1, std::memory_order_relaxed);

	return (uint8_t*)batchHeaders_  + offset*BATCHHEADERS_SIZE;
}

void CXLManager::GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers,
		TInode *tinode) {
	if (!get_registered_brokers_callback_(registered_brokers, nullptr /* msg_to_order removed */, tinode)) {
		LOG(ERROR) << "GetRegisteredBrokerSet: Callback failed to get registered brokers.";
		registered_brokers.clear(); // Ensure set is empty on failure
	}
}

void CXLManager::GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
		MessageHeader** msg_to_order, TInode *tinode){
	if(get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode)){
		for(const auto &broker_id : registered_brokers){
			// Wait for other brokers to initialize this topic. 
			// This is here to avoid contention in grpc(hearbeat) which can cause deadlock when rpc is called
			// while waiting for other brokers to initialize (untill publish is called)
			while(tinode->offsets[broker_id].log_offset == 0){
				std::this_thread::yield();
			}
			msg_to_order[broker_id] = ((MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id].log_offset));
		}
	}
}

// Sequence without respecting publish order
void CXLManager::Sequencer1(std::array<char, TOPIC_NAME_SIZE> topic) {
	LOG(INFO) <<"[DEBUG] ************** Seqeuncer 1 **************";
	struct TInode *tinode = GetTInode(topic.data());
	if (!tinode) {
		LOG(ERROR) << "Sequencer1: Failed to get TInode for topic " << topic.data();
		return;
	}

	// Local storage for message pointers, initialized to nullptr
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS] = {nullptr};
	absl::btree_set<int> registered_brokers;
	absl::btree_set<int> initialized_brokers; // Track initialized brokers
	static size_t seq = 0; // Sequencer counter

	// Get the initial set of registered brokers (without waiting)
	GetRegisteredBrokerSet(registered_brokers, tinode);

	if (registered_brokers.empty()) {
		LOG(WARNING) << "Sequencer1: No registered brokers found for topic " << topic.data() << ". Sequencer might idle.";
	}

	while (!stop_threads_) {
		bool processed_message = false; // Track if any work was done in this outer loop iteration

		// TODO: If brokers can register dynamically, call GetRegisteredBrokerSet periodically
		//       and update the registered_brokers set here.

		for (auto broker_id : registered_brokers) {
			// --- Dynamic Initialization Check ---
			if (initialized_brokers.find(broker_id) == initialized_brokers.end()) {
				// This broker hasn't been initialized yet, check its log offset NOW
				size_t current_log_offset = tinode->offsets[broker_id].log_offset; // Read the current offset

				if (current_log_offset == 0) {
					// Still not initialized, skip this broker for this iteration
					VLOG(5) << "Sequencer1: Broker " << broker_id << " log still uninitialized (offset=0), skipping.";
					continue;
				} else {
					// Initialize Now!
					VLOG(5) << "Sequencer1: Initializing broker " << broker_id << " with log_offset=" << current_log_offset;
					msg_to_order[broker_id] = ((MessageHeader*)((uint8_t*)cxl_addr_ + current_log_offset));
					initialized_brokers.insert(broker_id); // Mark as initialized
																								 // Proceed to process messages below
				}
			}

			// --- Process Messages if Initialized ---
			// Ensure msg_to_order pointer is valid before dereferencing
			if (msg_to_order[broker_id] == nullptr) {
				// This should ideally not happen if the logic above is correct, but safety check
				LOG(DFATAL) << "Sequencer1: msg_to_order[" << broker_id << "] is null despite being marked initialized!";
				continue;
			}

			// Read necessary volatile/shared values (consider atomics/locking if needed)
			size_t current_written_offset = tinode->offsets[broker_id].written; // Where the broker has written up to (logical offset)
																																					// Note: MessageHeader fields read below might also need volatile/atomic handling

																																					// Check if broker has indicated completion/error
			if (current_written_offset == static_cast<size_t>(-1)) {
				// Broker might be done or encountered an error, skip it permanently?
				// Or maybe just for this round? Depends on the meaning of -1.
				VLOG(4) << "Sequencer1: Broker " << broker_id << " written offset is -1, skipping.";
				continue;
			}

			// Get the logical offset embedded in the *current* message header we're pointing to
			// This assumes logical_offset field correctly tracks message sequence within the broker's log
			size_t msg_logical_off = msg_to_order[broker_id]->logical_offset;


			// Inner loop to process available messages for this broker
			while (!stop_threads_ &&
					msg_logical_off != static_cast<size_t>(-1) && // Check if current message is valid
					msg_logical_off <= current_written_offset && // Check if message offset has been written by broker
					msg_to_order[broker_id]->next_msg_diff != 0)  // Check if it links to a next message (validity)
			{
				// Check if total order has already been assigned (e.g., by another sequencer replica?)
				// Need to define what indicates "not yet assigned". Using 0 might be risky if 0 is valid.
				// Let's assume unassigned is indicated by a specific value, e.g., -1 or max_size_t
				// For now, let's assume we always assign if the conditions above are met. Revisit if needed.

				VLOG(5) << "Sequencer1: Assigning seq=" << seq << " to broker=" << broker_id << ", logical_offset=" << msg_logical_off;
				msg_to_order[broker_id]->total_order = seq; // Assign sequence number
																										// TODO: Ensure this write is visible (volatile, atomic, or fence)
																										// std::atomic_thread_fence(std::memory_order_release); // Example fence if needed

				seq++; // Increment global sequence number

				// Update TInode about the latest processed message *for this broker*
				// Assuming UpdateTinodeOrder persists this information safely
				UpdateTinodeOrder(topic.data(), tinode, broker_id, msg_logical_off,
						(uint8_t*)msg_to_order[broker_id] - (uint8_t*)cxl_addr_); // Pass CXL relative offset

				processed_message = true; // We did some work
				msg_to_order[broker_id] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker_id] + msg_to_order[broker_id]->next_msg_diff);
				msg_logical_off = msg_to_order[broker_id]->logical_offset;

			} // End inner while loop for processing broker messages

		} // End for loop iterating through registered_brokers

		// If no messages were processed across all brokers, yield briefly
		// This prevents busy-spinning when there's no new data.
		if (!processed_message && !stop_threads_) {
			// Check again if any uninitialized brokers became initialized
			bool potentially_newly_initialized = false;
			for(auto broker_id : registered_brokers) {
				if (initialized_brokers.find(broker_id) == initialized_brokers.end()) {
					if (tinode->offsets[broker_id].log_offset != 0) {
						potentially_newly_initialized = true;
						break;
					}
				}
			}
			if (!potentially_newly_initialized) {
				std::this_thread::yield();
			}
		}

	} // End outer while(!stop_threads_)
}

// Order 2 with single thread
void CXLManager::Sequencer2(std::array<char, TOPIC_NAME_SIZE> topic){
	LOG(INFO) <<"[DEBUG] ************** Seqeucner2 **************";
	struct TInode *tinode = GetTInode(topic.data());
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS];
	absl::btree_set<int> registered_brokers;
	absl::flat_hash_map<int/*client_id*/, size_t/*client_req_id*/> last_ordered; 
	// Store skipped messages to respect the client order.
	// Use absolute adrress b/c it is only used in this thread later
	absl::flat_hash_map<int/*client_id*/, absl::btree_map<size_t/*client_order*/, std::pair<int /*broker_id*/, struct MessageHeader*>>> skipped_msg;
	static size_t seq = 0;
	// Tracks the messages of written order to later report the sequentially written messages
	std::array<std::queue<MessageHeader* /*physical addr*/>, NUM_MAX_BROKERS> queues;


	GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
	auto last_updated = std::chrono::steady_clock::now();
	while(!stop_threads_){
		bool yield = true;
		for(auto broker : registered_brokers){
			size_t msg_logical_off = msg_to_order[broker]->logical_offset;
			//This ensures the message is Combined (complete ensures it is fully received)
			if(msg_to_order[broker]->complete == 1 && msg_logical_off != (size_t)-1 && msg_logical_off <= tinode->offsets[broker].written){
				yield = false;
				queues[broker].push(msg_to_order[broker]);
				int client = msg_to_order[broker]->client_id;
				size_t client_order = msg_to_order[broker]->client_order;
				auto last_ordered_itr = last_ordered.find(client);
				if(client_order == 0 || 
						(last_ordered_itr != last_ordered.end() && last_ordered_itr->second == client_order - 1)){
					msg_to_order[broker]->total_order = seq;
					seq++;
					last_ordered[client] = client_order;
					// Check if there are skipped messages from this client and give order
					auto it = skipped_msg.find(client);
					if(it != skipped_msg.end()){
						std::vector<int> to_remove;
						for (auto& pair : it->second) {
							int client_order = pair.first;
							if((size_t)client_order == last_ordered[client] + 1){
								pair.second.second->total_order = seq;
								seq++;
								last_ordered[client] = client_order;
								to_remove.push_back(client_order);
							}else{
								break;
							}
						}
						for(auto &id: to_remove){
							it->second.erase(id);
						}
					}
					for(auto b: registered_brokers){
						if(queues[b].empty()){
							continue;
						}else{
							MessageHeader  *header = queues[b].front();
							MessageHeader* exportable_msg = nullptr;
							while(header->client_order <= last_ordered[header->client_id]){
								queues[b].pop();
								exportable_msg = header;
								if(queues[b].empty()){
									break;
								}
								header = queues[b].front();
							}
							if(exportable_msg){
								UpdateTinodeOrder(topic.data(), tinode, b, exportable_msg->logical_offset,(uint8_t*)exportable_msg - (uint8_t*)cxl_addr_);
							}
						}
					}
				}else{
					queues[broker].push(msg_to_order[broker]);
					//Insert to skipped messages
					auto it = skipped_msg.find(client);
					if (it == skipped_msg.end()) {
						absl::btree_map<size_t, std::pair<int, MessageHeader*>> new_map;
						new_map.emplace(client_order, std::make_pair(broker, msg_to_order[broker]));
						skipped_msg.emplace(client, std::move(new_map));
					} else {
						it->second.emplace(client_order, std::make_pair(broker, msg_to_order[broker]));
					}
				}
				msg_to_order[broker] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker] + msg_to_order[broker]->next_msg_diff);
			}
		} // end broker loop
		if(yield){
			GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			last_updated = std::chrono::steady_clock::now();
			std::this_thread::yield();
		}else if(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
					- last_updated).count() >= HEARTBEAT_INTERVAL){
			GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			last_updated = std::chrono::steady_clock::now();
		}
	}// end while
}

// Does not support multi-client, dynamic message size, dynamic batch 
void CXLManager::Sequencer3(std::array<char, TOPIC_NAME_SIZE> topic){
	LOG(INFO) <<"[DEBUG] ************** Seqeuncer 3 **************";
	struct TInode *tinode = GetTInode(topic.data());
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS];
	absl::btree_set<int> registered_brokers;
	static size_t seq = 0;
	static size_t batch_seq = 0;

	GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
	//auto last_updated = std::chrono::steady_clock::now();
	size_t num_brokers = registered_brokers.size();

	while(!stop_threads_){
		//bool yield = true;
		for(auto broker : registered_brokers){
			while(msg_to_order[broker]->complete == 0){
				if(stop_threads_)
					return;
				std::this_thread::yield();
			}
			size_t num_msg_per_batch = BATCH_SIZE / msg_to_order[broker]->paddedSize;
			size_t msg_logical_off = (batch_seq/num_brokers)*num_msg_per_batch;
			size_t n = msg_logical_off + num_msg_per_batch;
			while(!stop_threads_ && msg_logical_off < n){
				size_t written = tinode->offsets[broker].written;
				if(written == (size_t)-1){
					continue;
				}
				written = std::min(written, n-1);
				while(!stop_threads_ && msg_logical_off <= written && msg_to_order[broker]->next_msg_diff != 0 
						&& msg_to_order[broker]->logical_offset != (size_t)-1){
					msg_to_order[broker]->total_order = seq;
					seq++;
					//std::atomic_thread_fence(std::memory_order_release);
					UpdateTinodeOrder(topic.data(), tinode, broker, msg_logical_off, (uint8_t*)msg_to_order[broker] - (uint8_t*)cxl_addr_);
					msg_to_order[broker] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker] + msg_to_order[broker]->next_msg_diff);
					msg_logical_off++;
				}
			}
			batch_seq++;
		}
		/*
			 if(yield){
			 GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			 last_updated = std::chrono::steady_clock::now();
			 std::this_thread::yield();
			 }else if(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
			 - last_updated).count() >= HEARTBEAT_INTERVAL){
			 GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			 last_updated = std::chrono::steady_clock::now();
			 }
			 */
	}
}

// Return sequentially ordered logical offset + 1 and 
// if end offset's physical address should be stored in end_offset_logical_to_physical_
size_t CXLManager::SequentialOrderTracker::InsertAndGetSequentiallyOrdered(size_t offset, size_t size){
	//absl::MutexLock lock(&range_mu_);
	size_t end = offset + size;

	// Find the first range that starts after our offset
	auto next_it = ordered_ranges_.upper_bound(offset);

	// Check if we can merge with the previous range
	if (next_it != ordered_ranges_.begin()) {
		auto prev_it = std::prev(next_it);
		if (prev_it->second >= offset) {
			// Our range overlaps with the previous one
			offset = prev_it->first;
			end = std::max(end, prev_it->second);
			ordered_ranges_.erase(prev_it);
			end_offset_logical_to_physical_.erase(prev_it->second);
		}
	}

	// Merge with any subsequent overlapping ranges
	// Do not have to be while as ranges will neve overlap but keep it for now
	while (next_it != ordered_ranges_.end() && next_it->first <= end) {
		size_t next_end_logical = next_it->second; // Store logical end before erasing
		auto to_erase = next_it++;
		ordered_ranges_.erase(to_erase);

		if(end < next_end_logical){
			end_offset_logical_to_physical_.erase(end);
			end = next_end_logical;
		}else if (end > next_end_logical){
			end_offset_logical_to_physical_.erase(next_end_logical);
		}
	}

	// Insert the merged range
	ordered_ranges_[offset] = end;

	return GetSequentiallyOrdered();
	// Find the lateset squentially ordered message offset
	if (ordered_ranges_.empty() || ordered_ranges_.begin()->first > 0) {
		return 0;
	}

	return ordered_ranges_.begin()->second;
	// Start with the range that begins at offset 0
	auto current_range_it = ordered_ranges_.begin();
	size_t current_end = current_range_it ->second;

	// Look for adjacent or overlapping ordered_ranges
	auto it = std::next(current_range_it );
	while (it != ordered_ranges_.end() && it->first <= current_end) {
		current_end = std::max(current_end, it->second);
		++it;
	}

	return current_end;
}

} // End of namespace Embarcadero
