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
#include "chain_replication.h"
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
			
			// [[OBSERVABILITY]] - Initialize replication metrics for all brokers
			for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
				replication_metrics_[i].batches_scanned.store(0, std::memory_order_relaxed);
				replication_metrics_[i].batches_replicated.store(0, std::memory_order_relaxed);
				replication_metrics_[i].pwrite_retries.store(0, std::memory_order_relaxed);
				replication_metrics_[i].pwrite_errors.store(0, std::memory_order_relaxed);
				replication_metrics_[i].last_replication_done.store(0, std::memory_order_relaxed);
				replication_metrics_[i].last_advance_time = std::chrono::steady_clock::now();
			}
			if(sequencerType == heartbeat_system::SequencerType::SCALOG){
				scalog_replication_manager_ = std::make_unique<Scalog::ScalogReplicationManager>(broker_id_, log_to_memory, "localhost", std::to_string(SCALOG_REP_PORT + broker_id_));
				return;
			}else if(sequencerType == heartbeat_system::SequencerType::CORFU){
				corfu_replication_manager_ = std::make_unique<Corfu::CorfuReplicationManager>(broker_id, log_to_memory);
				return;
			}else if(sequencerType == heartbeat_system::SequencerType::EMBARCADERO){
				// [[PHASE_2]] Chain replication with GOI + CompletionVector
				// Configuration via environment variables:
				// - EMBARCADERO_REPLICA_ID: This broker's position in replication chain (0=head, f=tail)
				// - EMBARCADERO_REPLICATION_FACTOR: Total number of replicas (including head)
				// - EMBARCADERO_REPLICA_DISK_PATH: Disk path for replica data (optional)

				const char* replica_id_env = getenv("EMBARCADERO_REPLICA_ID");
				const char* replication_factor_env = getenv("EMBARCADERO_REPLICATION_FACTOR");
				const char* disk_path_env = getenv("EMBARCADERO_REPLICA_DISK_PATH");

				int replica_id = replica_id_env ? atoi(replica_id_env) : 0;
				// Default 1 so single-replica (head-only) is the tail and updates CompletionVector; ack_level=2 works without env.
				int replication_factor = replication_factor_env ? atoi(replication_factor_env) : 1;

				// Default disk path: /tmp/embarcadero_replica_<broker_id>.dat
				std::string disk_path;
				if (disk_path_env) {
					disk_path = disk_path_env;
				} else {
					disk_path = "/tmp/embarcadero_replica_" + std::to_string(broker_id_) + ".dat";
				}

				// Calculate GOI and CV addresses from cxl_addr_
				GOIEntry* goi = reinterpret_cast<GOIEntry*>(
					reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kGOIOffset);
				CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
					reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kCompletionVectorOffset);

				chain_replication_manager_ = std::make_unique<Embarcadero::ChainReplicationManager>(
					replica_id, replication_factor, cxl_addr_, goi, cv, disk_path);
				chain_replication_manager_->Start();

				LOG(INFO) << "DiskManager: Chain replication enabled (replica_id=" << replica_id
				          << ", replication_factor=" << replication_factor
				          << ", disk=" << disk_path << ")";
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

	// [[LEGACY_CODE]] CopyThread appears unused in current Stage-4 batch-based replication
	// No producers write to copyQueue_ except sentinel values for shutdown
	// Kept for potential future use or compatibility with older replication paths
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
				// [[FIX-PWRITE-ARGS]] - Correct argument order: pwrite(fd, buf, count, offset)
				// Previous bug: pwrite(fd, buf, offset, len) was incorrect
				pwrite(req.fd, req.buf, req.len, req.offset);
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
		// [[PHASE_3_ALIGN_REPLICATION_SET]] - Use canonical replication set computation
		// replication_factor INCLUDES self (replication_factor=1 means local durability only)
		// This ensures consistent replica selection across DiskManager and NetworkManager
		// TODO: Get actual num_brokers instead of NUM_MAX_BROKERS (requires callback)
		int num_brokers = NUM_MAX_BROKERS;  // Temporary: use MAX until we have get_num_brokers callback
		
		if(!log_to_memory_){
			for(int i = 0; i< replication_factor; i++){
				int b = Embarcadero::GetReplicationSetBroker(broker_id_, replication_factor, num_brokers, i);
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
				int b = Embarcadero::GetReplicationSetBroker(broker_id_, replication_factor, num_brokers, i);
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
		
		// [[PHASE_0_INSTRUMENTATION]] - Log replication thread startup
		LOG(INFO) << "[ReplicateThread]: Starting replication for broker_id=" << req.broker_id
			<< " (replicating broker " << req.broker_id << "'s log)";

		void *log_addr = nullptr;
		size_t log_capacity = (1UL<<30);
		int fd = req.fd;

		if(log_to_memory_){
			log_addr = mi_malloc(log_capacity);
		}

		// Common variables for both memory and disk paths
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
		
		// [[PERF_CLEANUPS]] - Scan backoff state (spin→sleep pattern for low-load efficiency)
		constexpr auto kSpinDuration = std::chrono::microseconds(100);  // Spin for 100us
		constexpr auto kSleepDuration = std::chrono::milliseconds(1);    // Then sleep for 1ms
		
		// [[OBSERVABILITY]] - Metrics tracking and periodic logging
		ReplicationMetrics& metrics = replication_metrics_[req.broker_id];
		auto last_metrics_log_time = std::chrono::steady_clock::now();
		constexpr auto kMetricsLogInterval = std::chrono::seconds(10); // Log metrics every 10 seconds

		while (!stop_threads_) {
			// [[EXPLICIT_REPLICATION_STAGE4]] - Use batch-based replication for all orders
			// This works with ORDER=5 (batches) and older ORDER levels (message-based converted to batches)
			if (GetNextReplicationBatch(req.tinode, req.broker_id,
					batch_ring_start, batch_ring_end, current_batch, disk_offset,
					batch_payload, batch_payload_size,
					batch_start_logical_offset, batch_last_logical_offset)) {
				
				// Flag to track if batch write succeeded
				bool batch_write_success = true;
				
				// Write batch payload to disk (with proper short-write handling)
				if (batch_payload_size > 0) {
					if(log_to_memory_){
						memcpy((uint8_t*)log_addr + offset, batch_payload, batch_payload_size);
						offset += batch_payload_size;
						if (offset > log_capacity) offset = 0;
					}else{
						// [[FIX-SHORT-WRITES]] - Handle partial writes and EINTR properly
						size_t bytes_written_total = 0;
						while (bytes_written_total < batch_payload_size) {
							size_t bytes_remaining = batch_payload_size - bytes_written_total;
							const uint8_t* src = reinterpret_cast<const uint8_t*>(batch_payload) + bytes_written_total;
							off_t current_pos = disk_offset + bytes_written_total;
							
							ssize_t written = pwrite(fd, src, bytes_remaining, current_pos);
							
							if (written < 0) {
								// Error on pwrite
							if (errno == EINTR) {
								// Interrupted by signal; retry
								metrics.pwrite_retries.fetch_add(1, std::memory_order_relaxed);
								VLOG(2) << "[ReplicateThread B" << req.broker_id << "]: pwrite interrupted (EINTR), retrying";
								continue;
							} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
								// Non-blocking I/O would block; backoff and retry
								metrics.pwrite_retries.fetch_add(1, std::memory_order_relaxed);
								VLOG(2) << "[ReplicateThread B" << req.broker_id << "]: pwrite would block (EAGAIN), backing off";
								std::this_thread::sleep_for(std::chrono::microseconds(100));
								continue;
								} else {
									// Permanent error; fail-fast and exit thread
									metrics.pwrite_errors.fetch_add(1, std::memory_order_relaxed);
									LOG(ERROR) << "[ReplicateThread B" << req.broker_id << "]: pwrite PERMANENT ERROR at offset " 
										<< current_pos << " (written " << bytes_written_total << "/" << batch_payload_size 
										<< " bytes): " << strerror(errno);
									LOG(ERROR) << "[ReplicateThread B" << req.broker_id << "]: Exiting replication thread due to permanent disk error";
									// Set flag to exit outer loop
									batch_write_success = false;
									CXL::cpu_pause();
									break;  // Exit write loop
								}
							} else if (written == 0) {
								// No bytes written, but no error; this is unusual but might happen
								LOG(WARNING) << "[ReplicateThread B" << req.broker_id << "]: pwrite returned 0";
								// Backoff and retry
								std::this_thread::sleep_for(std::chrono::microseconds(100));
								continue;
							} else {
								// Partial or full write succeeded
								bytes_written_total += written;
								bytes_since_sync += written;
							}
						}
					}
				}
				
				// CRITICAL: Only advance disk_offset after successful write
				if (batch_write_success && batch_payload_size > 0) {
					disk_offset += batch_payload_size;
				}
				
			// Only proceed with sync and replication_done update if write succeeded
			if (batch_write_success) {
				// [[PERIODIC_DURABILITY_SYNC]] - Periodic fdatasync policy for ack_level=2
				// Syncs to disk when either:
				// - 64 MiB written since last sync (kSyncBytesThreshold)
				// - 250ms elapsed since last sync (kSyncTimeThreshold)
				// This provides "eventual durability" semantics for ack_level=2
				// Note: For stronger guarantees, consider configurable thresholds or explicit fsync() per batch
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
				// Only advance replication_done after full batch is successfully written to disk
				TInode* replica_tinode = req.replica_tinode;
				bool replicate_tinode = req.tinode->replicate_tinode;
				if(replicate_tinode){
					replica_tinode->offsets[broker_id_].replication_done[req.broker_id] = batch_last_logical_offset;
				}
				req.tinode->offsets[broker_id_].replication_done[req.broker_id] = batch_last_logical_offset;
				
				// Flush the updated replication_done so other hosts (ACK thread) can observe it under non-coherent CXL
				const void* rep_done_addr = reinterpret_cast<const void*>(
					const_cast<const uint64_t*>(&req.tinode->offsets[broker_id_].replication_done[req.broker_id]));
				CXL::flush_cacheline(rep_done_addr);
				
				// [[FLUSH_DUAL_WRITE]] - If dual-writing to replica_tinode, flush that too for CXL visibility
				if(replicate_tinode){
					const void* replica_rep_done_addr = reinterpret_cast<const void*>(
						const_cast<const uint64_t*>(&replica_tinode->offsets[broker_id_].replication_done[req.broker_id]));
					CXL::flush_cacheline(replica_rep_done_addr);
				}
				
				CXL::store_fence();
				
				// [[OBSERVABILITY]] - Update metrics after successful replication
				metrics.batches_replicated.fetch_add(1, std::memory_order_relaxed);
				metrics.last_replication_done.store(batch_last_logical_offset, std::memory_order_relaxed);
				{
					std::lock_guard<std::mutex> lock(metrics.metrics_mutex);
					metrics.last_advance_time = std::chrono::steady_clock::now();
				}
				
				VLOG(3) << "[ReplicationThread B" << req.broker_id << "]: Replicated batch, last_offset=" 
						<< batch_last_logical_offset << ", disk_offset=" << disk_offset;
				
				// Periodic metrics logging
				auto now = std::chrono::steady_clock::now();
				if (now - last_metrics_log_time >= kMetricsLogInterval) {
					uint64_t scanned = metrics.batches_scanned.load(std::memory_order_relaxed);
					uint64_t replicated = metrics.batches_replicated.load(std::memory_order_relaxed);
					uint64_t retries = metrics.pwrite_retries.load(std::memory_order_relaxed);
					uint64_t errors = metrics.pwrite_errors.load(std::memory_order_relaxed);
					uint64_t last_done = metrics.last_replication_done.load(std::memory_order_relaxed);
					
					std::chrono::steady_clock::time_point last_advance;
					{
						std::lock_guard<std::mutex> lock(metrics.metrics_mutex);
						last_advance = metrics.last_advance_time;
					}
					auto time_since_advance = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_advance).count();
					
					// [[PHASE_4_BOUNDED_TIMEOUTS]] - Stall detection: warn if scanning but not replicating
					if (scanned > 0 && replicated == 0 && time_since_advance > 5000) {
						LOG(WARNING) << "[ReplicationMetrics B" << req.broker_id << "]: STALL DETECTED - "
							<< "scanned=" << scanned << " batches but replicated=0 for " 
							<< time_since_advance << "ms. Replication may be stuck.";
					}
					
					LOG(INFO) << "[ReplicationMetrics B" << req.broker_id << "]: "
						<< "scanned=" << scanned << ", replicated=" << replicated 
						<< ", pwrite_retries=" << retries << ", pwrite_errors=" << errors
						<< ", last_replication_done=" << last_done
						<< ", time_since_last_advance=" << time_since_advance << "ms";
					
					last_metrics_log_time = now;
				}
			} else {
				// Batch write failed - for permanent errors, exit thread fail-fast
				// The thread will clean up and exit below
				LOG(ERROR) << "[ReplicateThread B" << req.broker_id << "]: Permanent disk write error, terminating replication thread";
				break;  // Exit main loop
			}
			} else {
				// [[PERF_CLEANUPS]] - Spin-then-sleep backoff when no batch found
				// CRITICAL: Do NOT call GetNextReplicationBatch() here!
				// Calling it in the spin loop can advance the cursor without replicating the batch,
				// which could drop batches and cause replication_done stalls.
				//
				// Pattern: Spin briefly with CPU pauses, then sleep. The outer loop will call
				// GetNextReplicationBatch() again on next iteration.
				auto spin_start = std::chrono::steady_clock::now();
				while (std::chrono::steady_clock::now() - spin_start < kSpinDuration) {
					CXL::cpu_pause(); 
				}
				
				// Sleep briefly to avoid 100% CPU during idle
				std::this_thread::sleep_for(kSleepDuration);
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
		// Disk path cleanup - log_addr is malloc'd buffer, not mmap
		// [[REFACTOR_DEAD_PATHS]] - Removed misleading msync() call
		// log_addr in disk mode is a simple malloc'd buffer for staging (not used in final implementation)
		// No need for msync since we use pwrite() directly to disk
		if (log_addr != nullptr) {
			VLOG(2) << "[ReplicateThread " << req.broker_id << "]: Freeing staging buffer.";
			mi_free(log_addr);
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
		// Use a reasonable upper bound to prevent excessive scanning under idle conditions
		// The outer loop's backoff will sleep if no batch found, so we don't need aggressive scanning
		size_t MAX_SCAN_ATTEMPTS = 256;  // Up to ~256 batches per scan
		
		// [[PHASE_0_INSTRUMENTATION]] - Track ordered visibility for stall diagnosis
		static thread_local size_t scan_iterations = 0;
		static thread_local size_t ordered_hits = 0;
		static thread_local size_t ordered_misses = 0;
		static thread_local size_t consecutive_not_ordered = 0;
		static thread_local auto last_diagnostic_log = std::chrono::steady_clock::now();
		constexpr auto kDiagnosticLogInterval = std::chrono::seconds(5);
		constexpr size_t kInvalidationThreshold = 1000;  // Invalidate after 1000 consecutive misses
		
		for (size_t i = 0; i < MAX_SCAN_ATTEMPTS; ++i) {
			// [[OBSERVABILITY]] - Track batches scanned (increment on each iteration)
			replication_metrics_[broker_id].batches_scanned.fetch_add(1, std::memory_order_relaxed);
			scan_iterations++;
			
			// [[PHASE_1_FIX_READER_INVALIDATION]] - Periodic cache invalidation for non-coherent CXL
			// On real non-coherent CXL, readers must invalidate their local cache to observe remote writes
			// Pattern: After N consecutive "not ready" reads, invalidate the cache line and issue load fence
			// This matches the pattern used in BrokerScannerWorker5 (topic.cc:1462-1466)
			if (consecutive_not_ordered >= kInvalidationThreshold) {
				CXL::flush_cacheline(current_batch);
				// Flush second cache line if BatchHeader spans multiple cache lines
				const void* batch_next_line = reinterpret_cast<const void*>(
					reinterpret_cast<const uint8_t*>(current_batch) + 64);
				CXL::flush_cacheline(batch_next_line);
				CXL::load_fence();
				consecutive_not_ordered = 0;
			}
			
			// [[DEVIATION_006: Export Chain Semantics]] 
			// Read ordered flag from the *slot* (like export does in Topic::GetBatchToExport)
			volatile uint32_t ordered_check = reinterpret_cast<volatile BatchHeader*>(current_batch)->ordered;
			
			// Calculate ring offset for diagnostics
			size_t ring_offset = reinterpret_cast<uint8_t*>(current_batch) - reinterpret_cast<uint8_t*>(batch_ring_start);
			
			if (ordered_check != 1) {
				ordered_misses++;
				consecutive_not_ordered++;
				// [[PHASE_0_INSTRUMENTATION]] - Periodic diagnostic logging
				auto now = std::chrono::steady_clock::now();
				if (now - last_diagnostic_log >= kDiagnosticLogInterval) {
					LOG(INFO) << "[GetNextReplicationBatch B" << broker_id << "]: Scan stats: "
						<< "iterations=" << scan_iterations << ", ordered_hits=" << ordered_hits
						<< ", ordered_misses=" << ordered_misses << ", ring_offset=" << ring_offset
						<< ", current_ordered=" << ordered_check << ", consecutive_not_ordered=" << consecutive_not_ordered;
					last_diagnostic_log = now;
				}
				// Current slot not yet ordered, advance and try next
				BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
				if (next_batch >= batch_ring_end) {
					next_batch = batch_ring_start;
				}
				current_batch = next_batch;
				continue;
			}
			
			ordered_hits++;
			consecutive_not_ordered = 0;  // Reset counter on success
			
			// ordered == 1: Follow the export chain to the actual batch header
			// [[PHASE_2_SIMPLIFY_EXPORT]] - batch_off_to_export semantics:
			// - batch_off_to_export == 0: This slot IS the export record (simplified ORDER=5 design)
			// - batch_off_to_export != 0: Points to actual batch header (legacy export chain)
			volatile size_t batch_off_to_export_check = 
				reinterpret_cast<volatile BatchHeader*>(current_batch)->batch_off_to_export;
			
			BatchHeader* actual_batch;
			if (batch_off_to_export_check == 0) {
				// Simplified design: same slot is the export record
				actual_batch = current_batch;
			} else {
				// Legacy export chain: follow offset to actual batch header
				// Bounds validation for batch_off_to_export
				// Must point within the batch ring and be aligned to BatchHeader size
				if (batch_off_to_export_check >= BATCHHEADERS_SIZE ||
				    batch_off_to_export_check % sizeof(BatchHeader) != 0) {
					LOG(WARNING) << "[GetNextReplicationBatch B" << broker_id 
						<< "]: Invalid batch_off_to_export=" << batch_off_to_export_check 
						<< " (must be in [0, " << BATCHHEADERS_SIZE << ") and aligned to " 
						<< sizeof(BatchHeader) << " bytes)";
					// Advance and continue scanning
					BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
					if (next_batch >= batch_ring_end) {
						next_batch = batch_ring_start;
					}
					current_batch = next_batch;
					continue;
				}
				
				// Compute the actual batch header by following the offset
				actual_batch = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(current_batch) + batch_off_to_export_check);
				
				// Verify actual_batch is still within ring bounds
				if (actual_batch < batch_ring_start || actual_batch >= batch_ring_end) {
					LOG(WARNING) << "[GetNextReplicationBatch B" << broker_id 
						<< "]: Export chain points outside ring (offset=" << batch_off_to_export_check << ")";
					// Advance and continue scanning
					BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
					if (next_batch >= batch_ring_end) {
						next_batch = batch_ring_start;
					}
					current_batch = next_batch;
					continue;
				}
			}
			
			// Read batch metadata from the actual batch header with payload bounds checks
			volatile uint32_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(actual_batch)->num_msg;
			volatile size_t total_size_check = reinterpret_cast<volatile BatchHeader*>(actual_batch)->total_size;
			volatile size_t log_idx_check = reinterpret_cast<volatile BatchHeader*>(actual_batch)->log_idx;
			volatile size_t start_logical_offset_check = reinterpret_cast<volatile BatchHeader*>(actual_batch)->start_logical_offset;
			
			// Bounds validation (match BrokerScannerWorker5 guards)
			// Add payload location check to prevent out-of-bounds
			constexpr uint32_t MAX_REASONABLE_NUM_MSG = 100000;
			// [[FIX-HARDCODED-SIZE]] - Use configured CXL size instead of hardcoded 68GB
			// This ensures bounds checks work correctly when CXL_SIZE is configured differently
			const size_t cxl_max_size = CXL_SIZE;
			
			// Check payload doesn't exceed CXL bounds
			// Note: log_idx_check is relative to cxl_addr_, so we validate against total CXL region size
			bool payload_in_bounds = (log_idx_check < cxl_max_size && 
			                          log_idx_check + total_size_check <= cxl_max_size);
			
			bool batch_ready = (num_msg_check != 0 && 
			                   total_size_check > 0 && 
			                   log_idx_check > 0 && 
			                   num_msg_check <= MAX_REASONABLE_NUM_MSG &&
			                   payload_in_bounds);
			
			if (batch_ready) {
				// Batch is valid; use data from actual_batch
				batch_payload = reinterpret_cast<uint8_t*>(cxl_addr_) + log_idx_check;
				batch_payload_size = total_size_check;
				batch_start_logical_offset = start_logical_offset_check;
				batch_last_logical_offset = start_logical_offset_check + num_msg_check - 1;
				
				VLOG(3) << "[GetNextReplicationBatch B" << broker_id << "]: Found ordered batch: "
						<< "num_msg=" << num_msg_check << ", log_idx=" << log_idx_check 
						<< ", payload_size=" << batch_payload_size
						<< ", start_offset=" << batch_start_logical_offset
						<< ", batch_off_to_export=" << batch_off_to_export_check;
				
				// Advance cursor for next iteration
				BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
				if (next_batch >= batch_ring_end) {
					next_batch = batch_ring_start;
				}
				current_batch = next_batch;
				return true;
			}
			
			// Actual batch not ready yet, advance slot and try next
			BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
			if (next_batch >= batch_ring_end) {
				next_batch = batch_ring_start;
			}
			current_batch = next_batch;
		}
		
		// No ordered batch found in scan range
		// Note: batches_scanned counter is incremented inside the loop, so it's already updated
		return false;
	}

	//[[PHASE_5_REFACTOR_LEGACY_PATHS]] - Legacy per-message replication path
	// STATUS: DEPRECATED - No longer used in Stage-4 batch-based replication (ORDER=5)
	// PURPOSE: Kept for reference/fallback only (may be used by older order levels)
	// MIGRATION: All ORDER=5 replication uses GetNextReplicationBatch() instead
	// OWNERSHIP: DiskManager maintains this for backward compatibility
	// TODO: Remove or move to archive/legacy/ directory after confirming no other order levels use it
	// This is a copy of Topic::GetMessageAddr changed to use tinode instead of topic variables
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
