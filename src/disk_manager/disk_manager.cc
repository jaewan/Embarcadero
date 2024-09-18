#include "disk_manager.h"

#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "mimalloc.h"
#include <iostream>

namespace Embarcadero{

#define DISK_LOG_PATH_SUFFIX ".Embarcadero_Replication"

DiskManager::DiskManager(size_t queueCapacity, int broker_id, void* cxl_addr,
						 int num_io_threads):
						 requestQueue_(queueCapacity),
						 broker_id_(broker_id),
						 cxl_addr_(cxl_addr),
						 num_io_threads_(num_io_threads){
	//TODO(Jae) this onlye works at single topic upto replication fator of all, change this later
	num_io_threads_ = NUM_MAX_BROKERS;
	const char *homedir;
	if ((homedir = getenv("HOME")) == NULL) {
		homedir = getpwuid(getuid())->pw_dir;
	}

	prefix_path_ = fs::path(homedir) / DISK_LOG_PATH_SUFFIX / std::to_string(broker_id_);
	if (!fs::exists(prefix_path_)) {
		if (!fs::create_directories(prefix_path_)) {
			LOG(ERROR) << "Error: Unable to create directory " << prefix_path_ << " :" <<strerror(errno);
			return ;
		}
	}

	// Create Disk I/O threads
	for (int i=0; i< num_io_threads_; i++)
		threads_.emplace_back(&DiskManager::DiskIOThread, this);

	while(thread_count_.load() != num_io_threads_){}
	LOG(INFO) << "\t[DiskManager]: \t\tConstructed";
}

DiskManager::~DiskManager(){
	stop_threads_ = true;
	std::optional<struct ReplicationRequest> sentinel = std::nullopt;
	for (int i=0; i<num_io_threads_; i++)
		requestQueue_.blockingWrite(sentinel);

	for(std::thread& thread : threads_){
		if(thread.joinable()){
			thread.join();
		}
	}
	LOG(INFO)<< "[DiskManager]: \tDestructed";
}

void DiskManager::Replicate(TInode* tinode, int replication_factor){
	fs::path log_dir = prefix_path_/tinode->topic;
	VLOG(3) << "Logging to:" << log_dir;
	if (!fs::exists(log_dir)) {
		if (!fs::create_directory(log_dir)) {
			LOG(ERROR) << "Error: Unable to create directory " << log_dir << " : " << strerror(errno);
			return ;
		}
	}

	for(int i = 0; i< replication_factor; i++){
		int b = (broker_id_ + i)%NUM_MAX_BROKERS;
		fs::path log = log_dir/std::to_string(b);
		int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if(fd == -1){
			LOG(ERROR) << "File open for replication failed:" << strerror(errno);
		}
		ReplicationRequest req = {tinode, fd, b};
		requestQueue_.blockingWrite(req);
	}

}

//This is a copy of Topic::GetMessageAddr changed to use tinode instead of topic variables
bool DiskManager::GetMessageAddr(TInode* tinode, int order, int broker_id, size_t &last_offset,
		void* &last_addr, void* &messages, size_t &messages_size){
	void* combined_addr = (void*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id].written_addr);
	//size_t combined_offset = ((MessageHeader*)combined_addr)->logical_offset;//tinode->offsets[broker_id].written;
	size_t combined_offset = tinode->offsets[broker_id].written;

	if(order > 0){
		combined_addr = (uint8_t*)cxl_addr_ + tinode->offsets[broker_id].ordered_offset;
		combined_offset = ((MessageHeader*)combined_addr)->logical_offset;//tinode->offsets[broker_id].ordered;
	}

	if(combined_offset == (size_t)-1 || ((last_addr != nullptr) && (combined_offset <= last_offset))){
		return false;
	}

	struct MessageHeader *start_msg_header = (struct MessageHeader*)last_addr;
	if(last_addr != nullptr){
		while(start_msg_header->next_msg_diff == 0){
			LOG(INFO) << "[GetMessageAddr] waiting for the message to be combined " << start_msg_header->logical_offset
				<< " last_offset:" << combined_offset << "paddedSize:" << start_msg_header->paddedSize 
				<< " segment:" << start_msg_header->segment_header << " client_order:" << start_msg_header->client_id
				<< " broker:" << (broker_id == broker_id_) << " complete:" << start_msg_header->complete;
			std::this_thread::yield();
			sleep(3);
		}
		start_msg_header = (struct MessageHeader*)((uint8_t*)start_msg_header + start_msg_header->next_msg_diff);
	}else{
		//TODO(Jae) this is only true in a single segment setup
		if(combined_addr <= last_addr){
			LOG(ERROR) << "[GetMessageAddr] Wrong!!";
			return false;
		}
		start_msg_header = (struct MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id].log_offset);
	}

	if(start_msg_header->paddedSize == 0){
		return false;
	}

	messages = (void*)start_msg_header;

#ifdef MULTISEGMENT
	//TODO(Jae) use relative addr here for multi-node
	unsigned long long int* last_msg_off = (unsigned long long int*)start_msg_header->segment_header;
	struct MessageHeader *last_msg_of_segment = (MessageHeader*)((uint8_t*)last_msg_off + *last_msg_off);

	if(combined_addr < last_msg_of_segment){ // last msg is not ordered yet
		messages_size = (uint8_t*)combined_addr - (uint8_t*)start_msg_header + ((MessageHeader*)combined_addr)->paddedSize; 
		last_offset = ((MessageHeader*)combined_addr)->logical_offset;
		last_addr = (void*)combined_addr;
	}else{
		messages_size = (uint8_t*)last_msg_of_segment - (uint8_t*)start_msg_header + last_msg_of_segment->paddedSize; 
		last_offset = last_msg_of_segment->logical_offset;
		last_addr = (void*)last_msg_of_segment;
	}
#else
	messages_size = (uint8_t*)combined_addr - (uint8_t*)start_msg_header + ((MessageHeader*)combined_addr)->paddedSize; 
	last_offset = ((MessageHeader*)combined_addr)->logical_offset;
	last_addr = (void*)combined_addr;
#endif
	return true;
}

void DiskManager::DiskIOThread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct ReplicationRequest> optReq;

	requestQueue_.blockingRead(optReq);
	if(!optReq.has_value()){
		return;
	}
	const struct ReplicationRequest &req = optReq.value();
	size_t last_offset = 0;
	void* last_addr = nullptr;
	void* messages;
	size_t messages_size;
	int order = req.tinode->order; // Do this here to avoid accessing CXL every time GetMesgaddr called
	while(!stop_threads_){
		if(GetMessageAddr(req.tinode, order, req.broker_id, last_offset, last_addr, messages, messages_size)){
			write(req.fd, messages, messages_size);
			req.tinode->offsets[broker_id_].replication_done[req.broker_id] = last_offset;
		}
	}
	close(req.fd);
}

} // End of namespace Embarcadero
