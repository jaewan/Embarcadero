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
		int off = offset_.fetch_add(req.req->payload_size(), std::memory_order_relaxed);
		std::cout << "[DiskManager]: Received payload is: " << req.req->payload().c_str() << std::endl;
		pwrite(log_fd_, req.req->payload().c_str(), req.req->payload_size(), off);

		// Post I/O work (as disk I/O depend on the same payload)
		int counter = req.counter->fetch_sub(1, std::memory_order_relaxed);

		// If no more tasks are left to do
		if (counter == 0) {
			if (req.req->acknowledge()) {
				// TODO: Set result - just assume success
				network_manager_->SetError(req.grpcTag, ERR_NO_ERROR);

				// Send to network manager ack queue
				struct NetworkRequest ack_req;
				ack_req.req_type = Acknowledge;
				ack_req.grpcTag = req.grpcTag;
				printf("DiskManager enquing to ack queue, tag=%p\n", ack_req.grpcTag);
				network_manager_->EnqueueAck(ack_req);
			} else {
				// gRPC has already sent response, so here we can just free the CallData object.
				std::cout << "DiskManager calling proceed on call data" << std::endl;
				network_manager_->Proceed(req.grpcTag);
			}
		} else {
			std::cout << "DiskManager got counter: " << counter << std::endl;
		}
	}
}

} // End of namespace Embarcadero
