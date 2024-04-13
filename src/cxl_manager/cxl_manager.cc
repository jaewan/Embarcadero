#include "cxl_manager.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <iostream>
#include <cstdlib>

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
	if (cxl_addr_ == MAP_FAILED)
		perror("Mapping Emulated CXL error");
	//memset(cxl_addr_, 0, CXL_SIZE);
	memset(cxl_addr_, 0, (1UL<<30));

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

#ifdef InternalTest
	while(startInternalTest_.load() == false){}
#endif
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
			free(req.counter);
			free(req.payload_address);
#ifdef InternalTest
			if(reqCount_.fetch_add(1) == 999999){
				auto end = std::chrono::high_resolution_clock::now();
				auto dur = end - start;
				std::cout<<"Runtime:" << std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() << std::endl;
				std::cout<<(double)1024/(double)std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() << "GB/s" << std::endl;
			}
#endif
		}else if(req.acknowledge){
			struct NetworkRequest req;
			req.req_type = Acknowledge;
			req.client_socket = 1;
			network_manager_->EnqueueRequest(req);
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
            if(perLogOff[i] < tinode->offsets[i].written){
				if((int)msg_headers[i]->logical_offset != perLogOff[i]+1){
					perror("!!!!!!!!!!!! [Sequencer1] Error msg_header is not equal to the perLogOff");
				}
				msg_headers[i]->total_order = seq;
				tinode->offsets[i].ordered = msg_headers[i]->logical_offset;
				perLogOff[i] = msg_headers[i]->logical_offset;
				seq++;
				msg_headers[i] = (MessageHeader*)((uint8_t*)msg_headers[i] + msg_headers[i]->paddedSize + header_size);
            }else{
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
	struct TInode *tinode = (struct TInode *)GetTInode(topic);
	struct MessageHeader *msg_headers[NUM_BROKERS];
	absl::flat_hash_map<int, size_t> last_ordered; // <client_id, client_request_id>>
	absl::flat_hash_map<int, absl::btree_map<size_t, struct MessageHeader*>> skipped_msg; //<client_id, <client_req_id,*msg_header>>
	size_t seq = 0;
    int perLogOff[NUM_BROKERS];

	for(int i = 0; i<NUM_BROKERS; i++){
        while(tinode->offsets[i].log_offset == 0){}
		msg_headers[i] = (struct MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[i].log_offset);
        perLogOff[i] = -1; 
	}

//TODO(Jae) This logic is wrong as the ordered offset can skip few messages 
//and the broker exports all data upto the ordered offset
	while(!stop_threads_){
		for(int i = 0; i<NUM_BROKERS; i++){
            if(perLogOff[i] < tinode->offsets[i].written && (int)msg_headers[i]->logical_offset == perLogOff[i]){
                msg_headers[i] = (MessageHeader*)(msg_headers[i]->next_message);
            }
            while(perLogOff[i] < tinode->offsets[i].written){
                int client = msg_headers[i]->client_id;
				auto last_ordered_itr = last_ordered.find(client);
				if(last_ordered_itr == last_ordered.end() || last_ordered_itr->second == msg_headers[i]->client_order - 1){
                    //Give order
                    msg_headers[i]->total_order = seq;
					perLogOff[i] = msg_headers[i]->logical_offset;
                    tinode->offsets[i].ordered = msg_headers[i]->logical_offset;
                    seq++;
                    last_ordered[client] = msg_headers[i]->client_order;
                    auto it = skipped_msg.find(client);
                    if(it != skipped_msg.end()){
                        std::vector<int> to_remove;
                        for (auto& pair : it->second) {
                            if(pair.first == last_ordered[client] + 1){
                                pair.second->total_order = seq;
								perLogOff[i] = msg_headers[i]->logical_offset;
                                tinode->offsets[i].ordered = pair.second->logical_offset;
                                seq++;
                                last_ordered[client] = pair.first;
                                to_remove.push_back(pair.first);
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
                if(msg_headers[i]->next_message)
                    msg_headers[i] = (struct MessageHeader*)msg_headers[i]->next_message;
			}
		}
	}
}

} // End of namespace Embarcadero
