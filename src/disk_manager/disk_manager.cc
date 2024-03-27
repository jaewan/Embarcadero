#include "disk_manager.h"
#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

namespace Embarcadero{

#define DISK_LOG_PATH "~/Embarcadero/.DiskLog/log"
#define NUM_ACTIVE_POLL 100
#define NUM_DISK_IO_THREADS 4

DiskManager::DiskManager(size_t queueCapacity):requests_(queueCapacity){
	//Initialize log file
	log_fd_ = open(DISK_LOG_PATH, O_RDWR|O_CREAT, 0777);
	if (log_fd_ < 0){
		perror("Error in opening a file for disk log\n");
		std::cout<< strerror(errno) << std::endl;
	}

	// Create Disk I/O threads
	for (int i=0; i< NUM_DISK_IO_THREADS; i++)
		threads_.emplace_back(&DiskManager::Disk_io_thread, this);

	while(thread_count_.load() != NUM_DISK_IO_THREADS){}
	std::cout << "[DiskManager]: \tCreated" << std::endl;
}

DiskManager::~DiskManager(){
	stop_threads_ = true;

	close(log_fd_);

	for(std::thread& thread : threads_){
		if(thread.joinable()){
			thread.join();
		}
	}
	std::cout << "[DiskManager]: \tDestructed" << std::endl;
}

DiskManager::Disk_io_thread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	int loop_count = 0;

	while(!stop_threads_){
		loop_count++;
		struct PublishRequest req;
		if(requests_.readIfNotEmpty(req)){
			int off = offset_.fetch_add(req.size, std::memory_order_relaxed);
			pwrite(log_fd_, req.payload_address, req.size, off);
			loop_count = 0;
		}else{
			// Active polling phase
			bool popped_reqeust = false;
			int num_try = 0;
			while(!stop_threads_ && !popped_reqeust && num_try < NUM_ACTIVE_POLL){
				if(queues.readIfNotEmpty(req)){
					int off = offset_.fetch_add(req.size, std::memory_order_relaxed);
					pwrite(log_fd_, req.payload_address, req.size, off);
					popped_reqeust = true;
					loop_count = 0;
				}else{
					std::this_thread::yield(); // Yield to reduce busy-waiting impact
				}
			}

			if (!popped_request) {
					// Wait phase: no tasks found, wait on a baton for a brief period
					baton_.try_wait_for(std::chrono::milliseconds(loop_count*10));
					baton_.reset();
			}
		}

		// Post I/O work (as disk I/O depend on the same payload)
		if( loop_count == 0){
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
}

} // End of namespace Embarcadero
