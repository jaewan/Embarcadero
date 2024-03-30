#include "cxl_manager.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <iostream>

namespace Embarcadero{

#define CXL_SIZE (1UL << 37)

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

	cxl_addr_= mmap(NULL, CXL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, cxl_fd_, 0);
	if (cxl_addr_ == MAP_FAILED)
		perror("Mapping Emulated CXL error");

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
	segments_ = (uint8_t*)bitmap_ + ((broker_id_)*Segment_Region_size);

	// Head node initialize the CXL
	if(broker_id_ == 0){
		memset(cxl_addr_, 0, TINode_Region_size);
	}

	// Wait untill al IO threads are up
	while(thread_count_.load() != num_io_threads_){}

	std::cout << "[CXLManager]: \tConstructed" << std::endl;
	return;
}

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

	std::cout << "[CXLManager]: \tDestructed" << std::endl;
}

void CXLManager::CXL_io_thread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct PublishRequest> optReq;

	while(!stop_threads_){
		requestQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		std::cout<<"Cxl req" << std::endl;
		struct PublishRequest &req = optReq.value();
		// Actual IO to the CXL
		topic_manager_->PublishToCXL(req.topic, req.payload_address, req.size);
		std::cout<<"Cxl wrote" << std::endl;

		// Post I/O work (as disk I/O depend on the same payload)
		int counter = req.counter->fetch_sub(1, std::memory_order_relaxed);
		if( counter == 1){
		std::cout<<"\t\tCXL is Slower" << std::endl;
			free(req.payload_address);
		}else if(req.acknowledge){
		std::cout<<"\t\tCXL is faster" << std::endl;
			//TODO(Jae)
			//Enque ack request to network manager
			// network_manager_.EnqueueAckRequest();
		}
	}
}

void* CXLManager::GetTInode(const char* topic){
	// Convert topic to tinode address
	static const std::hash<std::string> topic_to_idx;
	int TInode_idx = topic_to_idx(topic) % MAX_TOPIC_SIZE;
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

} // End of namespace Embarcadero
