#include "disk_manager.h"

#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "mimalloc.h"
#include <iostream>

namespace Embarcadero{

#define DISK_LOG_PATH_SUFFIX ".Embarcadero_Replication"

void memcpy_nt(void* dst, const void* src, size_t size) {
	// Cast the input pointers to the appropriate types
	uint8_t* d = static_cast<uint8_t*>(dst);
	const uint8_t* s = static_cast<const uint8_t*>(src);

	// Align the destination pointer to 16-byte boundary
	size_t alignment = reinterpret_cast<uintptr_t>(d) & 0xF;
	if (alignment) {
		alignment = 16 - alignment;
		size_t copy_size = (alignment > size) ? size : alignment;
		std::memcpy(d, s, copy_size);
		d += copy_size;
		s += copy_size;
		size -= copy_size;
	}

	// Copy the bulk of the data using non-temporal stores
	size_t block_size = size / 64;
	for (size_t i = 0; i < block_size; ++i) {
		_mm_stream_si64(reinterpret_cast<long long*>(d), *reinterpret_cast<const long long*>(s));
		_mm_stream_si64(reinterpret_cast<long long*>(d + 8), *reinterpret_cast<const long long*>(s + 8));
		_mm_stream_si64(reinterpret_cast<long long*>(d + 16), *reinterpret_cast<const long long*>(s + 16));
		_mm_stream_si64(reinterpret_cast<long long*>(d + 24), *reinterpret_cast<const long long*>(s + 24));
		_mm_stream_si64(reinterpret_cast<long long*>(d + 32), *reinterpret_cast<const long long*>(s + 32));
		_mm_stream_si64(reinterpret_cast<long long*>(d + 40), *reinterpret_cast<const long long*>(s + 40));
		_mm_stream_si64(reinterpret_cast<long long*>(d + 48), *reinterpret_cast<const long long*>(s + 48));
		_mm_stream_si64(reinterpret_cast<long long*>(d + 56), *reinterpret_cast<const long long*>(s + 56));
		d += 64;
		s += 64;
	}

	// Copy the remaining data using standard memcpy
	std::memcpy(d, s, size % 64);
}

unsigned long default_huge_page_size(void){
	FILE *f = fopen("/proc/meminfo", "r");
	unsigned long hps = 0;
	size_t linelen = 0;
	char *line = NULL;

	if (!f)
		return 0;
	while (getline(&line, &linelen, f) > 0) {
		if (sscanf(line, "Hugepagesize:       %lu kB", &hps) == 1) {
			hps <<= 10;
			break;
		}
	}
	free(line);
	fclose(f);
	return hps;
}

#define ALIGN_UP(x, align_to)   (((x) + ((align_to)-1)) & ~((align_to)-1))

void *mmap_large_buffer(size_t need, size_t &allocated){
	void *buffer;
	size_t sz;
	size_t map_align = default_huge_page_size();
	/* Attempt to use huge pages if possible. */
	sz = ALIGN_UP(need, map_align);
	buffer = mmap(NULL, sz, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

	if (buffer == (void *)-1) {
		sz = need;
		buffer = mmap(NULL, sz, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,-1, 0);
		if (buffer != (void *)-1){
			LOG(INFO) <<"MAP_HUGETLB attempt failed, look at /sys/kernel/mm/hugepages for optimal performance";
		}else{
			LOG(ERROR) <<"mmap failed:" << strerror(errno);
			buffer = malloc(need);
			if(buffer){
			LOG(ERROR) <<"malloc failed:" << strerror(errno);
			exit(1);
			}
		}
	}

	allocated = sz;
	memset(buffer, 0, sz);
	return buffer;
}

DiskManager::DiskManager(int broker_id, void* cxl_addr, bool log_to_memory, 
												heartbeat_system::SequencerType sequencerType, size_t queueCapacity):
						 requestQueue_(queueCapacity),
						 copyQueue_(1024),
						 broker_id_(broker_id),
						 cxl_addr_(cxl_addr),
						 log_to_memory_(log_to_memory),
						 sequencerType_(sequencerType){
	num_io_threads_ = NUM_MAX_BROKERS;

	if(sequencerType == heartbeat_system::SequencerType::SCALOG){
		// TODO(Tony) Figure out how to not initialize scalog replication manager if replication factor == 0
		scalog_replication_manager_ = std::make_unique<Scalog::ScalogReplicationManager>(broker_id_, "localhost", std::to_string(SCALOG_REP_PORT + broker_id_));
		return;
	}else if(sequencerType == heartbeat_system::SequencerType::CORFU){
		corfu_replication_manager_ = std::make_unique<Corfu::CorfuReplicationManager>();
		return;
	}

	if(!log_to_memory){
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
	}

	for (size_t i=0; i< num_io_threads_; i++){
		threads_.emplace_back(&DiskManager::ReplicateThread, this);
		threads_.emplace_back(&DiskManager::CopyThread, this);
	}

	while(thread_count_.load() != num_io_threads_){std::this_thread::yield();}
	VLOG(3) << "\t[DiskManager]: \t\tConstructed";
}

DiskManager::~DiskManager(){
	stop_threads_ = true;
	std::optional<struct ReplicationRequest> sentinel = std::nullopt;
	std::optional<struct MemcpyRequest> copy_sentinel = std::nullopt;
	size_t n = num_io_threads_.load();
	for (size_t i=0; i<n; i++){
		requestQueue_.blockingWrite(sentinel);
		copyQueue_.blockingWrite(copy_sentinel);
	}

	for(std::thread& thread : threads_){
		if(thread.joinable()){
			thread.join();
		}
	}
	
	VLOG(3)<< "[DiskManager]: \tDestructed";
}

void DiskManager::CopyThread(){
	if(sequencerType_ == heartbeat_system::SequencerType::SCALOG){
		scalog_replication_manager_->Shutdown();
		return;
	}else if(sequencerType_ == heartbeat_system::SequencerType::CORFU){
		corfu_replication_manager_->Shutdown();
		return;
	}
	while(!stop_threads_){
		std::optional<MemcpyRequest> optReq;
		copyQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			return;
		}
		MemcpyRequest &req = optReq.value();
		std::memcpy(req.addr, req.buf, req.len);
	}
}

void DiskManager::Replicate(TInode* tinode, TInode* replica_tinode, int replication_factor){
	// if(sequencerType_ == heartbeat_system::SequencerType::SCALOG && replication_factor > 0 && !scalog_replication_manager_){
		// scalog_replication_manager_ = std::make_unique<Scalog::ScalogReplicationManager>(broker_id_, "localhost", std::to_string(SCALOG_REP_PORT + broker_id_));
	// }

	size_t available_threads = num_io_threads_.load() - num_active_threads_.load();
	int threads_needed = replication_factor - available_threads;
	if(threads_needed > 0){
		for(int i=0; i < threads_needed; i++){
			threads_.emplace_back(&DiskManager::ReplicateThread, this);
		}
		num_io_threads_.fetch_add(threads_needed);
		while(thread_count_.load() != num_io_threads_.load()){std::this_thread::yield();}
	}
	if(!log_to_memory_){
		fs::path log_dir = prefix_path_/tinode->topic;
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
			ReplicationRequest req = {tinode, replica_tinode, fd, b};
			requestQueue_.blockingWrite(req);
		}
	}else{
		for(int i = 0; i< replication_factor; i++){
			//TODO(Jae) get current num brokers
			int b = (broker_id_ + i)%NUM_MAX_BROKERS;
			ReplicationRequest req = {tinode, replica_tinode, -1, b};
			requestQueue_.blockingWrite(req);
		}
	}
}

// Replicate req.tinode->topic req.broker_id's log to local disk
// Runs until stop_threads_ signaled
// TODO(Jae) handle when the leader broker fails. This is why we have num_io_threads_ tracked
void DiskManager::ReplicateThread(){
	void *log;
	size_t log_size = (1<<30);
	if(log_to_memory_){
		log = mi_malloc(log_size);
	}
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct ReplicationRequest> optReq;

	requestQueue_.blockingRead(optReq);
	if(!optReq.has_value()){
		if(log_to_memory_)
			mi_free(log);
		thread_count_.fetch_sub(1);
		num_active_threads_.fetch_sub(1);
		num_io_threads_.fetch_sub(1);
		return;
	}
	num_active_threads_.fetch_add(1);
	const struct ReplicationRequest &req = optReq.value();
	size_t last_offset = 0;
	void* last_addr = nullptr;
	void* messages;
	size_t messages_size;
	int order = req.tinode->order; // Do this here to avoid accessing CXL every time GetMesgaddr called
	TInode* replica_tinode = req.replica_tinode;
	bool replicate_tinode = req.tinode->replicate_tinode;

	if(log_to_memory_){
		size_t offset = 0;
		while(!stop_threads_){
			if(GetMessageAddr(req.tinode, order, req.broker_id, last_offset, last_addr, messages, messages_size)){
				if(messages_size > (1UL<<25)){
					size_t write_granularity = (1UL<<24);
					size_t remaining = messages_size - write_granularity;
					while(remaining > 0){
						MemcpyRequest req;
						if(remaining >= write_granularity){
							req.len = write_granularity;
						}else{
							req.len = remaining;
						}
						if(offset + req.len > log_size){
							offset = 0;
							LOG(ERROR) << "Consider increasing replica log size message_size:" << messages_size;
						}
						req.addr = (void*)((uint8_t*)log + offset);
						req.buf = (void*)((uint8_t*)messages + offset);
						copyQueue_.blockingWrite(req);

						offset += req.len;
						messages_size -= req.len;
						remaining -= req.len;
					}
				}
				memcpy((uint8_t*)log + offset, messages, messages_size);
				offset = 0;
				//offset = (offset+messages_size)%log_size;
				if(replicate_tinode){
					replica_tinode->offsets[broker_id_].replication_done[req.broker_id] = last_offset;
				}
				req.tinode->offsets[broker_id_].replication_done[req.broker_id] = last_offset;
			}
		}
		close(req.fd);
	}else{
		while(!stop_threads_){
			if(GetMessageAddr(req.tinode, order, req.broker_id, last_offset, last_addr, messages, messages_size)){
				write(req.fd, messages, messages_size);
				if(replicate_tinode){
					replica_tinode->offsets[broker_id_].replication_done[req.broker_id] = last_offset;
				}
				req.tinode->offsets[broker_id_].replication_done[req.broker_id] = last_offset;
			}
		}
	}
	if(log_to_memory_)
		mi_free(log);
	thread_count_.fetch_sub(1);
	num_active_threads_.fetch_sub(1);
	num_io_threads_.fetch_sub(1);
}

//This is a copy of Topic::GetMessageAddr changed to use tinode instead of topic variables
bool DiskManager::GetMessageAddr(TInode* tinode, int order, int broker_id, size_t &last_offset,
		void* &last_addr, void* &messages, size_t &messages_size){
	size_t relative_off = tinode->offsets[broker_id].written_addr;;
	if(relative_off == 0)
		return false;
	void* combined_addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(cxl_addr_) + relative_off);
	size_t combined_offset = ((MessageHeader*)combined_addr)->logical_offset;//tinode->offsets[broker_id].written;
	//size_t combined_offset = tinode->offsets[broker_id].written;

	if(order > 0){
		if(tinode->offsets[broker_id].ordered_offset == 0){
			return false;
		}
		combined_addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(cxl_addr_) + tinode->offsets[broker_id].ordered_offset);
		combined_offset = ((MessageHeader*)combined_addr)->logical_offset;//tinode->offsets[broker_id].ordered;
	}

	if(combined_offset == (size_t)-1 || ((last_addr != nullptr) && (combined_offset <= last_offset))){
		return false;
	}

	struct MessageHeader *start_msg_header = (struct MessageHeader*)last_addr;
	if(last_addr != nullptr){
		while(start_msg_header->next_msg_diff == 0){
			LOG(INFO) << "[GetMessageAddr] waiting for the message to be combined " << start_msg_header->logical_offset
				<< " cxl_addr:" << cxl_addr_  << " +  relative addr:" << relative_off
				<< " = combined_addr:" << reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(cxl_addr_) + relative_off)
				<< " combined_addr:" << combined_addr
				<< " combined_offset:" << combined_offset << " combined_from_addr:" << ((MessageHeader*)combined_addr)->logical_offset;
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

} // End of namespace Embarcadero
