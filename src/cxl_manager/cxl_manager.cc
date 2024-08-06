#include "cxl_manager.h"
#include <iostream>
#include <future>
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
		//memset(addr, 0, (1UL<<35));
		memset(addr, 0, CXL_SIZE);
		VLOG(3) << "Cleared CXL:" << CXL_SIZE;
	}
	return addr;
}

CXLManager::CXLManager(size_t queueCapacity, int broker_id, CXL_Type cxl_type, std::string head_address, int num_io_threads):
	requestQueue_(queueCapacity),
	broker_id_(broker_id),
	head_address_(head_address),
	num_io_threads_(num_io_threads){

	size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	// Initialize CXL
	cxl_addr_ = allocate_shm(broker_id, cxl_type);
	if(cxl_addr_ == nullptr){
		return;
	}
	// Create CXL I/O threads
	for (int i=0; i< num_io_threads_; i++)
		threads_.emplace_back(&CXLManager::CXLIOThread, this);

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

	// Wait untill al IO threads are up
	while(thread_count_.load() != num_io_threads_){}

	LOG(INFO) << "[CXLManager]: \t\tConstructed";
	return;
}

CXLManager::~CXLManager(){
	std::optional<struct PublishRequest> sentinel = std::nullopt;
	stop_threads_ = true;
	for (int i=0; i< num_io_threads_; i++) {
		requestQueue_.blockingWrite(sentinel);
	}

	if (munmap(cxl_addr_, CXL_SIZE) < 0)
		LOG(ERROR) << "Unmapping CXL error";

	for(std::thread& thread : threads_){
		if(thread.joinable()){
			thread.join();
		}
	}

	for(std::thread& thread : sequencerThreads_){
		if(thread.joinable()){
			thread.join();
		}
	}

	LOG(INFO) << "[CXLManager]: \t\tDestructed";
}


void CXLManager::CXLIOThread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct PublishRequest> optReq;
	while(!stop_threads_){
		requestQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		struct PublishRequest &req = optReq.value();

		// Actual IO to the CXL
		topic_manager_->PublishToCXL(req);//req.topic, req.payload_address, req.size);

		// Post I/O work (as disk I/O depend on the same payload)
		int counter = req.counter->fetch_sub(1);
		if( counter == 1){
			mi_free(req.counter);
			mi_free(req.payload_address);
		}else if(req.acknowledge){
			struct NetworkRequest ackReq;
			ackReq.client_socket = req.client_socket;
			network_manager_->EnqueueRequest(ackReq);
		}
	}
}

// This function returns TInode without inspecting if the topic exists
void* CXLManager::GetTInode(const char* topic){
	// Convert topic to tinode address
	//static const std::hash<std::string> topic_to_idx;
	//int TInode_idx = topic_to_idx(topic) % MAX_TOPIC_SIZE;
	int TInode_idx = atoi(topic) % MAX_TOPIC_SIZE;
	return ((uint8_t*)cxl_addr_ + (TInode_idx * sizeof(struct TInode)));
}

void CXLManager::EnqueueRequest(struct PublishRequest req){
	requestQueue_.blockingWrite(req);
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
		case Embarcadero:
			if (order == 1)
				sequencerThreads_.emplace_back(&CXLManager::Sequencer1, this, topic);
			else if (order == 2)
				sequencerThreads_.emplace_back(&CXLManager::Sequencer2, this, topic);
			break;
		case Scalog:
			if (order == 1){
				// std::cout << "Starting scalog sequencer in head" << std::endl;

				// // Set scalog_has_global_sequencer_ to true
				// scalog_has_global_sequencer_[topic] = true;

				// scalog_received_global_seq_[topic] = false;

				// scalog_received_gobal_seq_after_interval_[topic] = false;

				// scalog_local_epoch_[topic] = 0;

				// // Start scalog_io_service_thread_
				// scalog_io_service_thread_ = std::make_unique<std::thread>([this] {
				// 	// Keep io_service_ alive.
				// 	boost::asio::io_service::work io_service_work_(scalog_io_service_);
				// 	scalog_io_service_.run();
				// });

				// scalog_io_service_.dispatch([this, topic] {
				// 	std::cout << "Sending local cuts for topic " << topic << " to global sequencer" << std::endl;
				// 	ScalogLocalSequencer(topic);
				// });
			} else if (order == 2)
				LOG(ERROR) << "Order is set 2 at scalog";
			break;
		case Corfu:
			if (order == 1)
				LOG(ERROR) << "Order is set 1 at corfu";
			else if (order == 2){
				//TODO(Erika) fill this
			}
			break;
	}
}

void CXLManager::StartScalogLocalSequencer(std::string topic_str) {

	std::cout << "Initializing scalog sequencer service for topic: " << topic_str << std::endl;

	const char* topic = topic_str.c_str();

	scalog_sequencer_service_ = std::make_unique<ScalogSequencerService>(this, broker_id_, topic, broker_, cxl_addr_);
	ServerBuilder builder;
	builder.AddListeningPort(head_address_, grpc::InsecureServerCredentials());
	builder.RegisterService(scalog_sequencer_service_.get());
	scalog_server_ = builder.BuildAndStart();
	scalog_server_->Wait();
}

ScalogSequencerService::ScalogSequencerService(CXLManager* cxl_manager, int broker_id, const char* topic, HeartBeatManager* broker, void* cxl_addr) :
	cxl_manager_(cxl_manager),
	broker_id_(broker_id),
	timer_(io_service_),
	broker_(broker),
	cxl_addr_(cxl_addr) {

	if (broker_id_ == 0) {
		std::cout << "Starting scalog sequencer in head for topic: " << topic << std::endl;
		has_global_sequencer_[topic] = true;
	} else {
		has_global_sequencer_[topic] = false;
	}

	received_global_seq_[topic] = false;

	received_gobal_seq_after_interval_[topic] = false;

	local_epoch_[topic] = 0;

	// Start scalog_io_service_thread_
	io_service_thread_ = std::make_unique<std::thread>([this] {
		// Keep io_service_ alive.
		boost::asio::io_service::work io_service_work_(io_service_);
		io_service_.run();
	});

	std::string topic_str(topic);
	io_service_.dispatch([this, topic_str] {
		std::cout << "Sending local cuts for topic " << topic_str << " to global sequencer" << std::endl;
		LocalSequencer(topic_str.c_str());
	});
}

void ScalogSequencerService::LocalSequencer(const char* topic){
	struct TInode *tinode = (struct TInode *) cxl_manager_->GetTInode(topic);
	global_cut_[topic] = std::vector<int>(NUM_MAX_BROKERS, 0);

	// Send written offset variable to global sequencer every 5 ms iff global cut has been received
	if (local_epoch_[topic] == 0) {
		// This is the first local cut to be sent, don't need to wait for global cut
		std::cout << "Sending first local cut: " << tinode->offsets[broker_id_].written << std::endl;

		// send epoch and tinode->offsets[broker_id_].written to global sequencer
		SendLocalCut(local_epoch_[topic], tinode->offsets[broker_id_].written, topic);

		local_epoch_[topic]++;
	} else if (local_epoch_[topic] > 0 && received_global_seq_[topic]) {

		std::cout << "Received global cut, so sending local cut again" << std::endl;

		received_global_seq_[topic] = false;

		std::cout << "Set received global seq to false" << std::endl;

		std::cout << "Sending local cut: " << tinode->offsets[broker_id_].written << std::endl;

		/// Send epoch and tinode->offsets[broker_id_].written to global sequencer
		SendLocalCut(local_epoch_[topic], tinode->offsets[broker_id_].written, topic);

		local_epoch_[topic]++;
	} else if (local_epoch_[topic] > 0 && !received_global_seq_[topic]) {

		std::cout << "Global cut has not been received after the 5 ms interval" << std::endl;

		// If global cut hasn't been received, wait until it receives next global cut then immediately send local cut in ScalogReceiveGlobalCut
		received_gobal_seq_after_interval_[topic] = true;
	} else {
		LOG(ERROR) << "Epoch is negative in local sequencer";
		exit(1);
	}

	std::string topic_str(topic);
	timer_.expires_from_now(
		boost::posix_time::milliseconds(1000));
	timer_.async_wait([this, topic_str](auto) { 
			LocalSequencer(topic_str.c_str()); 
	});
}

// Helper function that allows a local scalog sequencer to send their local cut
void ScalogSequencerService::SendLocalCut(int epoch, int written, const char* topic) {
	if (has_global_sequencer_[topic] == true) {

		std::cout << "Detected global sequencer on this machine, so sending the local cut without grpc" << std::endl;

		std::async(std::launch::async, &ScalogSequencerService::ReceiveLocalCut, this, epoch, written, topic, broker_id_);
		// ScalogReceiveLocalCut(epoch, written, topic, broker_id_);

		std::cout << "Asynchronously sent local cut" << std::endl;
	} else {

		std::cout << "Sending local cut with grpc for topic: " << topic << std::endl;

		SendLocalCutRequest request;
		request.set_epoch(epoch);
		request.set_local_cut(written);
		request.set_topic(topic);
		request.set_broker_id(broker_id_);

		SendLocalCutResponse response;
		grpc::ClientContext context;

		auto callback = [](grpc::Status status) {
			if (!status.ok()) {
				std::cout << "Error sending local cut: " << status.error_message() << std::endl;
			}
		};

		// Async call to HandleStartLocalSequencer
		broker_->GetScalogStub()->async()->HandleSendLocalCut(&context, &request, &response, callback);
	}
}

std::unique_ptr<ScalogSequencer::Stub> ScalogSequencerService::GetRpcClient(std::string peer_url) {
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(peer_url, grpc::InsecureChannelCredentials());
    return ScalogSequencer::NewStub(channel);
}

void ScalogSequencerService::ReceiveLocalCut(int epoch, int local_cut, const char* topic, int broker_id) {
	if (epoch != global_epoch_[topic]) {
		// If the epoch is not the same as the current global epoch, there is an error
		LOG(ERROR) << "Local cut from local sequencer was sent too early, global sequencer has not yet sent the global cut";
		exit(1);
	}

	std::cout << "Received local cut from broker " << broker_id << " with value " << local_cut << std::endl;

	// Insert local cut into global cut
	global_cut_[topic][broker_id] = local_cut;

	// increment local_cuts_count_
	local_cuts_count_[topic]++;

	if (local_cuts_count_[topic] == broker_->GetNumBrokers()) {

		std::cout << "All local cuts for epoch " << epoch << " have been received, sending global cut" << std::endl;

		// Send global cut to own node's local sequencer
		std::async(std::launch::async, &ScalogSequencerService::ReceiveGlobalCut, this, global_cut_[topic], topic);
		// ScalogReceiveGlobalCut(scalog_global_cut_[topic], topic);

		// Iterate through broker list and call async grpc to send global cut
		for (auto const& peer : broker_->GetPeerBrokers()) {

			std::string peer_url = peer.second.network_mgr_addr;
			auto rpc_client = GetRpcClient(peer_url);

			std::cout << "Sending global cut to " << peer_url << std::endl;

			SendGlobalCutRequest request;
			for (int value : global_cut_[topic]) {
				request.add_global_cut(value);
			}

			SendGlobalCutResponse response;
			grpc::ClientContext context;

			auto callback = [](grpc::Status status) {
				if (!status.ok()) {
					std::cout << "Error sending global cut request" << std::endl;
				}
			};

			rpc_client->async()->HandleSendGlobalCut(&context, &request, &response, callback);
		}

		local_cuts_count_[topic] = 0;
		global_epoch_[topic]++;
	}
}

grpc::Status ScalogSequencerService::HandleSendGlobalCut(grpc::ServerContext* context, const SendGlobalCutRequest* request, SendGlobalCutResponse* response) {
    std::vector<int> global_cut(request->global_cut().begin(), request->global_cut().end());
    const char* topic = request->topic().c_str();

	// Maybe request can hold topic string
	struct TInode *tinode = (struct TInode *) cxl_manager_->GetTInode(topic);

	std::cout << "Received global cut with grpc" << std::endl;

	UpdateTotalOrdering(global_cut, tinode);

	received_global_seq_[topic] = true;

	// ScalogSendLocalCut(scalog_local_epoch_[topic], tinode->offsets[broker_id_].written, topic);

	if (received_gobal_seq_after_interval_[topic]) {

		std::cout << "Global cut was received after 5 ms interval so canceling timer" << std::endl;

		received_gobal_seq_after_interval_[topic] = false;

		// Cancel timer so ScalogLocalSequencer runs immediately
		timer_.cancel();
	}

	return grpc::Status::OK;
}

/// TODO: Complete this logic
void ScalogSequencerService::UpdateTotalOrdering(std::vector<int> global_cut, struct TInode *tinode) {
	// Reference topic_manager.cc to see how we get the first message header then iterate through messages using the next_message field
	struct MessageHeader *header = (struct MessageHeader*)((uint8_t*) cxl_addr_ + tinode->offsets[broker_id_].log_offset);

	int order_start = 0;
	for (int i = 0; i < broker_id_; i++) {
		order_start += global_cut[i];
	}

	// Iterate through messages and update total_order field
	for (int i = 0; i < global_cut[broker_id_]; i++) {
		header->total_order = order_start + i;
		
		// Move to next message using next_message
		// header = (struct MessageHeader*) header->next_message;
	}

	/// TODO: Update ordered offset

	// Update ordered
	tinode->offsets[broker_id_].ordered = header->logical_offset;
}

grpc::Status ScalogSequencerService::HandleSendLocalCut(grpc::ServerContext* context, const SendLocalCutRequest* request, SendLocalCutResponse* response) {
  const char* topic = request->topic().c_str();
	int epoch = request->epoch();
	int local_cut = request->local_cut();
	int broker_id = request->broker_id();

	std::cout << "Received local cut with grpc" << std::endl;

	ReceiveLocalCut(epoch, local_cut, topic, broker_id);

	return grpc::Status::OK;
}

void ScalogSequencerService::ReceiveGlobalCut(std::vector<int> global_cut, const char* topic) {
	// Maybe request can hold topic string
	struct TInode *tinode = (struct TInode *) cxl_manager_->GetTInode(topic);

	UpdateTotalOrdering(global_cut, tinode);

	std::cout << "Received global cut without grpc" << std::endl;

	received_global_seq_[topic] = true;

	std::cout << "Set global cut received to true" << std::endl;

	// ScalogSendLocalCut(scalog_local_epoch_[topic], tinode->offsets[broker_id_].written, topic);

	if (received_gobal_seq_after_interval_[topic]) {

		received_gobal_seq_after_interval_[topic] = false;

		std::cout << "Global cut was received after the 5 ms interval so cancelling timer" <<std::endl;

		// Cancel timer so ScalogLocalSequencer runs immediately
		timer_.cancel();
	}
}

void CXLManager::Sequencer1(char* topic){
	struct TInode *tinode = (struct TInode *)GetTInode(topic);
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS];
	absl::btree_set<int> registered_brokers;
	size_t perLogOff[NUM_MAX_BROKERS];
	static size_t seq = 0;

	for(int i = 0; i < NUM_MAX_BROKERS; i++){
		msg_to_order[i] = (struct MessageHeader*)cxl_addr_;
		perLogOff[i] = -1;
	}

	get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode);
	auto last_updated = std::chrono::steady_clock::now();

	while(!stop_threads_){
		bool yield = true;
		for(auto broker : registered_brokers){
			size_t msg_logical_off = msg_to_order[broker]->logical_offset;
			if(msg_to_order[broker]->paddedSize != 0 && msg_logical_off != (size_t)-1 && msg_logical_off <= tinode->offsets[broker].written){//This ensures the message is Combined (all the other fields are filled)
				if(msg_logical_off != perLogOff[broker]+1){
					if(msg_logical_off != (size_t)-1 && msg_to_order[broker]->next_msg_diff != 0){
						msg_to_order[broker] = (MessageHeader*)((uint8_t*)msg_to_order[broker] + msg_to_order[broker]->next_msg_diff);
					}
					continue;
				}
				msg_to_order[broker]->total_order = seq;
				tinode->offsets[broker].ordered = msg_logical_off;
				tinode->offsets[broker].ordered_offset = (uint8_t*)msg_to_order[broker] - (uint8_t*)cxl_addr_;
				perLogOff[broker] = msg_logical_off;
				seq++;
				yield = false;
				if(msg_to_order[broker]->next_msg_diff != 0){
					msg_to_order[broker] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker] + msg_to_order[broker]->next_msg_diff);
				}
			}
		}
		if(yield){
			get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode);
			last_updated = std::chrono::steady_clock::now();
			std::this_thread::yield();
		}else if(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
					- last_updated).count() >= HEARTBEAT_INTERVAL){
			get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode);
			last_updated = std::chrono::steady_clock::now();
		}
	}
}

// One Sequencer per topic. The broker that received CreateNewTopic spawn it.
void CXLManager::Sequencer2(char* topic){
	struct TInode *tinode = (struct TInode *)GetTInode(topic);
	struct MessageHeader *msg_headers[NUM_MAX_BROKERS];
	absl::flat_hash_map<int/*client_id*/, size_t/*client_req_id*/> last_ordered; 
	// It is OK to store as addresses b/c the total order is given by a single thread
	absl::flat_hash_map<int, absl::btree_map<size_t/*client_id*/, struct MessageHeader*>> skipped_msg;
	static size_t seq = 0;
	int perLogOff[NUM_MAX_BROKERS];

	for(int i = 0; i<NUM_MAX_BROKERS; i++){
		while(tinode->offsets[i].log_offset == 0){}
		msg_headers[i] = (struct MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[i].log_offset);
		perLogOff[i] = -1; 
	}

	//TODO(Jae) This logic is wrong as the ordered offset can skip few messages 
	//and the broker exports all data upto the ordered offset
	while(!stop_threads_){
		bool yield = true;
		for(int i = 0; i<NUM_MAX_BROKERS; i++){
			if(perLogOff[i] < tinode->offsets[i].written){//This ensures the message is Combined (all the other fields are filled)
				if((int)msg_headers[i]->logical_offset != perLogOff[i]+1){
					LOG(ERROR) << "!!!!!!!!!!!! [Sequencer2] Error msg_header is not equal to the perLogOff";
				}
				int client = msg_headers[i]->client_id;
				auto last_ordered_itr = last_ordered.find(client);
				perLogOff[i] = msg_headers[i]->logical_offset;
				if(msg_headers[i]->client_order == 0 || 
						(last_ordered_itr != last_ordered.end() && last_ordered_itr->second == msg_headers[i]->client_order - 1)){
					// Give order 
					msg_headers[i]->total_order = seq;
					tinode->offsets[i].ordered = msg_headers[i]->logical_offset;
					seq++;
					last_ordered[client] = msg_headers[i]->client_order;
					// Check if there are skipped messages from this client and give order
					auto it = skipped_msg.find(client);
					if(it != skipped_msg.end()){
						std::vector<int> to_remove;
						for (auto& pair : it->second) {
							if(pair.first == last_ordered[client] + 1){
								pair.second->total_order = seq;
								tinode->offsets[i].ordered = pair.second->logical_offset;
								seq++;
								last_ordered[client] = pair.first;
								to_remove.push_back(pair.first);
							}else{
								break;
							}
						}
						for(auto &id: to_remove){
							it->second.erase(id);
						}
					}
				}else{
					//Insert to skipped messages
					auto it = skipped_msg.find(client);
					if (it == skipped_msg.end()) {
						absl::btree_map<size_t, struct MessageHeader*> new_map;
						new_map.emplace(msg_headers[i]->client_order, msg_headers[i]);
						skipped_msg.emplace(client, std::move(new_map));
					} else {
						it->second.emplace(msg_headers[i]->client_order, msg_headers[i]);
					}
				}
				msg_headers[i] = (MessageHeader*)((uint8_t*)msg_headers[i] + msg_headers[i]->paddedSize);
				yield = false;
			}
		}
		if(yield)
			std::this_thread::yield();
	}
}

} // End of namespace Embarcadero
