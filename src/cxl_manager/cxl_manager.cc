#include "cxl_manager.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <glog/logging.h>
#include "mimalloc.h"

namespace Embarcadero{

CXLManager::CXLManager(size_t queueCapacity, int broker_id, int num_io_threads):
	requestQueue_(queueCapacity),
	broker_id_(broker_id),
	num_io_threads_(num_io_threads){
	// Initialize CXL
	cxl_type_ = Real;
	std::string cxl_path(getenv("HOME"));
	size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	switch(cxl_type_){
		case Emul:
			cxl_path += "/.CXL_EMUL/cxl";
			cxl_fd_ = open(cxl_path.c_str(), O_RDWR, 0777);
			break;
		case Real:
			cxl_fd_ = open("/dev/dax0.0", O_RDWR);
			break ;
	}
	if (cxl_fd_  < 0)
		perror("Opening CXL error");

	cxl_addr_= mmap(NULL, CXL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, cxl_fd_, 0);
	if (cxl_addr_ == MAP_FAILED){
		perror("Mapping Emulated CXL error");
	}
	//memset(cxl_addr_, 0, CXL_SIZE);
	memset(cxl_addr_, 0, (1UL<<34));
	// Create CXL I/O threads
	for (int i=0; i< num_io_threads_; i++)
		threads_.emplace_back(&CXLManager::CXL_io_thread, this);

	// Initialize CXL memory regions
	size_t TINode_Region_size = sizeof(TInode) * MAX_TOPIC_SIZE;
	size_t padding = TINode_Region_size - ((TINode_Region_size/cacheline_size) * cacheline_size);
	TINode_Region_size += padding;
	size_t Bitmap_Region_size = cacheline_size * MAX_TOPIC_SIZE;
	size_t Segment_Region_size = (CXL_SIZE - TINode_Region_size - Bitmap_Region_size)/NUM_BROKERS;

	bitmap_ = (uint8_t*)cxl_addr_ + TINode_Region_size;
	segments_ = (uint8_t*)bitmap_ + Bitmap_Region_size + ((broker_id_)*Segment_Region_size);

	// Head node initialize the CXL
	if(broker_id_ == 0){
		memset(cxl_addr_, 0, TINode_Region_size);
	}

#ifdef InternalTest
	for(size_t i=0; i<queueCapacity; i++){
		WriteDummyReq();
	}
#endif
	// Wait untill al IO threads are up
	while(thread_count_.load() != num_io_threads_){}

	std::cout << "[CXLManager]: \tConstructed" << std::endl;
	return;
}

#ifdef InternalTest
void CXLManager::WriteDummyReq(){
	PublishRequest req;
	memset(req.topic, 0, TOPIC_NAME_SIZE);
	req.topic[0] = '0';
	req.counter = (std::atomic<int>*)malloc(sizeof(std::atomic<int>));
	req.counter->store(1);
	req.payload_address = malloc(1024);
	req.size = 1024-64;
	requestQueue_.blockingWrite(req);
}

void CXLManager::DummyReq(){
	for(int i=0; i<10000; i++){
		WriteDummyReq();
	}
}

void CXLManager::StartInternalTest(){
	for(int i=0; i<100; i++){
		testThreads_.emplace_back(&CXLManager::DummyReq, this);
	}
	start = std::chrono::high_resolution_clock::now();
	startInternalTest_.store(true);
	for(std::thread& thread : testThreads_){
		if(thread.joinable()){
			thread.join();
		}
	}
}
#endif

CXLManager::~CXLManager(){
	std::optional<struct PublishRequest> sentinel = std::nullopt;
	stop_threads_ = true;
	for (int i=0; i< num_io_threads_; i++)
		requestQueue_.blockingWrite(sentinel);

	if (munmap(cxl_addr_, CXL_SIZE) < 0)
		perror("Unmapping CXL error");
	close(cxl_fd_);


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

	std::cout << "[CXLManager]: \tDestructed" << std::endl;
}


void CXLManager::CXL_io_thread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct PublishRequest> optReq;
	/*
	static std::atomic<size_t> DEBUG_1{0};
	static std::atomic<size_t> DEBUG_2{0};
	*/

#ifdef InternalTest
	while(startInternalTest_.load() == false){}
#endif
	while(!stop_threads_){
		requestQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		struct PublishRequest &req = optReq.value();
		/*
		if(DEBUG_1.fetch_add(1) == 9999999){
			VLOG(3) << "All requests popped";
			DEBUG_1_passed_ = true;
		}
		*/

		// Actual IO to the CXL
		topic_manager_->PublishToCXL(req);//req.topic, req.payload_address, req.size);

		/*
		if(DEBUG_2.fetch_add(1) == 9999999){
			VLOG(3) << "All requests Written";
			DEBUG_2_passed_ = true;
		}
		*/

		// Post I/O work (as disk I/O depend on the same payload)
		int counter = req.counter->fetch_sub(1);
		if( counter == 1){
			mi_free(req.counter);
			mi_free(req.payload_address);
#ifdef InternalTest
			if(reqCount_.fetch_add(1) == 999999){
				auto end = std::chrono::high_resolution_clock::now();
				auto dur = end - start;
				std::cout<<"Runtime:" << std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() << std::endl;
				std::cout<<(double)1024/(double)std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() << "GB/s" << std::endl;
			}
#endif
		}else if(req.acknowledge){
			struct NetworkRequest ackReq;
			ackReq.client_socket = req.client_socket;
			network_manager_->EnqueueRequest(ackReq);
		}
	}
}

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
	static std::atomic<int> segment_count{0};
	int offset = segment_count.fetch_add(1, std::memory_order_relaxed);

	//TODO(Jae) Implement bitmap
	return (uint8_t*)segments_ + offset*SEGMENT_SIZE;
}

bool CXLManager::GetMessageAddr(const char* topic, size_t &last_offset,
																void* &last_addr, void* messages, size_t &messages_size){
	return topic_manager_->GetMessageAddr(topic, last_offset, last_addr, messages, messages_size);
}

void CXLManager::CreateNewTopic(char topic[TOPIC_NAME_SIZE], int order, SequencerType sequencerType){
	topic_manager_->CreateNewTopic(topic, order);
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
				std::string global_sequencer_url = broker_->address_ + ":" + broker_->port_;
				
				// Iterate through broker list and call async grpc to notify them to run local ordering thread
				for (auto const& peer : broker_->peer_brokers_) {
					std::string peer_url = peer.first + ":" + peer.second;
					auto rpc_client = GetRpcClient(peer_url);

					ScalogStartLocalSequencerRequest request;
					request.set_topic(topic);
					request.set_global_sequencer_url(global_sequencer_url);

					ScalogStartLocalSequencerResponse response;
					grpc::ClientContext context;

					auto callback = [](grpc::Status status) {
						if (!status.ok()) {
							std::cout << "Error sending start local sequencer request" << std::endl;
						}
					};

					// Async call to HandleStartLocalSequencer
					rpc_client->async()->HandleScalogStartLocalSequencer(&context, &request, &response, callback);
				}

				// Spawn local ordering thread on broker that received CreateNewTopic
				sequencerThreads_.emplace_back(&CXLManager::ScalogLocalSequencer, this, topic, global_sequencer_url);

				// Spawn global sequencer thread on the broker that received CreateNewTopic
				sequencerThreads_.emplace_back(&CXLManager::ScalogGlobalSequencer, this, topic);

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

std::unique_ptr<ScalogSequencer::Stub> CXLManager::GetRpcClient(std::string peer_url) {
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(peer_url, grpc::InsecureChannelCredentials());
    return ScalogSequencer::NewStub(channel);
}

grpc::Status CXLManager::HandleScalogStartLocalSequencer(grpc::ServerContext* context, const ScalogStartLocalSequencerRequest* request, ScalogStartLocalSequencerResponse* response) {

	// Spawn local scalog sequencer thread
	sequencerThreads_.emplace_back(&CXLManager::ScalogLocalSequencer, this, request->topic(), request->global_sequencer_url());

	return grpc::Status::OK;
}

void CXLManager::ScalogLocalSequencer(char* topic, std::string global_sequencer_url){
	scalog_global_sequencer_url_[topic] = global_sequencer_url;
	struct TInode *tinode = (struct TInode *)GetTInode(topic);
	scalog_global_cut_[topic] = std::vector<int>(NUM_BROKERS, 0);
	scalog_received_global_seq_[topic] = false;

	// Send written offset variable to global sequencer every 5 ms iff global cut has been received
	while (true) {
		if (scalog_local_epoch_[topic] == 0) {
			// This is the first local cut to be sent, don't need to wait for global cut

			// send epoch and tinode->offsets[broker_id_].written to global sequencer
			ScalogSendLocalCut(scalog_local_epoch_[topic], tinode->offsets[broker_id_].written, topic);

			scalog_local_epoch_[topic]++;
		} else if (scalog_local_epoch_[topic] > 0 && scalog_received_global_seq_[topic]) {
			/// Send epoch and tinode->offsets[broker_id_].written to global sequencer
			ScalogSendLocalCut(scalog_local_epoch_[topic], tinode->offsets[broker_id_].written, topic);

			scalog_local_epoch_[topic]++;
			scalog_received_global_seq_[topic] = false;
		} else if (scalog_local_epoch_[topic] > 0 && !scalog_received_global_seq_[topic]) {
			// If global cut hasn't been received, wait until it receives next global cut then immediately send local cut in ScalogReceiveGlobalCut
			{}
		} else {
			LOG(ERROR) << "Epoch is negative in local sequencer";
			exit(1);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

// Helper function that allows a local scalog sequencer to send their local cut
void CXLManager::ScalogSendLocalCut(int epoch, int written, char* topic) {
	auto rpc_client = GetRpcClient(scalog_global_sequencer_url_[topic]);

	ScalogSendLocalCutRequest request;
	request.set_epoch(epoch);
	request.set_written(written);
	request.set_topic(topic);

	ScalogSendLocalCutResponse response;
	grpc::ClientContext context;

	auto callback = [](grpc::Status status) {
		if (!status.ok()) {
			std::cout << "Error sending local cut" << std::endl;
		}
	};

	// Async call to HandleStartLocalSequencer
	rpc_client->async()->HandleScalogSendLocalCut(&context, &request, &response, callback);
}

grpc::Status CXLManager::HandleScalogSendGlobalCut(grpc::ServerContext* context, const ScalogSendGlobalCutRequest* request, ScalogSendGlobalCutResponse* response) {
    std::vector<int> global_cut(request->global_cut().begin(), request->global_cut().end());
    std::string topic = request->topic();

	sequencerThreads_.emplace_back(&CXLManager::ScalogReceiveGlobalCut, this, global_cut, topic.c_str());
}

void ScalogReceiveGlobalCut(std::vector<int> global_cut, char* topic) {
	// Maybe request can hold topic string
	struct TInode *tinode = (struct TInode *)GetTInode(topic);

	ScalogUpdateTotalOrdering(global_cut, tinode);

	// Set scalog_received_global_seq_[topic] to true
	scalog_received_global_seq_[topic] = true;

	// Increment epoch
	scalog_local_epoch_[topic]++;

	/// TODO: Potential issue where after this happens we could immediately send again in the scalog local seq. Do we want this?
	// send epoch and tinode->offsets[broker_id_].written to global sequencer 
	ScalogSendLocalCut(scalog_local_epoch_[topic], tinode->offsets[broker_id_].written);
}

/// TODO: Complete this logic
void ScalogUpdateTotalOrdering(std::vector<int> global_cut, struct TInode *tinode) {
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
		header = header->next_message;
	}

	/// TODO: Update ordered offset

	// Update ordered
	tinode->offsets[broker_id_].ordered = header->logical_offset;
}

grpc::Status CXLManager::HandleScalogSendLocalCut(grpc::ServerContext* context, const ScalogSendLocalCutRequest* request, ScalogSendLocalCutResponse* response) {
	std::string topic = request->topic();
	sequencerThreads_.emplace_back(&CXLManager::ScalogReceiveLocalCut, this, request->epoch(), request->written(), topic.c_str());

	return grpc::Status::OK;
}

void ScalogReceiveLocalCut(int epoch, int written, char* topic) {
	if (request.epoch != scalog_global_epoch_[topic]) {
		// If the epoch is not the same as the current global epoch, there is an error
		LOG(ERROR) << "Local cut from local sequencer was sent too early, global sequencer has not yet sent the global cut";
		exit(1);
	}

	// Insert local cut into global cut
	scalog_global_cut_[topic][request.broker_id] = request.offset;

	// increment local_cuts_count_
	local_cuts_count_[topic]++;
}

void CXLManager::ScalogGlobalSequencer(char* topic){
	// Loop infinitely and wait until it gets local cuts from all local sequencers
	while (true) {

		// If count of local cuts equal to number of brokers, we have received all local cuts for the epoch
		if (local_cuts_count_[topic] == NUM_BROKERS) {
			// Send global cut to own node's local sequencer
			std::string current_node_url = broker_->address_ + ":" + broker_->port_;
			auto rpc_client = GetRpcClient(current_node_url);

			ScalogSendGlobalCutRequest request;
			/// TODO: Is there a more efficient way to do this?
			for (int value : scalog_global_cut_[topic]) {
				request.add_global_cut(value);
			}

			ScalogSendGlobalCutResponse response;
			grpc::ClientContext context;

			auto callback = [](grpc::Status status) {
				if (!status.ok()) {
					std::cout << "Error sending global cut request" << std::endl;
				}
			};

			rpc_client->async()->HandleScalogSendGlobalCut(&context, &request, &response, callback);

			// Iterate through broker list and call async grpc to send global cut
			for (auto const& peer : broker_->peer_brokers_) {
				std::string peer_url = peer.first + ":" + peer.second;
				auto rpc_client = GetRpcClient(peer_url);

				ScalogSendGlobalCutRequest request;
				for (int value : scalog_global_cut_[topic]) {
					request.add_global_cut(value);
				}

				ScalogSendGlobalCutResponse response;
				grpc::ClientContext context;

				auto callback = [](grpc::Status status) {
					if (!status.ok()) {
						std::cout << "Error sending global cut request" << std::endl;
					}
				};

				rpc_client->async()->HandleScalogSendGlobalCut(&context, &request, &response, callback);
			}

			local_cuts_count_[topic] = 0;
			scalog_global_epoch_[topic]++;
		}
	}
}

void CXLManager::Sequencer1(char* topic){
	static size_t header_size = sizeof(MessageHeader);
	struct TInode *tinode = (struct TInode *)GetTInode(topic);
	struct MessageHeader *msg_headers[NUM_BROKERS];
	size_t seq = 0;
    int perLogOff[NUM_BROKERS];

	for(int i = 0; i<NUM_BROKERS; i++){
        while(tinode->offsets[i].log_offset == 0){}
		msg_headers[i] = (struct MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[i].log_offset);
        perLogOff[i] = -1;
	}
	while(!stop_threads_){
		bool yield = true;
		for(int i = 0; i<NUM_BROKERS; i++){
            if(perLogOff[i] < tinode->offsets[i].written){//This ensures the message is Combined (all the other fields are filled)
				if((int)msg_headers[i]->logical_offset != perLogOff[i]+1){
					perror("!!!!!!!!!!!! [Sequencer1] Error msg_header is not equal to the perLogOff");
				}
				msg_headers[i]->total_order = seq;
				tinode->offsets[i].ordered = msg_headers[i]->logical_offset;
				perLogOff[i] = msg_headers[i]->logical_offset;
				seq++;
				msg_headers[i] = (MessageHeader*)((uint8_t*)msg_headers[i] + msg_headers[i]->paddedSize);
				yield = false;
			}
			//TODO(Jae) if multi segment is implemented as last message to have a dummy, this should be handled
		}
		if(yield)
			std::this_thread::yield();
	}
}

// One Sequencer per topic. The broker that received CreateNewTopic spawn it.
void CXLManager::Sequencer2(char* topic){
	static size_t header_size = sizeof(MessageHeader);
	struct TInode *tinode = (struct TInode *)GetTInode(topic);
	struct MessageHeader *msg_headers[NUM_BROKERS];
	absl::flat_hash_map<int/*client_id*/, size_t/*client_req_id*/> last_ordered; 
	// It is OK to store as addresses b/c the total order is given by a single thread
	absl::flat_hash_map<int, absl::btree_map<size_t/*client_id*/, struct MessageHeader*>> skipped_msg;
	static size_t seq = 0;
    int perLogOff[NUM_BROKERS];

	for(int i = 0; i<NUM_BROKERS; i++){
        while(tinode->offsets[i].log_offset == 0){}
		msg_headers[i] = (struct MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[i].log_offset);
        perLogOff[i] = -1; 
	}

//TODO(Jae) This logic is wrong as the ordered offset can skip few messages 
//and the broker exports all data upto the ordered offset
	while(!stop_threads_){
		bool yield = true;
		for(int i = 0; i<NUM_BROKERS; i++){
            if(perLogOff[i] < tinode->offsets[i].written){//This ensures the message is Combined (all the other fields are filled)
				if((int)msg_headers[i]->logical_offset != perLogOff[i]+1){
					perror("!!!!!!!!!!!! [Sequencer2] Error msg_header is not equal to the perLogOff");
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
