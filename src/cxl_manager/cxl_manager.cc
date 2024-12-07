#include "cxl_manager.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
		//memset(addr, 0, (1UL<<34));
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
	size_t Segment_Region_size = (cxl_size_ - TINode_Region_size - Bitmap_Region_size)/NUM_MAX_BROKERS;
	padding = Segment_Region_size%cacheline_size;
	Segment_Region_size -= padding;

	bitmap_ = (uint8_t*)cxl_addr_ + TINode_Region_size;
	segments_ = (uint8_t*)bitmap_ + Bitmap_Region_size + ((broker_id_)*Segment_Region_size);


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

    // If this is the head node, terminate the global sequencer
    if (broker_id_ == 0 && scalog_local_sequencer_) {
		LOG(INFO) << "Terminating global sequencer";
        scalog_local_sequencer_->TerminateGlobalSequencer();
    }	

	if (munmap(cxl_addr_, CXL_SIZE) < 0)
		LOG(ERROR) << "Unmapping CXL error";

	VLOG(3) << "[CXLManager]: \t\tDestructed";
}

std::function<void(void*, size_t)> CXLManager::GetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset){
	return topic_manager_->GetCXLBuffer(batch_header, topic, log, segment_header, logical_offset);
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
	std::atomic<int> segment_count{0};
	int offset = segment_count.fetch_add(1, std::memory_order_relaxed);

	return (uint8_t*)segments_ + offset*SEGMENT_SIZE;
}

bool CXLManager::GetMessageAddr(const char* topic, size_t &last_offset,
		void* &last_addr, void* &messages, size_t &messages_size){
	return topic_manager_->GetMessageAddr(topic, last_offset, last_addr, messages, messages_size);
}

void CXLManager::RunSequencer(char topic[TOPIC_NAME_SIZE], int order, SequencerType sequencerType){
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
				LOG(INFO) << "Order 2 for Corfu is right. But check Client library to change order to 0 in corfu as corfu is already ordered at publich";
			}
			break;
		default:
			LOG(ERROR) << "Unknown sequencer:" << sequencerType;
			break;
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

inline void CXLManager::UpdateTinodeOrder(char *topic, TInode* tinode, int broker, size_t msg_logical_off, size_t ordered_offset){
	if(tinode->replicate_tinode){
		struct TInode *replica_tinode = GetReplicaTInode(topic);
		replica_tinode->offsets[broker].ordered = msg_logical_off;
		replica_tinode->offsets[broker].ordered_offset = ordered_offset;
	}

	tinode->offsets[broker].ordered = msg_logical_off;
	tinode->offsets[broker].ordered_offset = ordered_offset;
}

void CXLManager::Sequencer1(std::array<char, TOPIC_NAME_SIZE> topic){
	struct TInode *tinode = GetTInode(topic.data());
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS];
	absl::btree_set<int> registered_brokers;
	static size_t seq = 0;

	GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
	//auto last_updated = std::chrono::steady_clock::now();

	while(!stop_threads_){
		//bool yield = true;
		for(auto broker : registered_brokers){
			size_t msg_logical_off = msg_to_order[broker]->logical_offset;
			size_t written = tinode->offsets[broker].written;
			if(written == (size_t)-1){
				continue;
			}
			while(!stop_threads_ && msg_logical_off <= written && msg_to_order[broker]->next_msg_diff != 0 
						&& msg_to_order[broker]->logical_offset != (size_t)-1){
				msg_to_order[broker]->total_order = seq;
				seq++;
				//std::atomic_thread_fence(std::memory_order_release);
				UpdateTinodeOrder(topic.data(), tinode, broker , msg_logical_off, (uint8_t*)msg_to_order[broker] - (uint8_t*)cxl_addr_);
				msg_to_order[broker] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker] + msg_to_order[broker]->next_msg_diff);
				msg_logical_off++;
				//yield = false;
			}
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
			//VLOG(3) << batch_seq << ") Broker:" << broker <<  " Ordering:" << msg_logical_off << "~" << n;
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

void CXLManager::StartScalogLocalSequencer(std::string topic_str) {
	// int unique_port = SCALOG_SEQ_PORT + scalog_local_sequencer_port_offset_.fetch_add(1);
	int unique_port = SCALOG_SEQ_PORT;
	std::string scalog_seq_address = scalog_global_sequencer_ip_ + ":" + std::to_string(unique_port);
	scalog_local_sequencer_ = std::make_unique<ScalogLocalSequencer>(this, broker_id_, cxl_addr_, scalog_seq_address);

	scalog_local_sequencer_->SendLocalCut(topic_str);
}

//TODO (tony) priority 2 (failure test)  make the scalog code failure prone.
//Current logic proceeds epoch with all brokers at the same pace. 
//If a broker fails, the entire cluster is stuck. If a failure is detected from the heartbeat, GetRegisteredBroker will return the alive brokers
//after heartbeat_interval (failure is detected), if there is a change in the cluster, only proceed with the brokers
ScalogLocalSequencer::ScalogLocalSequencer(CXLManager* cxl_manager, int broker_id, void* cxl_addr, std::string scalog_seq_address) :
	cxl_manager_(cxl_manager),
	broker_id_(broker_id),
	cxl_addr_(cxl_addr) {

	std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(scalog_seq_address, grpc::InsecureChannelCredentials());
	stub_ = ScalogSequencer::NewStub(channel);
}

void ScalogLocalSequencer::TerminateGlobalSequencer() {
	TerminateGlobalSequencerRequest request;
	TerminateGlobalSequencerResponse response;
	grpc::ClientContext context;

	grpc::Status status = stub_->HandleTerminateGlobalSequencer(&context, request, &response);
	if (!status.ok()) {
		LOG(ERROR) << "Error terminating global sequencer: " << status.error_message();
	}
}

void ScalogLocalSequencer::SendLocalCut(std::string topic_str){
	static char topic[TOPIC_NAME_SIZE];
	memcpy(topic, topic_str.data(), topic_str.size());
	struct TInode *tinode = cxl_manager_->GetTInode(topic);

	grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>> stream(
        stub_->HandleSendLocalCut(&context));

	// Spawn a thread to receive global cuts, passing the stream by reference
    std::thread receive_global_cut(&ScalogLocalSequencer::ReceiveGlobalCut, this, std::ref(stream), topic_str);

	while (!cxl_manager_->GetStopThreads()) {
		/// Send epoch and tinode->offsets[broker_id_].written to global sequencer
		int local_cut = tinode->offsets[broker_id_].written;

		LocalCut request;
		request.set_local_cut(local_cut);
		request.set_topic(topic);
		request.set_broker_id(broker_id_);
		request.set_epoch(local_epoch_);

		// Send the LocalCut message to the server
		if (!stream->Write(request)) {
			std::cerr << "Error writing LocalCut to the server" << std::endl;
		}

		// Increment the epoch
		local_epoch_++;

		// Sleep until interval passes to send next local cut
		std::this_thread::sleep_for(local_cut_interval_);
	}

	stream->WritesDone();
	stop_reading_from_stream_ = true;
	receive_global_cut.join();
}

void ScalogLocalSequencer::ReceiveGlobalCut(std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>>& stream, std::string topic_str) {
	static char topic[TOPIC_NAME_SIZE];
	memcpy(topic, topic_str.data(), topic_str.size());

	while (!stop_reading_from_stream_) {
		GlobalCut global_cut;
		if (stream->Read(&global_cut)) {
			// Convert google::protobuf::Map<int64_t, int64_t> to absl::flat_hash_map<int, int>
			for (const auto& entry : global_cut.global_cut()) {
				global_cut_[static_cast<int>(entry.first)] = static_cast<int>(entry.second);
			}

			this->cxl_manager_->ScalogSequencer(topic, global_cut_);
		}
	}

    grpc::Status status = stream->Finish();
    if (!status.ok()) {
        std::cerr << "Stream finished with error: " << status.error_message() << std::endl;
    }
}

void CXLManager::ScalogSequencer(const char* topic, absl::btree_map<int, int> &global_cut) {
	static char topic_char[TOPIC_NAME_SIZE];
	static size_t seq = 0;
	static TInode *tinode = nullptr; 
	static MessageHeader* msg_to_order = nullptr;
	memcpy(topic_char, topic, TOPIC_NAME_SIZE);
	if(tinode == nullptr){
		tinode = GetTInode(topic);
		msg_to_order = ((MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id_].log_offset));
	}

	for(auto &cut : global_cut){
		if(cut.first == broker_id_){
			for(int i = 0; i<cut.second; i++){
				msg_to_order->total_order = seq;
				std::atomic_thread_fence(std::memory_order_release);
				/*
				tinode->offsets[broker_id_].ordered = msg_to_order->logical_offset;
				tinode->offsets[broker_id_].ordered_offset = (uint8_t*)msg_to_order - (uint8_t*)cxl_addr_;
				*/
				UpdateTinodeOrder(topic_char, tinode, broker_id_, msg_to_order->logical_offset, (uint8_t*)msg_to_order - (uint8_t*)cxl_addr_);
				msg_to_order = (MessageHeader*)((uint8_t*)msg_to_order + msg_to_order->next_msg_diff);
				seq++;
			}
		}else{
			seq += cut.second;
		}
	}
}
} // End of namespace Embarcadero
