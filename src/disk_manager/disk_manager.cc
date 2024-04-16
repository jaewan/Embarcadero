#include "disk_manager.h"

#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <glog/logging.h>
#include "../network_manager/request_data.h"

namespace Embarcadero{

#define DISK_LOG_PATH "embarc.disklog"

DiskManager::DiskManager(std::shared_ptr<AckQueue> ack_queue, std::shared_ptr<ReqQueue> req_queue, int num_io_threads):
						ackQueue_(ack_queue),
						 reqQueue_(req_queue),
						 num_io_threads_(num_io_threads){
	//Initialize log file
	log_fd_ = open(DISK_LOG_PATH, O_RDWR|O_CREAT, 0777);
	if (log_fd_ < 0){
		perror("Error in opening a file for disk log\n");
		LOG(ERROR) << strerror(errno);
	}
	// Create Disk I/O threads
	for (int i=0; i< num_io_threads_; i++)
		threads_.emplace_back(&DiskManager::DiskIOThread, this);

	while(thread_count_.load() != num_io_threads_){}
	LOG(INFO) << "[DiskManager] Created";
}

DiskManager::~DiskManager(){
	stop_threads_ = true;
	std::optional<struct PublishRequest> sentinel = std::nullopt;
	for (int i=0; i<num_io_threads_; i++)
		reqQueue_->blockingWrite(sentinel);

	close(log_fd_);

	for(std::thread& thread : threads_){
		if(thread.joinable()){
			thread.join();
		}
	}
	LOG(INFO) << "[DiskManager] Destructed";
}

void DiskManager::DiskIOThread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct PublishRequest> optReq;

	while(!stop_threads_){
		reqQueue_->blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		struct PublishRequest &req = optReq.value();
		struct RequestData *req_data = static_cast<RequestData*>(req.grpcTag);

		int off = offset_.fetch_add(req_data->request_.payload_size(), std::memory_order_relaxed);
		pwrite(log_fd_, req_data->request_.payload().c_str(), req_data->request_.payload_size(), off);

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
			} else {
				// gRPC has already sent response, so just mark the object as ready for destruction
				DLOG(INFO) << "!!!!!!!!!!!!! This should not happen";
				//req_data->Proceed();
			}
		} else {
			//delete req.counter;
			//DLOG(INFO) << "counter: " << counter;
		}
	}
}

} // End of namespace Embarcadero
