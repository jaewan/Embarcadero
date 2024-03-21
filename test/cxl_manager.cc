#include "cxl_manager.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <iostream>

namespace Embarcadero{

#define CXL_SIZE (1UL << 34)
#define log_SIZE (1UL << 30)
#define NUM_CXL_IO_THREADS 2
#define MAX_TOPIC 4

CXLManager::CXLManager(){
	// Initialize CXL
	cxl_type_ = Emul;
	std::string cxl_path(getenv("HOME"));
	cxl_path += "/.CXL_EMUL/cxl";

	long cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	switch(cxl_type_){
		case Emul:
			cxl_emul_fd_ = open(cxl_path.c_str(), O_RDWR, 0777);
			if (cxl_emul_fd_  < 0)
				perror("Opening Emulated CXL error");

			cxl_addr_= mmap(NULL, CXL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, cxl_emul_fd_, 0);
			if (cxl_addr_ == MAP_FAILED)
				perror("Mapping Emulated CXL error");

			std::cout << "Successfully initialized CXL Emul" << std::endl;
			break;
		case Real:
			perror("Not implemented real cxl yet");
			break ;
	}

	// Create CXL I/O threads
	for (int i=0; i< NUM_CXL_IO_THREADS; i++)
		threads_.emplace_back(&CXLManager::CXL_io_thread, this);
}

CXLManager::~CXLManager(){
	// Close CXL emulation
	switch(cxl_type_){
		case Emul:
			if (munmap(cxl_addr_, CXL_SIZE) < 0)
				perror("Unmapping Emulated CXL error");
			close(cxl_emul_fd_);

			std::cout << "Successfully deinitialized CXL Emul" << std::endl;
			break;
		case Real:
			perror("Not implemented real cxl yet");
			break;
	}

	// Stop IO threads
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		stop_threads_ = true;
		queueCondVar_.notify_all(); 
	}

	for(std::thread& thread : threads_){
		if(thread.joinable()){
			thread.join();
		}
	}
}

void CXLManager::CXL_io_thread(){
	while(!stop_threads_){
		// Sleep until a request is popped from the requestQueue
		struct publish_request req;
		{
			std::unique_lock<std::mutex> lock(queueMutex_);
			queueCondVar_.wait(lock, [this] {return !requestQueue_.empty() || stop_threads_;});
			req = requestQueue_.front();
			requestQueue_.pop();
		}

		// Actual IO to the CXL
		topic_manager_->PublishToCXL(req.topic, req.payload_address, req.size);

		// Post I/O work (as disk I/O depend on the same payload)
		int counter = req.counter->fetch_sub(1, std::memory_order_relaxed);
		if( counter == 1){
			free(req.payload_address);
		}else if(req.acknowledge){
			//TODO(Jae)
			//Enque ack request to network manager
			// network_manager_.EnqueueAckRequest();
		}
	}
}

void* CXLManager::Get_tinode(const char* topic, int broker_num){
	// Convert topic to tinode address
	static const std::hash<std::string> topic_to_idx;
	int TInode_idx = topic_to_idx(topic) % MAX_TOPIC;
	return ((uint8_t*)cxl_addr_ + (TInode_idx * sizeof(struct TInode)));
}

void* CXLManager::GetNewSegment(){
	return nullptr;
}


} // End of namespace Embarcadero

int main(){
	Embarcadero::CXLManager cxl_manager = Embarcadero::CXLManager();
	Embarcadero::TopicManager topic_manager = Embarcadero::TopicManager();
	cxl_manager.SetTopicManager(&topic_manager);
	return 0;
}
