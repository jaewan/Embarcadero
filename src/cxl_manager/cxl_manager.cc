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
		size_t BatchHeaders_Region_size = NUM_MAX_BROKERS * BATCHHEADERS_SIZE * MAX_TOPIC_SIZE;
		size_t Segment_Region_size = (cxl_size_ - TINode_Region_size - Bitmap_Region_size - BatchHeaders_Region_size)/NUM_MAX_BROKERS;
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
	std::atomic<size_t> segment_count{0};
	size_t offset = segment_count.fetch_add(1, std::memory_order_relaxed);

	return (uint8_t*)segments_ + offset*SEGMENT_SIZE;
}

void* CXLManager::GetNewBatchHeaderLog(){
	std::atomic<size_t> batch_header_log_count{0};
	CHECK_LT(batch_header_log_count, MAX_TOPIC_SIZE) << "You are creating too many topics";
	size_t offset = batch_header_log_count.fetch_add(1, std::memory_order_relaxed);

	return (uint8_t*)batchHeaders_  + offset*BATCHHEADERS_SIZE;
}

bool CXLManager::GetMessageAddr(const char* topic, size_t &last_offset,
		void* &last_addr, void* &messages, size_t &messages_size){
	return topic_manager_->GetMessageAddr(topic, last_offset, last_addr, messages, messages_size);
}

void CXLManager::RunSequencer(const char topic[TOPIC_NAME_SIZE], int order, SequencerType sequencerType){
	if (order == 0)
		return;
	std::array<char, TOPIC_NAME_SIZE> topic_arr;
	memcpy(topic_arr.data(), topic, TOPIC_NAME_SIZE);

	switch(sequencerType){
		case KAFKA: // Kafka is just a way to not run CombinerThread, not actual sequencer
		case EMBARCADERO:
			if (order == 1)
				sequencerThreads_.emplace_back(&CXLManager::Sequencer1, this, topic_arr);
			else if (order == 2)
				sequencerThreads_.emplace_back(&CXLManager::Sequencer2, this, topic_arr);
			else if (order == 3)
				sequencerThreads_.emplace_back(&CXLManager::Sequencer3, this, topic_arr);
			else if (order == 4){
				sequencerThreads_.emplace_back(&CXLManager::Sequencer4, this, topic_arr);
			}
			break;
		case SCALOG:
			if (order == 1){
				std::string topic_str(topic);
				sequencerThreads_.emplace_back(&CXLManager::StartScalogLocalSequencer, this, topic_str);
			}else if (order == 2)
				LOG(ERROR) << "Order is set 2 at scalog";
			break;
		case CORFU:
			if (order == 1)
				LOG(ERROR) << "Order is set 1 at corfu";
			else if (order == 2){
				LOG(INFO) << "Order 2 for Corfu is right. But check Client library to change order to 0 in corfu as corfu is already ordered at publish";
			}
			break;
		default:
			LOG(ERROR) << "Unknown sequencer:" << sequencerType;
			break;
	}
}

void CXLManager::GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers,
                                         struct TInode *tinode) {
    if (!get_registered_brokers_callback_(registered_brokers, nullptr /* msg_to_order removed */, tinode)) {
			 LOG(ERROR) << "GetRegisteredBrokerSet: Callback failed to get registered brokers.";
			 registered_brokers.clear(); // Ensure set is empty on failure
    }
}

void CXLManager::GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
		struct MessageHeader** msg_to_order, struct TInode *tinode){
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

inline void CXLManager::UpdateTInodeOrderandWritten(char *topic, TInode* tinode, int broker, size_t msg_logical_off, size_t ordered_offset){
	if(tinode->replicate_tinode){
		struct TInode *replica_tinode = GetReplicaTInode(topic);
		replica_tinode->offsets[broker].ordered = msg_logical_off;
		replica_tinode->offsets[broker].ordered_offset = ordered_offset;
		replica_tinode->offsets[broker].written = msg_logical_off;
		replica_tinode->offsets[broker].written_addr = ordered_offset;
	}

	tinode->offsets[broker].ordered = msg_logical_off;
	tinode->offsets[broker].ordered_offset = ordered_offset;
	tinode->offsets[broker].written = msg_logical_off;
	tinode->offsets[broker].written_addr = ordered_offset;
}

// Sequence without respecting publish order
void CXLManager::Sequencer1(std::array<char, TOPIC_NAME_SIZE> topic) {
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

void CXLManager::Sequencer4(std::array<char, TOPIC_NAME_SIZE> topic) {
	struct TInode *tinode = GetTInode(topic.data());

	absl::btree_set<int> registered_brokers;
	GetRegisteredBrokerSet(registered_brokers, tinode);

	global_seq_.store(0);

	absl::flat_hash_map<size_t, std::atomic<size_t>> next_expected_batch_seq; // client_id -> next expected batch_seq
	folly::MPMCQueue<BatchHeader*> ready_batches_queue(1024);

	sequencer4_threads_.clear();

	for (int broker_id : registered_brokers) {
		trackers_.emplace(
				broker_id,
				std::make_unique<SequentialOrderTracker>(broker_id)
				);
		sequencer4_threads_.emplace_back(
				&CXLManager::BrokerScannerWorker,
				this, // Pass pointer to current object
				broker_id,
				topic // Pass topic by value (or const reference)
				);
	}

	// Join worker threads
	for(auto &t : sequencer4_threads_){
		while(!t.joinable()){
			std::this_thread::yield();
		}
		t.join();
	}
	sequencer4_threads_.clear(); // Clear after joining
}

// This does not work with multi-segments as it advances to next messaeg with message's size
void CXLManager::BrokerScannerWorker(int broker_id, std::array<char, TOPIC_NAME_SIZE> topic) {
	struct TInode *tinode = GetTInode(topic.data());
	// Wait until tinode of the broker is initialized by the broker
	// Sequencer4 relies on GetRegisteredBrokerSet that does not wait
	while(tinode->offsets[broker_id].log_offset == 0){
		std::this_thread::yield();
	}
	// Get the starting point for this broker's batch header log
	BatchHeader* current_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode->offsets[broker_id].batch_headers_offset);
	if (!current_batch_header) {
		LOG(ERROR) << "Scanner [Broker " << broker_id << "]: Failed to calculate batch header start address.";
		return;
	}

	size_t logical_offset = 0;
	// client -> <batch_seq, header*>
	absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>> skipped_batches; 

	while (!stop_threads_) {
		// 1. Check for new Batch Header (Use memory_order_acquire for visibility)
		volatile size_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;

		// No new batch written.
		if (num_msg_check == 0 || current_batch_header->log_idx == 0) {
			if(!ProcessSkipped(topic, skipped_batches)){
				std::this_thread::yield();
			}
			continue;
		}

		// 2. Check if this batch is the next expected one for the client
		BatchHeader* header_to_process = current_batch_header;
		size_t client_id = current_batch_header->client_id;
		size_t batch_seq = current_batch_header->batch_seq;
		bool ready_to_order = false;
		std::atomic<size_t>* expected_seq_atomic_ptr = nullptr;

		// Give logical_offset to the first message in the batch header
		// Writing it to message header can be overwritten by network thread
		// Rest will be given by GlobalOrdererWorker
		current_batch_header->start_logical_offset = logical_offset;
		logical_offset += current_batch_header->num_msg;

		// Check if client exists in the map
		auto map_it = next_expected_batch_seq_.find(client_id);
		// New client: Need to insert safely
		if (map_it == next_expected_batch_seq_.end()) {
			absl::MutexLock lock(&client_seq_map_mutex_);
			// Double check after locking
			map_it = next_expected_batch_seq_.find(client_id);
			if (map_it == next_expected_batch_seq_.end()) {
				// Initialize expectation. Assume first batch is 0.
				if (batch_seq == 0) {
					auto new_atomic_ptr = std::make_unique<std::atomic<size_t>>(1); // Initialize to expect 1
					expected_seq_atomic_ptr = new_atomic_ptr.get();
					// Emplace the key and move the unique_ptr into the map
					next_expected_batch_seq_.emplace(client_id, std::move(new_atomic_ptr));
					ready_to_order = true;
				} else {
					// First batch seen is not 0 - skip for now
					skipped_batches[client_id][batch_seq] = header_to_process;
				}
			} else {
				// Another thread added it concurrently, fall through to normal check
				expected_seq_atomic_ptr = map_it->second.get();
			}
		} else {
			// Get next batch sequence to order
			expected_seq_atomic_ptr = map_it->second.get();
		}

		// Proceed only if we have a valid pointer to the atomic counter
		if (expected_seq_atomic_ptr) {
			if (!ready_to_order) { // Only check CAS if not already deemed ready (new client case)
				size_t expected_seq = expected_seq_atomic_ptr->load(std::memory_order_acquire);
				if (batch_seq == expected_seq) {
					// Attempt to atomically increment the expected sequence
					// TODO(Jae) have this in outer if
					if (expected_seq_atomic_ptr->compare_exchange_strong(expected_seq, expected_seq + 1, std::memory_order_acq_rel)) {
						ready_to_order = true;
					} else {
						// CAS failed - another scanner for the *same client* (if possible?) or spurious fail. Skip.
						LOG(WARNING) << "Scanner [Broker " << broker_id << "]: CAS failed for client " << client_id << " batch " << batch_seq;
						skipped_batches[client_id][batch_seq] = header_to_process;
					}
				} else if (batch_seq > expected_seq) {
					// Out of order batch
					skipped_batches[client_id][batch_seq] = header_to_process;
				} else {
					// Duplicate or older batch - ignore or log error
					LOG(WARNING) << "Scanner [Broker " << broker_id << "]: Duplicate/old batch seq " 
						<< batch_seq << " detected from client " << client_id << " (expected " << expected_seq << ")";
				}
			}
		}

		// 3. Queue if ready
		if (ready_to_order) {
			AssignOrder(topic, header_to_process);
			ProcessSkipped(topic, skipped_batches);
		}

		// 4. Advance to next batch header (handle segment/log wrap around)
		current_batch_header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader)
				);
	} // end of main while loop
}

// Helper to process skipped batches for a specific client after a batch was enqueued
bool CXLManager::ProcessSkipped(std::array<char, TOPIC_NAME_SIZE>& topic,
		absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>>& skipped_batches){

	bool ret = false;
	auto client_skipped_it = skipped_batches.begin();
	while (client_skipped_it != skipped_batches.end()){
		size_t client_id = client_skipped_it->first;
		auto& client_skipped_map = client_skipped_it->second; // Ref to btree_map
		auto map_it = next_expected_batch_seq_.find(client_id);
		if(map_it == next_expected_batch_seq_.end()){
			return ret;
		}

		std::atomic<size_t>* expected_seq_atomic_ptr = map_it->second.get();
		std::atomic<size_t>& expected_seq_atomic = *expected_seq_atomic_ptr;

		bool batch_processed;
		do {
			batch_processed = false;
			size_t expected_seq = expected_seq_atomic.load(std::memory_order_acquire);
			auto batch_it = client_skipped_map.find(expected_seq); // Find in btree_map

			if (batch_it != client_skipped_map.end()) { // Check if found
																									// Found the next expected batch in the skipped list
																									// Attempt to atomically increment the expected sequence *again*
				if (expected_seq_atomic.compare_exchange_strong(expected_seq, expected_seq + 1, std::memory_order_acq_rel)) {
					BatchHeader* batch_header = batch_it->second;
					client_skipped_map.erase(batch_it); // Remove from skipped map *after* CAS succeeds
					AssignOrder(topic, batch_header);

					batch_processed = true; // Indicate we should check again for the *next* sequence
					ret = true;
				} else {
					// CAS failed - stop checking for now. Another thread might be processing.
					break;
				}
			} else {
				// The next expected batch is not in the skipped map, stop checking.
				break;
			}
		} while (batch_processed && !client_skipped_map.empty()); // Keep checking if we processed one

		if (client_skipped_map.empty()) {
			auto next_it = std::next(client_skipped_it);
			skipped_batches.erase(client_skipped_it);
			client_skipped_it = next_it;
		}else{
			++client_skipped_it;
		}
	}
	return ret;
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

void CXLManager::AssignOrder(std::array<char, TOPIC_NAME_SIZE>& topic, BatchHeader *batch_to_order) {
	TInode* tinode = GetTInode(topic.data());
	static std::atomic<size_t> global_seq = 0;
	int broker = batch_to_order->broker_id;

	// **Assign Global Order using Atomic fetch_add**
	size_t num_messages = batch_to_order->num_msg;
	if (num_messages == 0) {
		LOG(WARNING) << "!!!! Orderer: Dequeued batch with zero messages. Skipping !!!";
		return;
	}

	// Get pointer to the first message
	MessageHeader* msg_header = reinterpret_cast<MessageHeader*>(
			batch_to_order->log_idx + reinterpret_cast<uint8_t*>(cxl_addr_)
			);
	if (!msg_header) {
		LOG(ERROR) << "Orderer: Failed to calculate message address for logical offset " << batch_to_order->log_idx;
		return;
	}
	MessageHeader *last_msg_of_batch = nullptr;
	auto tracker_it = trackers_.find(broker);
	if(tracker_it == trackers_.end()){
		LOG(ERROR) << "Trackers for broker:" << broker << " not instantiated which cannot happen";
		return;
	}
	auto tracker = tracker_it->second.get();
	size_t seq = global_seq.fetch_add(batch_to_order->num_msg);

	size_t logical_offset = batch_to_order->start_logical_offset;
	size_t batch_start_offset = logical_offset;
	size_t contiguous_end_offset = tracker->InsertAndGetSequentiallyOrdered(batch_start_offset, batch_to_order->num_msg);
	bool update_tinode = (contiguous_end_offset >= batch_start_offset + batch_to_order->num_msg);

	for (size_t i = 0; i < num_messages; ++i) {
		// 1. Wait for message completion (minimize this wait)
		// Use volatile read in the loop condition
		volatile uint32_t* complete_ptr = &(reinterpret_cast<volatile MessageHeader*>(msg_header)->complete);
		while (*complete_ptr == 0) {
			if (stop_threads_) return;
			std::this_thread::yield();
		}

		// 2. Read paddedSize AFTER completion check TODO(Jae) Get rid of this if error message is never shown
		size_t current_padded_size = msg_header->paddedSize;
		if (current_padded_size == 0) {
			LOG(ERROR) << "Orderer: Message completed but paddedSize is 0! Batch client="
				<< batch_to_order->client_id << " seq=" << batch_to_order->batch_seq
				<< " msg_idx=" << i << ". Skipping rest of batch.";
			break; // Stop processing this batch
		}

		// 3. Assign order and set next pointer difference
		msg_header->logical_offset = logical_offset;
		logical_offset++;
		msg_header->total_order = seq;
		seq++;
		msg_header->next_msg_diff = current_padded_size;

		// 4. Make total_order and next_msg_diff visible before readers might use them
		//std::atomic_thread_fence(std::memory_order_release);

		// For latency, update per message. Can do this batch granularity after the loop
		if(update_tinode){
			UpdateTInodeOrderandWritten(topic.data(), tinode, broker, msg_header->logical_offset, (uint8_t*)msg_header - (uint8_t*)cxl_addr_);
		}
		last_msg_of_batch = msg_header;
		msg_header = reinterpret_cast<MessageHeader*>(
				reinterpret_cast<uint8_t*>(msg_header) + current_padded_size
				);
	} // End message loop


	// *** ALWAYS store the physical offset mapping for the last message of THIS batch ***
	tracker->StorePhysicalOffset(last_msg_of_batch->logical_offset,
			(uint8_t*)last_msg_of_batch - (uint8_t*)cxl_addr_);

	if(contiguous_end_offset > last_msg_of_batch->logical_offset + 1){ // contiguous_end_offset can be 0 so -1 has edge cases.
		size_t lookup_offset = contiguous_end_offset - 1; // The logical offset of the absolute last message in the contiguous range from 0
		size_t addr = tracker->GetPhysicalOffset(lookup_offset);

		if (addr == 0) {
			LOG(ERROR) << "end_offset_logical_to_physical_ still not found for CONTIGUOUS END offset:" << lookup_offset
				<< ". ContiguousEnd=" << contiguous_end_offset
				<< ". BatchClient=" << batch_to_order->client_id
				<< " BatchSeq=" << batch_to_order->batch_seq
				<< " BatchBroker=" << broker
				<< " LastMsgLogicalInBatch=" << last_msg_of_batch->logical_offset
				<< ". THIS SHOULD NOT HAPPEN if lookup_offset is from the current batch OR locking is correct.";
		}else{
			UpdateTInodeOrderandWritten(topic.data(), tinode, broker, lookup_offset, addr);
		}
	}
}

void CXLManager::StartScalogLocalSequencer(std::string topic_str) {
	// int unique_port = SCALOG_SEQ_PORT + scalog_local_sequencer_port_offset_.fetch_add(1);
	auto scalog_local_sequencer = std::make_unique<Scalog::ScalogLocalSequencer>(this, broker_id_, cxl_addr_, topic_str);
	scalog_local_sequencer->SendLocalCut(topic_str);
}


} // End of namespace Embarcadero
