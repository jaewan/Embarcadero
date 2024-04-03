#include "disk_manager.h"

#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

namespace Embarcadero{

#define DISK_LOG_PATH "embarc.disklog"
#define NUM_ACTIVE_POLL 100

DiskManager::DiskManager(size_t queueCapacity, 
						 int num_io_threads):
						 requestQueue_(queueCapacity),
						 num_io_threads_(num_io_threads){
	//Initialize log file
	log_fd_ = open(DISK_LOG_PATH, O_RDWR|O_CREAT, 0777);
	if (log_fd_ < 0){
		perror("Error in opening a file for disk log\n");
		std::cout<< strerror(errno) << std::endl;
	}
	// Create Disk I/O threads
	for (int i=0; i< num_io_threads_; i++)
		threads_.emplace_back(&DiskManager::Disk_io_thread, this);

	while(thread_count_.load() != num_io_threads_){}
	std::cout << "[DiskManager]: \tCreated" << std::endl;
}

DiskManager::~DiskManager(){
	stop_threads_ = true;
	std::optional<struct PublishRequest> sentinel = std::nullopt;
	for (int i=0; i<num_io_threads_; i++)
		requestQueue_.blockingWrite(sentinel);

	close(log_fd_);

	for(std::thread& thread : threads_){
		if(thread.joinable()){
			thread.join();
		}
	}
	std::cout << "[DiskManager]: \tDestructed" << std::endl;
}

void DiskManager::EnqueueRequest(struct PublishRequest req){
	requestQueue_.blockingWrite(req);
}

void DiskManager::Disk_io_thread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct PublishRequest> optReq;

	while(!stop_threads_){
		requestQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		const struct PublishRequest &req = optReq.value();
		int off = offset_.fetch_add(req.size, std::memory_order_relaxed);
		pwrite(log_fd_, req.payload_address, req.size, off);

		// Post I/O work (as disk I/O depend on the same payload)
		int counter = req.counter->fetch_sub(1, std::memory_order_relaxed);
		if( counter == 1){
			//free(req.payload_address);
		}else if(req.acknowledge){
			struct NetworkRequest req;
			req.req_type = Acknowledge;
			network_manager_->EnqueueRequest(req);
			//TODO(Jae)
			//Enque ack request to network manager
			// network_manager_.EnqueueAckRequest();
		}
	}
}

} // End of namespace Embarcadero
