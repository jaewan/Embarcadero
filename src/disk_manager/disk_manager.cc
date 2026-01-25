#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <iostream>
#include <chrono>

#include "mimalloc.h"

#include "disk_manager.h"
#include "scalog_replication_manager.h"
#include "corfu_replication_manager.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "../common/performance_utils.h"

namespace Embarcadero{

#define DISK_LOG_PATH_SUFFIX ".Replication/disk"

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
				buffer = mi_malloc(need);
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
				scalog_replication_manager_ = std::make_unique<Scalog::ScalogReplicationManager>(broker_id_, log_to_memory, "localhost", std::to_string(SCALOG_REP_PORT + broker_id_));
				return;
			}else if(sequencerType == heartbeat_system::SequencerType::CORFU){
				corfu_replication_manager_ = std::make_unique<Corfu::CorfuReplicationManager>(broker_id, log_to_memory);
				return;
			}

			if(!log_to_memory){
				const char *homedir;
				if ((homedir = getenv("HOME")) == NULL) {
					homedir = getpwuid(getuid())->pw_dir;
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
		if(sequencerType_ == heartbeat_system::SequencerType::SCALOG && scalog_replication_manager_){
			scalog_replication_manager_->Shutdown();
			return;
		}else if(sequencerType_ == heartbeat_system::SequencerType::CORFU){
			corfu_replication_manager_->Shutdown();
			return;
		}
		if(log_to_memory_){
			while(!stop_threads_){
				std::optional<MemcpyRequest> optReq;
				copyQueue_.blockingRead(optReq);
				if(!optReq.has_value()){
					return;
				}
				MemcpyRequest &req = optReq.value();
				std::memcpy(req.addr, req.buf, req.len);
			}
		}else{
			while(!stop_threads_){
				std::optional<MemcpyRequest> optReq;
				copyQueue_.blockingRead(optReq);
				if(!optReq.has_value()){
					return;
				}
				MemcpyRequest &req = optReq.value();
				pwrite(req.fd, req.buf, req.offset, req.len);
			}
		}
	}

	void DiskManager::Replicate(TInode* tinode, TInode* replica_tinode, int replication_factor){
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
			for(int i = 0; i< replication_factor; i++){
				int b = (broker_id_ + i)%NUM_MAX_BROKERS;
				int disk_to_write = b % NUM_DISKS ;
				std::string base_dir = "../../.Replication/disk" + std::to_string(disk_to_write) + "/";
				std::string base_filename = base_dir+"embarcadero_replication_log"+std::to_string(b) +".dat";
				int fd = open(base_filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
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
		thread_count_.fetch_add(1, std::memory_order_relaxed);
		std::optional<struct ReplicationRequest> optReq;

		requestQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			thread_count_.fetch_sub(1);
			return;
		}

		num_active_threads_.fetch_add(1);
		const struct ReplicationRequest &req = optReq.value();

		void *log_addr = nullptr;
		size_t log_capacity = (1UL<<30);
		int fd = req.fd;

		if(log_to_memory_){
			log_addr = mi_malloc(log_capacity);
		}

		// Common variables for both memory and disk paths
		size_t current_offset = 0; // Current write position within log_addr
		int order = req.tinode->order;
		TInode* replica_tinode = req.replica_tinode;
		bool replicate_tinode = req.tinode->replicate_tinode;
		size_t offset = 0;
		size_t disk_offset = 0;
		
		// [[EXPLICIT_REPLICATION_STAGE4]] - Batch-based replication variables
		BatchHeader* batch_ring_start = nullptr;
		BatchHeader* batch_ring_end = nullptr;
		BatchHeader* current_batch = nullptr;
		void* batch_payload = nullptr;
		size_t batch_payload_size = 0;
		size_t batch_start_logical_offset = 0;
		size_t batch_last_logical_offset = 0;
		
		// Periodic durability sync state
		size_t bytes_since_sync = 0;
		auto last_sync_time = std::chrono::steady_clock::now();
		constexpr size_t kSyncBytesThreshold = 64 * 1024 * 1024; // 64 MiB
		constexpr auto kSyncTimeThreshold = std::chrono::milliseconds(250); // 250 ms

		while (!stop_threads_) {
			// [[EXPLICIT_REPLICATION_STAGE4]] - Use batch-based replication for all orders
			// This works with ORDER=5 (batches) and older ORDER levels (message-based converted to batches)
			if (GetNextReplicationBatch(req.tinode, req.broker_id,
					batch_ring_start, batch_ring_end, current_batch, disk_offset,
					batch_payload, batch_payload_size,
					batch_start_logical_offset, batch_last_logical_offset)) {
				
				// Write batch payload to disk
				if (batch_payload_size > 0) {
					if(log_to_memory_){
						memcpy((uint8_t*)log_addr + offset, batch_payload, batch_payload_size);
						offset += batch_payload_size;
						if (offset > log_capacity) offset = 0;
					}else{
						ssize_t written = pwrite(fd, batch_payload, batch_payload_size, disk_offset);
						if (written <= 0) {
							LOG(ERROR) << "pwrite failed for batch in broker " << req.broker_id << ": " << strerror(errno);
							CXL::cpu_pause();
							continue;
						}
						bytes_since_sync += written;
					}
					disk_offset += batch_payload_size;
				}
				
				// Check if periodic sync is needed
				if (!log_to_memory_) {
					auto now = std::chrono::steady_clock::now();
					bool sync_needed = (bytes_since_sync >= kSyncBytesThreshold) ||
					                   (now - last_sync_time >= kSyncTimeThreshold);
					
					if (sync_needed && bytes_since_sync > 0) {
						if (fdatasync(fd) < 0) {
							LOG(ERROR) << "fdatasync failed for broker " << req.broker_id << ": " << strerror(errno);
						}
						bytes_since_sync = 0;
						last_sync_time = now;
					}
				}
				
				// Update replication_done to signal ACK level 2
				// [[EXPLICIT_REPLICATION_STAGE4]] - Flush after replication_done update
				if(replicate_tinode){
					replica_tinode->offsets[broker_id_].replication_done[req.broker_id] = batch_last_logical_offset;
				}
				req.tinode->offsets[broker_id_].replication_done[req.broker_id] = batch_last_logical_offset;
				
				// Flush the updated replication_done so other hosts (ACK thread) can observe it under non-coherent CXL
				const void* rep_done_addr = reinterpret_cast<const void*>(
					const_cast<const int*>(&req.tinode->offsets[broker_id_].replication_done[req.broker_id]));
				CXL::flush_cacheline(rep_done_addr);
				CXL::store_fence();
				
				VLOG(3) << "[ReplicationThread B" << req.broker_id << "]: Replicated batch, last_offset=" 
						<< batch_last_logical_offset << ", disk_offset=" << disk_offset;
			} else {
				CXL::cpu_pause();
			}
		} // End while(!stop_threads_)
		
		// Final sync if needed
		if (!log_to_memory_ && bytes_since_sync > 0) {
			if (fdatasync(fd) < 0) {
				LOG(WARNING) << "Final fdatasync failed for broker " << req.broker_id << ": " << strerror(errno);
			}
		}

		// --- Cleanup ---
		VLOG(1) << "[ReplicateThread " << req.broker_id << "]: Stopping replication loop.";

		// Cleanup based on log type
		if (!log_to_memory_) {
			// Disk (mmap) path cleanup
			if (log_addr != nullptr && log_addr != MAP_FAILED) {
				VLOG(2) << "[ReplicateThread " << req.broker_id << "]: Syncing mmaped file.";
				// Ensure data is written to disk before closing
				if (msync(log_addr, current_offset, MS_SYNC) == -1) {
					LOG(ERROR) << "Failed to msync log file for target " << req.broker_id << ": " << strerror(errno);
				}
			}
			close(fd);
		} else {
			// Memory path cleanup
			if (log_addr != nullptr) {
				VLOG(2) << "[ReplicateThread " << req.broker_id << "]: Freeing memory log.";
				mi_free(log_addr); // Use the corresponding free function for mi_malloc
			}
			// req.fd should be -1 for memory path, no need to close
		}

		// Decrement counters (ensure this happens exactly once per thread exit)
		thread_count_.fetch_sub(1);
		num_active_threads_.fetch_sub(1);
	}

	/**
	 * @brief Get next batch to replicate from CXL BatchHeader ring
	 * 
	 * [[EXPLICIT_REPLICATION_STAGE4]] - Batch-based replication
	 * Polls the BatchHeader ring for ordered batches (ordered == 1)
	 * Returns batch metadata and payload location in CXL
	 * 
	 * @threading Called by single ReplicateThread per primary broker
	 * @ownership batch_ring_start/end allocated by GetNewBatchHeaderLog(), caller manages cursor
	 * @alignment BatchHeader is 64-byte aligned
	 * @paper_ref Paper §3.4 - Stage 4: Replication threads poll ordered batches
	 * 
	 * @return true if valid ordered batch found, false if no batch ready or cursor exhausted
	 */
	bool DiskManager::GetNextReplicationBatch(TInode* tinode, int broker_id,
			BatchHeader* &batch_ring_start, BatchHeader* &batch_ring_end,
			BatchHeader* &current_batch, size_t &disk_offset,
			void* &batch_payload, size_t &batch_payload_size,
			size_t &batch_start_logical_offset, size_t &batch_last_logical_offset) {
		
		// Initialize ring pointers on first call
		if (batch_ring_start == nullptr) {
			batch_ring_start = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + tinode->offsets[broker_id].batch_headers_offset);
			batch_ring_end = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(batch_ring_start) + BATCHHEADERS_SIZE);
			current_batch = batch_ring_start;
			disk_offset = 0;
			VLOG(2) << "[GetNextReplicationBatch B" << broker_id << "]: Initialized ring at offset " 
					<< tinode->offsets[broker_id].batch_headers_offset;
		}
		
		// Scan for next ordered batch (match BrokerScannerWorker5 pattern)
		// Use a reasonable upper bound to prevent infinite loops
		size_t MAX_SCAN_ATTEMPTS = 10000;  // Up to ~10k batches per scan
		for (size_t i = 0; i < MAX_SCAN_ATTEMPTS; ++i) {
			// Read batch header fields as volatile (sender writes from remote broker)
			volatile uint32_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch)->num_msg;
			volatile uint32_t ordered_check = reinterpret_cast<volatile BatchHeader*>(current_batch)->ordered;
			volatile size_t total_size_check = reinterpret_cast<volatile BatchHeader*>(current_batch)->total_size;
			volatile size_t log_idx_check = reinterpret_cast<volatile BatchHeader*>(current_batch)->log_idx;
			volatile size_t start_logical_offset_check = reinterpret_cast<volatile BatchHeader*>(current_batch)->start_logical_offset;
			
			// Bounds validation (match BrokerScannerWorker5 guards)
			constexpr uint32_t MAX_REASONABLE_NUM_MSG = 100000;
			bool batch_ready = (num_msg_check != 0 && 
			                   ordered_check == 1 && 
			                   total_size_check > 0 && 
			                   log_idx_check > 0 && 
			                   num_msg_check <= MAX_REASONABLE_NUM_MSG);
			
			if (batch_ready) {
				// Batch is ordered and valid
				batch_payload = reinterpret_cast<uint8_t*>(cxl_addr_) + log_idx_check;
				batch_payload_size = total_size_check;
				batch_start_logical_offset = start_logical_offset_check;
				batch_last_logical_offset = start_logical_offset_check + num_msg_check - 1;
				
				VLOG(3) << "[GetNextReplicationBatch B" << broker_id << "]: Found ordered batch: "
						<< "num_msg=" << num_msg_check << ", log_idx=" << log_idx_check 
						<< ", payload_size=" << batch_payload_size
						<< ", start_offset=" << batch_start_logical_offset;
				
				// Advance cursor for next iteration
				BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
				if (next_batch >= batch_ring_end) {
					next_batch = batch_ring_start;
				}
				current_batch = next_batch;
				return true;
			}
			
			// Current batch not ready, advance and try next
			BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
			if (next_batch >= batch_ring_end) {
				next_batch = batch_ring_start;
			}
			current_batch = next_batch;
		}
		
		// No ordered batch found in scan range
		return false;
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

	void DiskManager::StartScalogReplicaLocalSequencer() {
		scalog_replication_manager_->StartSendLocalCut();
	}

} // End of namespace Embarcadero
