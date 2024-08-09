#include "disk_manager.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "mimalloc.h"

namespace Embarcadero{

#define DISK_LOG_PATH "embarc.disklog"

DiskManager::DiskManager(size_t queueCapacity, 
						 int num_io_threads):
						 requestQueue_(queueCapacity),
						 num_io_threads_(num_io_threads){
	//Initialize log file
	log_fd_ = open(DISK_LOG_PATH, O_RDWR|O_CREAT, 0777);
	if (log_fd_ < 0){
		LOG(ERROR) << "Error in opening a file for disk log:" <<strerror(errno);
	}
	// Create Disk I/O threads
	for (int i=0; i< num_io_threads_; i++)
		threads_.emplace_back(&DiskManager::DiskIOThread, this);

	while(thread_count_.load() != num_io_threads_){}
	LOG(INFO) << "\t[DiskManager]: \t\tConstructed";
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
	LOG(INFO)<< "[DiskManager]: \tDestructed";
}

void DiskManager::EnqueueRequest(struct PublishRequest req){
	requestQueue_.blockingWrite(req);
}

void DiskManager::DiskIOThread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct PublishRequest> optReq;

	while(!stop_threads_){
		requestQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		const struct PublishRequest &req = optReq.value();
		int off = offset_.fetch_add(req.paddedSize, std::memory_order_relaxed);
		pwrite(log_fd_, req.payload_address, req.paddedSize, off);

		// Post I/O work (as disk I/O depend on the same payload)
		int counter = req.counter->fetch_sub(1,std::memory_order_release);
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

} // End of namespace Embarcadero
