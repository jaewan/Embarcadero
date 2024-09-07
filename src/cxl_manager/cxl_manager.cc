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

static inline void* allocate_shm(int broker_id, CXL_Type cxl_type){
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
		if (ftruncate(cxl_fd, CXL_SIZE) == -1) {
			LOG(ERROR) << "ftruncate failed";
			close(cxl_fd);
			return nullptr;
		}
	}
	addr = mmap(NULL, CXL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, cxl_fd, 0);
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
		if (mbind(addr, CXL_SIZE, MPOL_BIND, bitmask->maskp, bitmask->size, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
			LOG(ERROR)<< "mbind failed";
			numa_free_nodemask(bitmask);
			munmap(addr, CXL_SIZE);
			return nullptr;
		}
		VLOG(3) << "Binded the memory to CXL";

		numa_free_nodemask(bitmask);
	}

	if(broker_id == 0){
		//memset(addr, 0, (1UL<<34));
		memset(addr, 0, CXL_SIZE);
		VLOG(3) << "Cleared CXL:" << CXL_SIZE;
	}
	return addr;
}

CXLManager::CXLManager(int broker_id, CXL_Type cxl_type, std::string head_ip):
	head_ip_(head_ip),
	broker_id_(broker_id){
	size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	// Initialize CXL
	cxl_addr_ = allocate_shm(broker_id, cxl_type);
	if(cxl_addr_ == nullptr){
		return;
	}

	// Initialize CXL memory regions
	size_t TINode_Region_size = sizeof(TInode) * MAX_TOPIC_SIZE;
	size_t padding = TINode_Region_size - ((TINode_Region_size/cacheline_size) * cacheline_size);
	TINode_Region_size += padding;
	size_t Bitmap_Region_size = cacheline_size * MAX_TOPIC_SIZE;
	size_t Segment_Region_size = (CXL_SIZE - TINode_Region_size - Bitmap_Region_size)/NUM_MAX_BROKERS;
	padding = Segment_Region_size%cacheline_size;
	Segment_Region_size -= padding;

	bitmap_ = (uint8_t*)cxl_addr_ + TINode_Region_size;
	segments_ = (uint8_t*)bitmap_ + Bitmap_Region_size + ((broker_id_)*Segment_Region_size);


	LOG(INFO) << "\t[CXLManager]: \t\tConstructed";
	return;
}

CXLManager::~CXLManager(){
	stop_threads_ = true;
	for(std::thread& thread : sequencerThreads_){
		if(thread.joinable()){
			thread.join();
		}
	}

	if (munmap(cxl_addr_, CXL_SIZE) < 0)
		LOG(ERROR) << "Unmapping CXL error";

	LOG(INFO) << "[CXLManager]: \t\tDestructed";
}

void* CXLManager::GetCXLBuffer(PublishRequest &req){
	return topic_manager_->GetCXLBuffer(req);
}

// This function returns TInode without inspecting if the topic exists
void* CXLManager::GetTInode(const char* topic){
	// Convert topic to tinode address
	//static const std::hash<std::string> topic_to_idx;
	//int TInode_idx = topic_to_idx(topic) % MAX_TOPIC_SIZE;
	int TInode_idx = atoi(topic) % MAX_TOPIC_SIZE;
	return ((uint8_t*)cxl_addr_ + (TInode_idx * sizeof(struct TInode)));
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
	switch(sequencerType){
		case KAFKA: // Kafka is just a way to not run CombinerThread, not actual sequencer
		case EMBARCADERO:
			if (order == 1)
				sequencerThreads_.emplace_back(&CXLManager::Sequencer1, this, topic);
			else if (order == 2)
				sequencerThreads_.emplace_back(&CXLManager::Sequencer2, this, topic);
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
				//TODO(Erika) fill this
			}
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
			msg_to_order[broker_id] = ((struct Embarcadero::MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id].log_offset));
		}
	}
}

void CXLManager::Sequencer1(char* topic){
	struct TInode *tinode = (struct TInode *)GetTInode(topic);
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS];
	absl::btree_set<int> registered_brokers;
	static size_t seq = 0;

	GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
	auto last_updated = std::chrono::steady_clock::now();

	while(!stop_threads_){
		bool yield = true;
		for(auto broker : registered_brokers){
			size_t msg_logical_off = msg_to_order[broker]->logical_offset;
			//This ensures the message is Combined (all the other fields are filled)
			if(msg_logical_off != (size_t)-1 && (int)msg_logical_off <= tinode->offsets[broker].written && msg_to_order[broker]->next_msg_diff != 0){
				msg_to_order[broker]->total_order = seq;
				seq++;
				tinode->offsets[broker].ordered = msg_logical_off;
				tinode->offsets[broker].ordered_offset = (uint8_t*)msg_to_order[broker] - (uint8_t*)cxl_addr_;
				msg_to_order[broker] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker] + msg_to_order[broker]->next_msg_diff);
				yield = false;
			}
		}
		if(yield){
			GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			last_updated = std::chrono::steady_clock::now();
			std::this_thread::yield();
		}else if(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
					- last_updated).count() >= HEARTBEAT_INTERVAL){
			GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			last_updated = std::chrono::steady_clock::now();
		}
	}
}

void CXLManager::Sequencer2(char* topic){
	struct TInode *tinode = (struct TInode *)GetTInode(topic);
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
			if(msg_to_order[broker]->complete == 1 && msg_logical_off != (size_t)-1 && (int)msg_logical_off <= tinode->offsets[broker].written){
				int client_id;
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
							if(client_order == last_ordered[client] + 1){
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
								tinode->offsets[b].ordered = exportable_msg->logical_offset;
								tinode->offsets[b].ordered_offset = (uint8_t*)exportable_msg - (uint8_t*)cxl_addr_;
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

void CXLManager::StartScalogLocalSequencer(std::string topic_str) {
	VLOG(3) << "Initializing scalog sequencer service for topic: " << topic_str;

	int unique_port = SCALOG_SEQ_PORT + scalog_sequencer_service_port_offset_.fetch_add(1);
	std::string scalog_seq_address = head_ip_ + ":" + std::to_string(unique_port);
	scalog_sequencer_service_ = std::make_unique<ScalogSequencerService>(this, broker_id_, cxl_addr_, scalog_seq_address);

	/// New thread for the local sequencer is required in the head so we can also run the grpc server
	if (broker_id_ == 0) {
		VLOG(3) << "Starting scalog sequencer in head for topic: " << topic_str << std::endl;
		std::thread local_sequencer_thread([this, topic_str]() {
			while (!stop_threads_) {
				scalog_sequencer_service_->LocalSequencer(topic_str);
			}
		});
		local_sequencer_thread.detach();

		grpc::ServerBuilder builder;
		builder.AddListeningPort(scalog_seq_address, grpc::InsecureServerCredentials());
		builder.RegisterService(scalog_sequencer_service_.get());
		scalog_server_ = builder.BuildAndStart();
		VLOG(3) << "Scalog sequencer listening on " << scalog_seq_address;
		scalog_server_->Wait();
	} else {
		VLOG(3) << "Starting scalog sequencer in broker " << broker_id_ << " for topic: " << topic_str << " and address: " << scalog_seq_address;
		while (!stop_threads_) {
			scalog_sequencer_service_->LocalSequencer(topic_str);
		}
	}
}

//TODO (tony) priority 2 (failure test)  make the scalog code failure prone.
//Current logic proceeds epoch with all brokers at the same pace. 
//If a broker fails, the entire cluster is stuck. If a failure is detected from the heartbeat, GetRegisteredBroker will return the alive brokers
//after heartbeat_interval (failure is detected), if there is a change in the cluster, only proceed with the brokers
ScalogSequencerService::ScalogSequencerService(CXLManager* cxl_manager, int broker_id, void* cxl_addr, std::string scalog_seq_address) :
	cxl_manager_(cxl_manager),
	broker_id_(broker_id),
	cxl_addr_(cxl_addr) {

	/// For now, only the head node will have the global sequencer and send global cuts
	if (broker_id_ == 0) {
		has_global_sequencer_ = true;
		global_epoch_ = 0;
	} else {
		has_global_sequencer_ = false;
		std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(scalog_seq_address, grpc::InsecureChannelCredentials());
    	stub_ = ScalogSequencer::NewStub(channel);
	}

	VLOG(3) << "Finished starting scalog sequencer" << std::endl;
}

void ScalogSequencerService::LocalSequencer(std::string topic_str){
	VLOG(3) << "Calling local sequencer for topic: " << topic_str;
	struct TInode *tinode = (struct TInode *) cxl_manager_->GetTInode(topic_str.c_str());

	VLOG(3) << "Sending local cut: " << tinode->offsets[broker_id_].written;

	auto start_time = std::chrono::high_resolution_clock::now();
	/// Send epoch and tinode->offsets[broker_id_].written to global sequencer
	SendLocalCut(tinode->offsets[broker_id_].written, topic_str.c_str());
	auto end_time = std::chrono::high_resolution_clock::now();

	/// We measure the time it takes to send the local cut
	auto elapsed_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
	VLOG(3) << "Elapsed time: " << elapsed_time_us.count() << " us";
	
	/// In the case where we receive the global cut before the interval has passed, we wait for the remaining time left in the interval
	while(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start_time) 
			< local_cut_interval_){
		std::this_thread::yield();
	}
}

void ScalogSequencerService::SendLocalCut(int local_cut, const char* topic) {
	static int epoch = 0;
	// Bypass grpc call
	if (has_global_sequencer_ == true) {
		// Insert local cut into global cut
		// TODO(tony) manage local cuts per epoch
		/*
		global_cut_[epoch][broker_id_] = local_cut;
		lock()
			insert();
		unlock();

		broadcast glocal cut()
			locj();
		remove();
		unclock();
		*/

		{
			absl::WriterMutexLock lock(&global_cut_mu_);
			global_cut_[epoch][broker_id_] = local_cut;
		}

		VLOG(3) << "Received local cut from broker " << broker_id_ << " with value " << local_cut;

		/// Call local function to receive local cut instead of grpc
		ReceiveLocalCut(epoch, topic, broker_id_);
	} else {
		VLOG(3) << "Sending local cut with grpc for topic: " << topic;

		SendLocalCutRequest request;
		request.set_epoch(epoch);
		request.set_local_cut(local_cut);
		request.set_topic(topic);
		request.set_broker_id(broker_id_);

		SendLocalCutResponse response;
		grpc::ClientContext context;

		std::mutex mu;
		std::condition_variable cv;
		bool done = false;

		/// Callback is called when all followers are ready to receive the global cut
		auto callback = [this, topic, &response, &mu, &cv, &done, epoch](grpc::Status status) {
			if (!status.ok()) {
				LOG(ERROR) << "Error sending local cut: " << status.error_message();
			} else {
				VLOG(3) << "Successfully sent local cut for topic: " << topic;

				// Convert google::protobuf::Map<int64_t, int64_t> to absl::flat_hash_map<int, int>
				{
					absl::WriterMutexLock lock(&global_cut_mu_);
					for (const auto& entry : response.global_cut()) {
						global_cut_[epoch][static_cast<int>(entry.first)] = static_cast<int>(entry.second);
					}
				}

				this->ReceiveGlobalCut(epoch, topic);
			}

			std::lock_guard<std::mutex> lock(mu);
			done = true;

			/// Notify the main thread that the callback has been called
			cv.notify_one();
		};

		// Async call to HandleStartLocalSequencer
		stub_->async()->HandleSendLocalCut(&context, &request, &response, callback);
		//TODO (tony) priority 1 (latency improvement) potential change
		// stub_->HandleSendLocalCut(&context, &request, &response, callback);
		//(tony) check response

		/// Wait until the callback has been called so it doesn't go out of scope
		std::unique_lock<std::mutex> lock(mu);
		cv.wait(lock, [&done] { return done; });
	}
	epoch++;
}

void ScalogSequencerService::ReceiveGlobalCut(int epoch, const char* topic) {
	// Maybe request can hold topic string
	struct TInode *tinode = (struct TInode *) cxl_manager_->GetTInode(topic);

	VLOG(3) << "Received global cut";

	//TODO(Jae) Update Total order

}

grpc::Status ScalogSequencerService::HandleSendLocalCut(grpc::ServerContext* context, const SendLocalCutRequest* request, SendLocalCutResponse* response) {
	VLOG(3) << "Received local cut with grpc";
	
	const char* topic = request->topic().c_str();
	int epoch = request->epoch();
	int local_cut = request->local_cut();
	int broker_id = request->broker_id();

	//TODO(tony) this should be parallel safe. This function can be interrupted while it is updating the glocal_cut_
	{
		absl::WriterMutexLock lock(&global_cut_mu_);
		global_cut_[epoch][broker_id] = local_cut;
	}

	VLOG(3) << "Received local cut from broker " << broker_id << " with value " << local_cut;

	ReceiveLocalCut(epoch, topic, broker_id);

	/// Convert global_cut_ to google::protobuf::Map<int64_t, int64_t>
	auto* mutable_global_cut = response->mutable_global_cut();
	{
		absl::ReaderMutexLock lock(&global_cut_mu_);
		for (const auto& entry : global_cut_[epoch]) {
			(*mutable_global_cut)[static_cast<int64_t>(entry.first)] = static_cast<int64_t>(entry.second);
		}
	}

	VLOG(3) << "Finished receiving local cut" << std::endl;

	return grpc::Status::OK;
}

void ScalogSequencerService::ReceiveLocalCut(int epoch, const char* topic, int broker_id) {
	if (epoch - 1 > global_epoch_) {
		// If the epoch is not the same as the current global epoch, there is an error
		LOG(ERROR) << "Local epoch: " << epoch << " while global epoch: " << global_epoch_;
		exit(1);
	}

	std::unique_lock<std::mutex> lock(mutex_);

	struct TInode *tinode = (struct TInode *) cxl_manager_->GetTInode(topic);
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS];
	absl::btree_set<int> registered_brokers;

	cxl_manager_->GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);

	// local_cuts_count_[epoch]++;
	//TODO(tony)
	// Add checking epoch of each local cut logic here
	//(tony)
	int local_cut_num = 0;
	{
		absl::ReaderMutexLock lock(&global_cut_mu_);
		local_cut_num = global_cut_[epoch].size();
	}

	// auto result = local_cuts_count_.emplace(epoch, std::make_unique<std::atomic<int>>(0));
	// result.first->second->fetch_add(1);

	if (local_cut_num == registered_brokers.size()) {

		std::cout << "We have received all local cuts, calling receive local cut from broker: " << broker_id << std::endl;

		std::cout << "All local cuts for epoch " << epoch << " have been received, sending global cut" << std::endl;

		// Send global cut to own node's local sequencer
		global_epoch_++;
		ReceiveGlobalCut(epoch, topic);

		// waiting_threads_count_ = broker_->GetNumBrokers() - 1;

		/// Notify all waiting grpc threads that the global cut has been received
		cv_.notify_all();

		/// Wait until all threads have been notified before resetting local_cuts_count_
		// reset_cv_.wait(lock, [this]() { return waiting_threads_count_ == 0; });

		/// Safely reset local_cuts_count_ after all threads have been notified and processed
		auto it = global_cut_.find(epoch - 2);
		if (it != global_cut_.end()) {
			// The element exists, so delete it
			std::cout << "Erasing global cut for epoch: " << epoch - 2 << std::endl;
			{
				absl::WriterMutexLock lock(&global_cut_mu_);
				global_cut_.erase(epoch - 2);
			}
		}
		std::cout << "Reset local_cuts_count_ to 0 after notifying all threads" << std::endl;
	} else {
		VLOG(3) << "Calling receive local cut from broker: " << broker_id << std::endl;

		/// If we haven't received all local cuts, the grpc thread must wait until we do to send the correct global cut back to the caller
        cv_.wait(lock, [this, broker_id, epoch, registered_brokers]() {
			int local_cut_num = 0;
			{
				absl::ReaderMutexLock lock(&global_cut_mu_);
				local_cut_num = global_cut_[epoch].size();
			}
			std::cout << "I've been notified that all local cuts have been received from broker: " << broker_id << std::endl;
			std::cout << "Num brokers: " << registered_brokers.size() << std::endl;
			std::cout << "Local cuts count: " << local_cut_num << std::endl;
			if (local_cut_num == registered_brokers.size()){
				return true;
			} else {
				return false;
			}
        });

		// while (local_cuts_count_[epoch]->load() != broker_->GetNumBrokers()) {
		// 	std::this_thread::yield();
		// }

		/// Decrement waiting_threads_count_ after processing the notification
		// if (--waiting_threads_count_ == 0) {
		// 	/// This notifies the waiting thread that we can reset local_cuts_count_ to 0
		// 	reset_cv_.notify_one();
		// }		

		std::cout << "Finished waiting for all local cuts to be received for broker: " << broker_id << std::endl;
	}
}
} // End of namespace Embarcadero
