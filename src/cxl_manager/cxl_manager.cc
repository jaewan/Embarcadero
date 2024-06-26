#include "cxl_manager.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <iostream>
#include <cstdlib>
#include <glog/logging.h>
#include "../network_manager/request_data.h"

namespace Embarcadero{

CXLManager::CXLManager(std::shared_ptr<AckQueue> ack_queue, std::shared_ptr<ReqQueue> req_queue, int broker_id, CXL_Type cxl_type, int num_io_threads):
	ackQueue_(ack_queue),
	reqQueue_(req_queue),
	broker_id_(broker_id),
	num_io_threads_(num_io_threads) {
	// Initialize CXL

	cxl_type_ = cxl_type;
	std::string cxl_path(getenv("HOME"));
	size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	switch(cxl_type_){
		case Emul:
			cxl_path += "/.CXL_EMUL/cxl";
			cxl_fd_ = open(cxl_path.c_str(), O_RDWR, 0777);
			break;
		case Real:
			cxl_path = "/dev/dax0.0";
			cxl_fd_ = open(cxl_path.c_str(), O_RDWR);
			break;
	}
	LOG(INFO) << "Opening CXL at: " << cxl_path;
	if (cxl_fd_ < 0) {
		perror("Opening CXL error");
		exit(-1);
	}

	cxl_addr_= mmap(NULL, CXL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, cxl_fd_, 0);
	if (cxl_addr_ == MAP_FAILED){
		perror("Mapping Emulated CXL error");close(cxl_fd_);
		exit(-1);
	}
	LOG(INFO) << "Zeroing the CXL memory: ";
	//memset(cxl_addr_, 0, CXL_SIZE);
	memset(cxl_addr_, 0, (1UL)<<30);

	// Create CXL I/O threads
	for (int i=0; i< num_io_threads_; i++)
		threads_.emplace_back(&CXLManager::CXLIOThread, this);

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

	LOG(INFO) << "[CXLManager] Constructed";
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
	reqQueue_.blockingWrite(req);
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
	for (int i=0; i< num_io_threads_; i++) {
		reqQueue_->blockingWrite(sentinel);
	}

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
}

void CXLManager::CXLIOThread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct PublishRequest> optReq;

#ifdef InternalTest
	while(startInternalTest_.load() == false){}
#endif
	while(!stop_threads_){
		reqQueue_->blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		struct PublishRequest &req = optReq.value();
		struct RequestData *req_data = static_cast<RequestData*>(req.grpcTag);

		// Actual IO to the CXL
	   topic_manager_->PublishToCXL(req_data);//(char *)(req_data->request_.topic().c_str()), (void *)(req_data->request_.payload().c_str()), req_data->request_.payload_size());
	   //topic_manager_->PublishToCXL(req);//req.topic, req.payload_address, req.size);
	   
	   // TODO(erika): below logic should really be shared function between CXL and Disk managers
	 int counter = req.counter->fetch_sub(1, std::memory_order_relaxed);
	
	 // If no more tasks are left to do
	 if (counter == 1) {
	 	if (req_data->request_.acknowledge()) {
			// TODO: Set result - just assume success
			req_data->SetError(ERR_NO_ERROR);

			// Send to network manager ack queue
			auto maybeTag = std::make_optional(req.grpcTag);
			VLOG(2) << "Enquing to ack queue, tag=" << req.grpcTag;
			EnqueueAck(ackQueue_, maybeTag);
      	}
#ifdef InternalTest
		if(reqCount_.fetch_add(1) == 999999){
			auto end = std::chrono::high_resolution_clock::now();
			auto dur = end - start;
			std::cout<<"Runtime:" << std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() << std::endl;
			std::cout<<(double)1024/(double)std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() << "GB/s" << std::endl;
		}
#endif
	  } else {
			// gRPC has already sent response, so just mark the object as ready for destruction
			//delete req.counter;
			//req_data->Proceed();
	  }
	}// End While
}

void* CXLManager::GetTInode(const char* topic){
	// Convert topic to tinode address
	//static const std::hash<std::string> topic_to_idx;
	//int TInode_idx = topic_to_idx(topic) % MAX_TOPIC_SIZE;
	int TInode_idx = atoi(topic) % MAX_TOPIC_SIZE;
	return ((uint8_t*)cxl_addr_ + (TInode_idx * sizeof(struct TInode)));
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

void CXLManager::CreateNewTopic(char topic[TOPIC_NAME_SIZE], int order){
	topic_manager_->CreateNewTopic(topic, order);
	if (order == 1){
		sequencerThreads_.emplace_back(&CXLManager::Sequencer1, this, topic);
	}else if (order == 2)
		sequencerThreads_.emplace_back(&CXLManager::Sequencer2, this, topic);
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
					LOG(ERROR) << "!!!!!!!!!!!! [Sequencer1] Error msg_header is not equal to the perLogOff";
				}
				msg_headers[i]->total_order = seq;
				tinode->offsets[i].ordered = msg_headers[i]->logical_offset;
				perLogOff[i] = msg_headers[i]->logical_offset;
				seq++;
				msg_headers[i] = (MessageHeader*)((uint8_t*)msg_headers[i] + msg_headers[i]->paddedSize + header_size);
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
				msg_headers[i] = (MessageHeader*)((uint8_t*)msg_headers[i] + msg_headers[i]->paddedSize + header_size);
				yield = false;
            }
		}
		if(yield)
			std::this_thread::yield();
	}
}

} // End of namespace Embarcadero
