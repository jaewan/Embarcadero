#include "cxl_manager.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <queue>
#include <tuple>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <thread>
#include <vector>
#include <glog/logging.h>
#include "mimalloc.h"
#include "common/configuration.h"
#include "common/config.h"
#include "common/performance_utils.h"

namespace Embarcadero{

static inline void* allocate_shm(int broker_id, CXL_Type cxl_type, size_t cxl_size){
	void *addr = nullptr;
	int cxl_fd;
	bool dev = false;
	if(cxl_type == Real){
		if(std::filesystem::exists("/dev/dax0.0")){
			dev = true;
			cxl_fd = open("/dev/dax0.0", O_RDWR);
		}else{
			if(numa_available() == -1){
				LOG(ERROR) << "Cannot allocate from real CXL";
				return nullptr;
			}else{
				cxl_fd = shm_open("/CXL_SHARED_FILE", O_CREAT | O_RDWR, 0666);
			}
		}
	}else{
		cxl_fd = shm_open("/CXL_SHARED_FILE", O_CREAT | O_RDWR, 0666);
	}

	if (cxl_fd < 0){
		LOG(ERROR)<<"Opening CXL error: " << strerror(errno);
		return nullptr;
	}
	if(broker_id == 0 && !dev){
		LOG(INFO) << "Head broker setting CXL file size to " << cxl_size << " bytes";
		if (ftruncate(cxl_fd, cxl_size) == -1) {
			LOG(ERROR) << "ftruncate failed: " << strerror(errno);
			close(cxl_fd);
			return nullptr;
		}
		LOG(INFO) << "ftruncate completed successfully";
	}
	LOG(INFO) << "Mapping CXL shared memory: " << cxl_size << " bytes";

	const char* fixed_addr_env = std::getenv("EMBARCADERO_CXL_BASE_ADDR");
	std::vector<uintptr_t> fixed_addrs;
	if (fixed_addr_env && fixed_addr_env[0] != '\0') {
		char* end = nullptr;
		uintptr_t parsed = static_cast<uintptr_t>(std::strtoull(fixed_addr_env, &end, 0));
		if (end && *end == '\0' && parsed != 0) {
			fixed_addrs.push_back(parsed);
		} else {
			LOG(ERROR) << "Invalid EMBARCADERO_CXL_BASE_ADDR: " << fixed_addr_env;
			close(cxl_fd);
			return nullptr;
		}
	} else {
		// Fallback addresses to keep all brokers on the same virtual base.
		fixed_addrs = {
			0x600000000000ULL,
			0x500000000000ULL,
			0x400000000000ULL
		};
	}

	for (uintptr_t candidate : fixed_addrs) {
#ifdef MAP_FIXED_NOREPLACE
		addr = mmap(reinterpret_cast<void*>(candidate), cxl_size,
		            PROT_READ | PROT_WRITE,
		            MAP_SHARED | MAP_POPULATE | MAP_FIXED_NOREPLACE,
		            cxl_fd, 0);
#else
		// Best-effort fallback if MAP_FIXED_NOREPLACE is unavailable.
		addr = mmap(reinterpret_cast<void*>(candidate), cxl_size,
		            PROT_READ | PROT_WRITE,
		            MAP_SHARED | MAP_POPULATE | MAP_FIXED,
		            cxl_fd, 0);
#endif
		if (addr != MAP_FAILED) {
			if (addr != reinterpret_cast<void*>(candidate)) {
				LOG(ERROR) << "CXL mapping did not honor requested address "
				           << reinterpret_cast<void*>(candidate)
				           << ", got " << addr;
				munmap(addr, cxl_size);
				addr = MAP_FAILED;
				continue;
			}
			break;
		}
	}

	if (addr == MAP_FAILED || addr == nullptr) {
		LOG(ERROR) << "Mapping CXL failed: " << strerror(errno);
		close(cxl_fd);
		return nullptr;
	}
	close(cxl_fd);
	LOG(INFO) << "CXL mapping successful at address: " << addr;

	if(cxl_type == Real && !dev && broker_id == 0){
		// Create a bitmask for the NUMA node (numa node 2 should be the CXL memory)
		struct bitmask* bitmask = numa_allocate_nodemask();
		numa_bitmask_setbit(bitmask, 2);

		// Bind the memory to the specified NUMA node
		// Remove MPOL_MF_STRICT to allow partial binding if some pages can't be moved
		if (mbind(addr, cxl_size, MPOL_BIND, bitmask->maskp, bitmask->size, MPOL_MF_MOVE) == -1) {
			LOG(WARNING) << "mbind failed, but continuing with best-effort NUMA binding: " << strerror(errno);
			// Don't fail completely - continue with whatever NUMA binding we got
		} else {
			VLOG(3) << "Successfully bound " << cxl_size << " bytes to NUMA node 2";
		}

		numa_free_nodemask(bitmask);
	}

	if(broker_id == 0){
		LOG(INFO) << "Head broker clearing CXL memory: " << cxl_size << " bytes";
		// OPTIMIZATION: Use faster memory clearing with parallel chunks
		const size_t chunk_size = 1024 * 1024 * 1024;  // 1GB chunks
		const size_t num_chunks = (cxl_size + chunk_size - 1) / chunk_size;
		
		std::vector<std::thread> clear_threads;
		for (size_t i = 0; i < num_chunks; ++i) {
			clear_threads.emplace_back([addr, i, chunk_size, cxl_size]() {
				size_t start = i * chunk_size;
				size_t size = std::min(chunk_size, cxl_size - start);
				memset((uint8_t*)addr + start, 0, size);
			});
		}
		
		// Wait for all threads to complete
		for (auto& thread : clear_threads) {
			thread.join();
		}
		LOG(INFO) << "CXL memory cleared successfully using " << num_chunks << " parallel threads";
	}
	return addr;
}

CXLManager::CXLManager(int broker_id, CXL_Type cxl_type, std::string head_ip):
	broker_id_(broker_id),
	head_ip_(head_ip){
		size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	// CRITICAL FIX: All brokers must use the same CXL size for consistent memory layout
	// Get the configured size from YAML to ensure consistency
	cxl_size_ = Embarcadero::Configuration::getInstance().config().cxl.size.get();
	
	LOG(INFO) << "CXLManager: broker_id=" << broker_id << " using CXL size=" << cxl_size_ << " bytes";

		// Initialize CXL
		cxl_addr_ = allocate_shm(broker_id, cxl_type, cxl_size_);
		if(cxl_addr_ == nullptr){
			return;
		}

			// [[PHASE_1A_EPOCH_FENCING]] ControlBlock at offset 0 (128 bytes)
		// [[CXL_MEMORY_LAYOUT_v2]] PBR/BatchHeaders/TInode/Bitmap/Segments start AFTER Phase 2 metadata
		// Layout: 0x0 ControlBlock | 0x1000 CompletionVector | 0x2000 GOI (16GB) | 0x4_0000_2000 PBR...
		static constexpr size_t kPhase2MetadataEnd = 0x4'0000'2000ULL;  // 16 GB + 8 KB, end of GOI (docs/CXL_MEMORY_LAYOUT_v2.md)
		control_block_ = reinterpret_cast<ControlBlock*>(cxl_addr_);
		base_for_regions_ = reinterpret_cast<uint8_t*>(cxl_addr_) + kPhase2MetadataEnd;
		uint8_t* base_for_regions = reinterpret_cast<uint8_t*>(base_for_regions_);

		// Initialize CXL memory regions (TInode, Bitmap, BatchHeaders, Segments after Phase 2 region)
		size_t TINode_Region_size = sizeof(TInode) * MAX_TOPIC_SIZE;
		size_t padding = TINode_Region_size - ((TINode_Region_size/cacheline_size) * cacheline_size);
		TINode_Region_size += padding;
		size_t Bitmap_Region_size = cacheline_size * MAX_TOPIC_SIZE;
		// Use configured max brokers consistently with GetNewSegment()
		const size_t configured_max_brokers = NUM_MAX_BROKERS_CONFIG;
		size_t BatchHeaders_Region_size = configured_max_brokers * BATCHHEADERS_SIZE * MAX_TOPIC_SIZE;
		
		// [[DEVIATION_005: Atomic Bitmap-Based Segment Allocation]]
		// Phase 1: Single-Node Optimized (cache-coherent)
		// All brokers share the same segment pool (no per-broker partitioning)
		// This prevents fragmentation and enables efficient multi-topic support
		// See docs/memory-bank/spec_deviation.md DEV-005
		
		// Calculate total segment region size (shared pool for all brokers)
		// Usable size = after Phase 2 metadata (ControlBlock + CV + GOI)
		if (cxl_size_ < kPhase2MetadataEnd + TINode_Region_size + Bitmap_Region_size + BatchHeaders_Region_size) {
			LOG(ERROR) << "CXL size " << cxl_size_ << " too small for layout v2: need at least "
			           << (kPhase2MetadataEnd + TINode_Region_size + Bitmap_Region_size + BatchHeaders_Region_size)
			           << " (Phase2 metadata + TInode + Bitmap + BatchHeaders)";
			return;
		}
		size_t Segment_Region_size = (cxl_size_ - kPhase2MetadataEnd - TINode_Region_size - Bitmap_Region_size - BatchHeaders_Region_size);
		padding = Segment_Region_size % cacheline_size;
		Segment_Region_size -= padding;

		bitmap_ = base_for_regions + TINode_Region_size;
		batchHeaders_ = reinterpret_cast<uint8_t*>(bitmap_) + Bitmap_Region_size;
		
		// [[DEVIATION_004]] - Bmeta region removed, segments start after BatchHeaders
		// Shared segment pool: All brokers allocate from the same region
		// Bitmap coordinates allocation to prevent fragmentation
		segments_ = reinterpret_cast<uint8_t*>(batchHeaders_) + BatchHeaders_Region_size;
		batchHeaders_ = reinterpret_cast<uint8_t*>(batchHeaders_) + (broker_id_ * (BATCHHEADERS_SIZE * MAX_TOPIC_SIZE));

		// [[PHASE_1A_EPOCH_FENCING]] Head broker initializes ControlBlock (epoch-based fencing)
		if (broker_id_ == 0) {
			control_block_->epoch.store(1, std::memory_order_release);
			CXL::flush_cacheline(control_block_);
			CXL::store_fence();
			LOG(INFO) << "CXLManager: ControlBlock initialized (epoch=1) for zombie fencing";
		}

		// [[PHASE_2_GOI_CV_ALLOCATION]] Allocate GOI and CompletionVector in CXL memory
		// Memory layout per docs/CXL_MEMORY_LAYOUT_v2.md:
		//   0x0000_0000: ControlBlock (128 B)
		//   0x0000_1000: CompletionVector (4 KB)
		//   0x0000_2000: GOI (16 GB)
		static constexpr size_t kCompletionVectorOffset = 0x1000;  // 4 KB from base
		static constexpr size_t kGOIOffset = 0x2000;               // 8 KB from base
		static constexpr size_t kMaxGOIEntries = 256ULL * 1024 * 1024;  // 256M entries

		completion_vector_ = reinterpret_cast<CompletionVectorEntry*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);

		goi_ = reinterpret_cast<GOIEntry*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + kGOIOffset);

		// Head broker initializes CompletionVector (GOI can be lazy-initialized - zeros are valid)
		if (broker_id_ == 0) {
			// Sentinel (uint64_t)-1 = no progress; 0 = first batch completed (so GetOffsetToAck can distinguish)
			constexpr uint64_t kCVNoProgress = static_cast<uint64_t>(-1);
			for (int i = 0; i < NUM_MAX_BROKERS; i++) {
				completion_vector_[i].completed_pbr_head.store(kCVNoProgress, std::memory_order_release);
			}

			// Flush CV to CXL (4 KB = 64 cache lines, flush in batches)
			for (int i = 0; i < NUM_MAX_BROKERS; i += 2) {  // 2 entries per 128-byte cacheline
				CXL::flush_cacheline(&completion_vector_[i]);
			}
			CXL::store_fence();

			LOG(INFO) << "CXLManager: Phase 2 initialized - CompletionVector (4 KB) and GOI (16 GB) allocated";
			LOG(INFO) << "  - CompletionVector at offset 0x" << std::hex << kCompletionVectorOffset
			          << " (" << NUM_MAX_BROKERS << " brokers × 128 bytes)";
			LOG(INFO) << "  - GOI at offset 0x" << std::hex << kGOIOffset
			          << " (" << std::dec << kMaxGOIEntries << " entries × 64 bytes = 16 GB)";
		}

		// Initialize bitmap to zero (broker 0 only, to avoid race conditions)
		if (broker_id_ == 0) {
			// Calculate bitmap size needed for segment allocation
			// Bitmap tracks segments: 1 bit per segment
			// We use uint64_t words for atomic operations (64 segments per word)
			size_t num_segments = Segment_Region_size / SEGMENT_SIZE;
			size_t bitmap_words = (num_segments + 63) / 64;  // Round up to uint64_t words
			size_t bitmap_bytes = bitmap_words * sizeof(uint64_t);
			
			// Ensure bitmap region is large enough
			if (Bitmap_Region_size < bitmap_bytes) {
				LOG(WARNING) << "Bitmap region (" << Bitmap_Region_size 
				            << " bytes) may be insufficient for " << num_segments 
				            << " segments (needs " << bitmap_bytes << " bytes)";
			}
			
			// Zero-initialize bitmap (all segments free)
			memset(bitmap_, 0, std::min(Bitmap_Region_size, bitmap_bytes));
			CXL::flush_cacheline(bitmap_);
			CXL::store_fence();
			
			LOG(INFO) << "CXLManager: Initialized segment bitmap (" << bitmap_bytes 
			          << " bytes, " << num_segments << " segments, " 
			          << Segment_Region_size / (1024*1024*1024) << " GB pool)";
			LOG(INFO) << "CXLManager: Bmeta region removed (using TInode.offset_entry instead)";
		}


		VLOG(3) << "\t[CXLManager]: \t\tConstructed";
		return;
	}

CXLManager::~CXLManager(){
	stop_threads_ = true;
	for(std::thread& thread : sequencerThreads_){
		if(thread.joinable()){
			thread.join();
		}
	}

	if (munmap(cxl_addr_, cxl_size_) < 0)
		LOG(ERROR) << "Unmapping CXL error";

	VLOG(3) << "[CXLManager]: \t\tDestructed";
}

std::function<void(void*, size_t)> CXLManager::GetCXLBuffer(BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header,
		size_t &logical_offset, SequencerType &seq_type, BatchHeader* &batch_header_location,
		bool epoch_already_checked) {
	return topic_manager_->GetCXLBuffer(batch_header, topic, log, segment_header,
			logical_offset, seq_type, batch_header_location, epoch_already_checked);
}

bool CXLManager::ReserveBLogSpace(const char* topic, size_t size, void*& log) {
	return topic_manager_->ReserveBLogSpace(topic, size, log);
}

bool CXLManager::IsPBRAboveHighWatermark(const char* topic, int high_pct) {
	return topic_manager_->IsPBRAboveHighWatermark(topic, high_pct);
}

bool CXLManager::IsPBRBelowLowWatermark(const char* topic, int low_pct) {
	return topic_manager_->IsPBRBelowLowWatermark(topic, low_pct);
}

bool CXLManager::ReservePBRSlotAndWriteEntry(const char* topic, BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location) {
	return topic_manager_->ReservePBRSlotAndWriteEntry(topic, batch_header, log,
			segment_header, logical_offset, batch_header_location);
}

bool CXLManager::ReservePBRSlotAfterRecv(const char* topic, BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location) {
	return topic_manager_->ReservePBRSlotAfterRecv(topic, batch_header, log,
			segment_header, logical_offset, batch_header_location);
}

Topic* CXLManager::GetTopicPtr(const char* topic) {
	return topic_manager_ ? topic_manager_->GetTopic(std::string(topic)) : nullptr;
}

bool CXLManager::IsPBRAboveHighWatermark(Topic* topic_ptr, int high_pct) {
	return topic_manager_ && topic_manager_->IsPBRAboveHighWatermark(topic_ptr, high_pct);
}

bool CXLManager::ReserveBLogSpace(Topic* topic_ptr, size_t size, void*& log, bool epoch_already_checked) {
	return topic_manager_ && topic_manager_->ReserveBLogSpace(topic_ptr, size, log, epoch_already_checked);
}

bool CXLManager::ReservePBRSlotAndWriteEntry(Topic* topic_ptr, BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	return topic_manager_ && topic_manager_->ReservePBRSlotAndWriteEntry(topic_ptr, batch_header, log,
			segment_header, logical_offset, batch_header_location, epoch_already_checked);
}

bool CXLManager::ReservePBRSlotAfterRecv(Topic* topic_ptr, BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	return topic_manager_ && topic_manager_->ReservePBRSlotAfterRecv(topic_ptr, batch_header, log,
			segment_header, logical_offset, batch_header_location, epoch_already_checked);
}

inline int hashTopic(const char topic[TOPIC_NAME_SIZE]) {
	unsigned int hash = 0;

	for (int i = 0; i < TOPIC_NAME_SIZE; ++i) {
		hash = (hash * TOPIC_NAME_SIZE) + topic[i];
	}
	return hash % MAX_TOPIC_SIZE;
}

// This function returns TInode without inspecting if the topic exists
TInode* CXLManager::GetTInode(const char* topic){
	// Convert topic to tinode address
	//static const std::hash<std::string> topic_to_idx;
	//int TInode_idx = topic_to_idx(topic) % MAX_TOPIC_SIZE;
	int TInode_idx = hashTopic(topic);
	return (TInode*)((uint8_t*)base_for_regions_ + (TInode_idx * sizeof(struct TInode)));
}

TInode* CXLManager::GetReplicaTInode(const char* topic){
	char replica_topic[TOPIC_NAME_SIZE];
	memcpy(replica_topic, topic, TOPIC_NAME_SIZE);
	memcpy((uint8_t*)replica_topic + (TOPIC_NAME_SIZE-7), "replica", 7); 
	int TInode_idx = hashTopic(replica_topic);
	return (TInode*)((uint8_t*)base_for_regions_ + (TInode_idx * sizeof(struct TInode)));
}

/**
 * [[DEVIATION_005: Atomic Bitmap-Based Segment Allocation]]
 * 
 * @brief Lock-free segment allocation using shared bitmap
 * 
 * @deployment Single-node multi-process (cache-coherent)
 * @threading Thread-safe across processes via CPU cache coherence
 * @performance ~50ns allocation latency (vs ~30μs for network RPC)
 * @scaling Works up to ~128 cores sharing cache-coherent domain
 * 
 * @future_work For multi-node non-coherent CXL:
 *   - Option A: Partitioned bitmap (no cross-broker coordination)
 *   - Option B: Leader-based allocation (network RPC)
 * 
 * See docs/memory-bank/spec_deviation.md DEV-005
 */
void* CXLManager::GetNewSegment(){
	// [[DEVIATION_005]] Phase 1: Single-Node Optimized (cache-coherent atomic bitmap)
	// [Future Work] 
	// Use per-broker segment allocation instead of global atomic counter
    // This eliminates cross-process contention and provides proper isolation
	
	// Calculate total segments in shared pool (static initialization)
	static size_t total_segments = 0;
	static size_t bitmap_words = 0;
	static bool initialized = false;
	
	if (!initialized) {
		size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
		size_t TINode_Region_size = sizeof(TInode) * MAX_TOPIC_SIZE;
		size_t padding = TINode_Region_size - ((TINode_Region_size/cacheline_size) * cacheline_size);
		TINode_Region_size += padding;
		size_t Bitmap_Region_size = cacheline_size * MAX_TOPIC_SIZE;
		
		// Get configuration values
		size_t cxl_size = Embarcadero::Configuration::getInstance().config().cxl.size.get();
		const size_t configured_max_brokers = NUM_MAX_BROKERS_CONFIG;
		size_t BatchHeaders_Region_size = configured_max_brokers * BATCHHEADERS_SIZE * MAX_TOPIC_SIZE;
		
		// [[DEVIATION_004]] - Bmeta region removed; [[PHASE_1A]] subtract ControlBlock
		size_t Segment_Region_size = (cxl_size - sizeof(ControlBlock) - TINode_Region_size - Bitmap_Region_size - BatchHeaders_Region_size);
		padding = Segment_Region_size % cacheline_size;
		Segment_Region_size -= padding;
		
		total_segments = Segment_Region_size / SEGMENT_SIZE;
		bitmap_words = (total_segments + 63) / 64;  // Round up to uint64_t words
		
		LOG(INFO) << "GetNewSegment (Broker " << broker_id_ << "): CXL_size=" << cxl_size
		          << " CONFIGURED_MAX_BROKERS=" << configured_max_brokers
		          << " Segment_Region_size=" << Segment_Region_size / (1024*1024*1024) << " GB"
		          << " SEGMENT_SIZE=" << SEGMENT_SIZE / (1024*1024) << " MB"
		          << " total_segments=" << total_segments
		          << " bitmap_words=" << bitmap_words;
		initialized = true;
	}
	
	// Thread-local hint to reduce contention (brokers naturally drift to different bitmap words)
	static thread_local size_t hint = 0;
	
	// Cast bitmap to uint64_t* for atomic operations (64 segments per word)
	uint64_t* bitmap64 = static_cast<uint64_t*>(bitmap_);
	
	// Linear scan with hint: Start from last successful allocation
	for (size_t attempts = 0; attempts < bitmap_words; ++attempts) {
		size_t i = (hint + attempts) % bitmap_words;
		
		// Load current bitmap word (acquire semantics for visibility)
		uint64_t current_bits = __atomic_load_n(&bitmap64[i], __ATOMIC_ACQUIRE);
		
		// Skip if word is full (all 64 segments allocated)
		if (current_bits == 0xFFFFFFFFFFFFFFFFULL) {
			continue;
		}
		
		// [[PERFORMANCE: OPTIMIZED]] Use bit manipulation to find first zero bit faster
		// Instead of scanning all 64 bits, use __builtin_ctzll to find first zero
		// This reduces average scan time from O(32) to O(1) for sparse bitmaps
		uint64_t inverted = ~current_bits;  // Invert: 1 = free, 0 = allocated
		
		if (inverted == 0) {
			// All bits set (all segments allocated in this word)
			continue;
		}
		
		// Find first zero bit (first free segment) using hardware instruction
		// __builtin_ctzll returns number of trailing zeros (0-63)
		// If inverted has no set bits, behavior is undefined, but we checked above
		int bit = __builtin_ctzll(inverted);  // Count trailing zeros = first set bit in inverted
		
		// bit is guaranteed to be 0-63 by __builtin_ctzll semantics
		uint64_t mask = 1ULL << bit;
		
		// Attempt atomic claim: set bit to 1
		uint64_t old = __atomic_fetch_or(&bitmap64[i], mask, __ATOMIC_SEQ_CST);
		
		// Verify we successfully claimed it (bit was 0 before)
		if (!(old & mask)) {
			// Success! Calculate global segment index and address
			size_t global_idx = (i * 64) + bit;
			
			// Bounds check
			if (global_idx >= total_segments) {
				// This shouldn't happen, but handle gracefully
				LOG(ERROR) << "Broker " << broker_id_ 
				           << " allocated out-of-bounds segment " << global_idx
				           << " (max: " << total_segments << ")";
				// Unset the bit we just set
				__atomic_fetch_and(&bitmap64[i], ~mask, __ATOMIC_SEQ_CST);
				continue;
			}
			
			void* segment_addr = static_cast<uint8_t*>(segments_) + (global_idx * SEGMENT_SIZE);
			
			// Update hint for next allocation (reduces contention)
			hint = i;
			
			// CRITICAL: Flush bitmap cache line for CXL visibility
			// Even on cache-coherent systems, this ensures visibility to CXL device
			CXL::flush_cacheline(&bitmap64[i]);
			CXL::store_fence();
			
			// Initialize segment header (first 64 bytes)
			memset(segment_addr, 0, 64);
			CXL::flush_cacheline(segment_addr);
			CXL::store_fence();
			
			LOG(INFO) << "Broker " << broker_id_ 
			          << " allocated segment " << global_idx 
			          << " at " << segment_addr;
			
			return segment_addr;
		}
		// If atomic claim failed (another broker claimed it), continue to next word
	}
	
	// All segments exhausted
	LOG(ERROR) << "CXL memory exhausted: All " << total_segments 
	           << " segments allocated (Broker " << broker_id_ << ")";
	return nullptr;
}

void* CXLManager::GetNewBatchHeaderLog(){
	static std::atomic<size_t> batch_header_log_count{0};
	CHECK_LT(batch_header_log_count, MAX_TOPIC_SIZE) << "You are creating too many topics";
	size_t offset = batch_header_log_count.fetch_add(1, std::memory_order_relaxed);

	return (uint8_t*)batchHeaders_  + offset*BATCHHEADERS_SIZE;
}

// Phase 1.2: Memory layout calculation functions for PBR and GOI
size_t CXLManager::CalculatePBROffset(int broker_id, int max_brokers) {
	// PBR comes after existing BatchHeaders region
	size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
	size_t TINode_Region_size = sizeof(TInode) * MAX_TOPIC_SIZE;
	size_t padding = TINode_Region_size - ((TINode_Region_size/cacheline_size) * cacheline_size);
	TINode_Region_size += padding;
	
	size_t Bitmap_Region_size = cacheline_size * MAX_TOPIC_SIZE;
	size_t BatchHeaders_Region_size = max_brokers * BATCHHEADERS_SIZE * MAX_TOPIC_SIZE;
	
	// PBR region starts after BatchHeaders
	size_t PBR_Region_start = TINode_Region_size + Bitmap_Region_size + BatchHeaders_Region_size;
	
	// Each broker gets its own PBR
	return PBR_Region_start + broker_id * PBR_SIZE_PER_BROKER;
}

size_t CXLManager::CalculateGOIOffset(int max_brokers) {
	// GOI comes after all PBRs
	size_t last_pbr_offset = CalculatePBROffset(max_brokers - 1, max_brokers);
	return last_pbr_offset + PBR_SIZE_PER_BROKER;
}

size_t CXLManager::CalculateBrokerLogOffset(int broker_id, int max_brokers) {
	// BrokerLogs come after GOI
	size_t goi_offset = CalculateGOIOffset(max_brokers);
	size_t broker_logs_start = goi_offset + GOI_SIZE;
	
	// Each broker gets equal share of remaining memory for its log
	// This is a placeholder - in Phase 2 we'll implement proper BrokerLog sizing
	size_t remaining_memory = CXL_SIZE - broker_logs_start;
	size_t broker_log_size = remaining_memory / max_brokers;
	
	return broker_logs_start + broker_id * broker_log_size;
}

size_t CXLManager::GetTotalMemoryRequirement(int max_brokers) {
	// Calculate total memory needed for new layout
	size_t cacheline_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
	size_t TINode_Region_size = sizeof(TInode) * MAX_TOPIC_SIZE;
	size_t padding = TINode_Region_size - ((TINode_Region_size/cacheline_size) * cacheline_size);
	TINode_Region_size += padding;
	
	size_t Bitmap_Region_size = cacheline_size * MAX_TOPIC_SIZE;
	size_t BatchHeaders_Region_size = max_brokers * BATCHHEADERS_SIZE * MAX_TOPIC_SIZE;
	size_t PBR_Region_size = max_brokers * PBR_SIZE_PER_BROKER;
	size_t GOI_Region_size = GOI_SIZE;
	
	// Minimum memory requirement (before BrokerLogs)
	return TINode_Region_size + Bitmap_Region_size + BatchHeaders_Region_size + 
	       PBR_Region_size + GOI_Region_size;
}

void CXLManager::GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers,
		TInode *tinode) {
	if (!get_registered_brokers_callback_(registered_brokers, nullptr /* msg_to_order removed */, tinode)) {
		LOG(ERROR) << "GetRegisteredBrokerSet: Callback failed to get registered brokers.";
		registered_brokers.clear(); // Ensure set is empty on failure
	}
}

void CXLManager::GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
		MessageHeader** msg_to_order, TInode *tinode){
	if(get_registered_brokers_callback_(registered_brokers, msg_to_order, tinode)){
		for(const auto &broker_id : registered_brokers){
			// Wait for other brokers to initialize this topic. 
			// This is here to avoid contention in grpc(hearbeat) which can cause deadlock when rpc is called
			// while waiting for other brokers to initialize (untill publish is called)
			while(tinode->offsets[broker_id].log_offset == 0){
				std::this_thread::yield();
			}
			msg_to_order[broker_id] = ((MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id].log_offset));
		}
	}
}

// Sequence without respecting publish order
void CXLManager::Sequencer1(std::array<char, TOPIC_NAME_SIZE> topic) {
	VLOG(5) << "[DEBUG] ************** Sequencer 1 **************";
	struct TInode *tinode = GetTInode(topic.data());
	if (!tinode) {
		LOG(ERROR) << "Sequencer1: Failed to get TInode for topic " << topic.data();
		return;
	}

	// Local storage for message pointers, initialized to nullptr
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS] = {nullptr};
	absl::btree_set<int> registered_brokers;
	absl::btree_set<int> initialized_brokers; // Track initialized brokers
	static size_t seq = 0; // Sequencer counter

	// Get the initial set of registered brokers (without waiting)
	GetRegisteredBrokerSet(registered_brokers, tinode);

	if (registered_brokers.empty()) {
		LOG(WARNING) << "Sequencer1: No registered brokers found for topic " << topic.data() << ". Sequencer might idle.";
	}

	while (!stop_threads_) {
		bool processed_message = false; // Track if any work was done in this outer loop iteration

		// TODO: If brokers can register dynamically, call GetRegisteredBrokerSet periodically
		//       and update the registered_brokers set here.

		for (auto broker_id : registered_brokers) {
			// --- Dynamic Initialization Check ---
			if (initialized_brokers.find(broker_id) == initialized_brokers.end()) {
				// This broker hasn't been initialized yet, check its log offset NOW
				size_t current_log_offset = tinode->offsets[broker_id].log_offset; // Read the current offset

				if (current_log_offset == 0) {
					// Still not initialized, skip this broker for this iteration
					VLOG(5) << "Sequencer1: Broker " << broker_id << " log still uninitialized (offset=0), skipping.";
					continue;
				} else {
					// Initialize Now!
					VLOG(5) << "Sequencer1: Initializing broker " << broker_id << " with log_offset=" << current_log_offset;
					msg_to_order[broker_id] = ((MessageHeader*)((uint8_t*)cxl_addr_ + current_log_offset));
					initialized_brokers.insert(broker_id); // Mark as initialized
																								 // Proceed to process messages below
				}
			}

			// --- Process Messages if Initialized ---
			// Ensure msg_to_order pointer is valid before dereferencing
			if (msg_to_order[broker_id] == nullptr) {
				// This should ideally not happen if the logic above is correct, but safety check
				LOG(DFATAL) << "Sequencer1: msg_to_order[" << broker_id << "] is null despite being marked initialized!";
				continue;
			}

			// Read necessary volatile/shared values (consider atomics/locking if needed)
			size_t current_written_offset = tinode->offsets[broker_id].written; // Where the broker has written up to (logical offset)
																																					// Note: MessageHeader fields read below might also need volatile/atomic handling

																																					// Check if broker has indicated completion/error
			if (current_written_offset == static_cast<size_t>(-1)) {
				// Broker might be done or encountered an error, skip it permanently?
				// Or maybe just for this round? Depends on the meaning of -1.
				VLOG(4) << "Sequencer1: Broker " << broker_id << " written offset is -1, skipping.";
				continue;
			}

			// Get the logical offset embedded in the *current* message header we're pointing to
			// This assumes logical_offset field correctly tracks message sequence within the broker's log
			size_t msg_logical_off = msg_to_order[broker_id]->logical_offset;


			// Inner loop to process available messages for this broker
			while (!stop_threads_ &&
					msg_logical_off != static_cast<size_t>(-1) && // Check if current message is valid
					msg_logical_off <= current_written_offset && // Check if message offset has been written by broker
					msg_to_order[broker_id]->next_msg_diff != 0)  // Check if it links to a next message (validity)
			{
				// Check if total order has already been assigned (e.g., by another sequencer replica?)
				// Need to define what indicates "not yet assigned". Using 0 might be risky if 0 is valid.
				// Let's assume unassigned is indicated by a specific value, e.g., -1 or max_size_t
				// For now, let's assume we always assign if the conditions above are met. Revisit if needed.

				VLOG(5) << "Sequencer1: Assigning seq=" << seq << " to broker=" << broker_id << ", logical_offset=" << msg_logical_off;
				msg_to_order[broker_id]->total_order = seq; // Assign sequence number

								// Note: DEV-002 (batched flushes) planned - could batch if multiple fields in same cache line
				CXL::flush_cacheline(msg_to_order[broker_id]);
				CXL::store_fence();

				seq++; // Increment global sequence number

				// Update TInode about the latest processed message *for this broker*
				// UpdateTinodeOrder now includes its own flush
				UpdateTinodeOrder(topic.data(), tinode, broker_id, msg_logical_off,
						(uint8_t*)msg_to_order[broker_id] - (uint8_t*)cxl_addr_); // Pass CXL relative offset

				processed_message = true; // We did some work
				msg_to_order[broker_id] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker_id] + msg_to_order[broker_id]->next_msg_diff);
				msg_logical_off = msg_to_order[broker_id]->logical_offset;

			} // End inner while loop for processing broker messages

		} // End for loop iterating through registered_brokers

		// If no messages were processed across all brokers, yield briefly
		// This prevents busy-spinning when there's no new data.
		if (!processed_message && !stop_threads_) {
			// Check again if any uninitialized brokers became initialized
			bool potentially_newly_initialized = false;
			for(auto broker_id : registered_brokers) {
				if (initialized_brokers.find(broker_id) == initialized_brokers.end()) {
					if (tinode->offsets[broker_id].log_offset != 0) {
						potentially_newly_initialized = true;
						break;
					}
				}
			}
			if (!potentially_newly_initialized) {
				std::this_thread::yield();
			}
		}

	} // End outer while(!stop_threads_)
}

// Order 2 with single thread
void CXLManager::Sequencer2(std::array<char, TOPIC_NAME_SIZE> topic){
	LOG(INFO) <<"[DEBUG] ************** Seqeucner2 **************";
	struct TInode *tinode = GetTInode(topic.data());
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS];
	absl::btree_set<int> registered_brokers;
	absl::flat_hash_map<int/*client_id*/, size_t/*client_req_id*/> last_ordered; 
	// Store skipped messages to respect the client order.
	// Use absolute adrress b/c it is only used in this thread later
	absl::flat_hash_map<int/*client_id*/, absl::btree_map<size_t/*client_order*/, std::pair<int /*broker_id*/, struct MessageHeader*>>> skipped_msg;
	static size_t seq = 0;
	// Tracks the messages of written order to later report the sequentially written messages
	std::array<std::queue<MessageHeader* /*physical addr*/>, NUM_MAX_BROKERS> queues;


	GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
	auto last_updated = std::chrono::steady_clock::now();
	while(!stop_threads_){
		bool yield = true;
		for(auto broker : registered_brokers){
			size_t msg_logical_off = msg_to_order[broker]->logical_offset;
		//This ensures the message is Combined (complete flag removed - legacy code)
		if(/* msg_to_order[broker]->complete == 1 && */ msg_logical_off != (size_t)-1 && msg_logical_off <= tinode->offsets[broker].written){
				yield = false;
				queues[broker].push(msg_to_order[broker]);
				int client = msg_to_order[broker]->client_id;
				size_t client_order = msg_to_order[broker]->client_order;
				auto last_ordered_itr = last_ordered.find(client);
				if(client_order == 0 || 
						(last_ordered_itr != last_ordered.end() && last_ordered_itr->second == client_order - 1)){
					msg_to_order[broker]->total_order = seq;
					// Flush & Poll principle: Sequencer must flush after writing total_order
					CXL::flush_cacheline(msg_to_order[broker]);
					CXL::store_fence();
					seq++;
					last_ordered[client] = client_order;
					// Check if there are skipped messages from this client and give order
					auto it = skipped_msg.find(client);
					if(it != skipped_msg.end()){
						std::vector<int> to_remove;
						for (auto& pair : it->second) {
							int client_order = pair.first;
							if((size_t)client_order == last_ordered[client] + 1){
								pair.second.second->total_order = seq;
								seq++;
								last_ordered[client] = client_order;
								to_remove.push_back(client_order);
							}else{
								break;
							}
						}
						for(auto &id: to_remove){
							it->second.erase(id);
						}
					}
					for(auto b: registered_brokers){
						if(queues[b].empty()){
							continue;
						}else{
							MessageHeader  *header = queues[b].front();
							MessageHeader* exportable_msg = nullptr;
							while(header->client_order <= last_ordered[header->client_id]){
								queues[b].pop();
								exportable_msg = header;
								if(queues[b].empty()){
									break;
								}
								header = queues[b].front();
							}
							if(exportable_msg){
								UpdateTinodeOrder(topic.data(), tinode, b, exportable_msg->logical_offset,(uint8_t*)exportable_msg - (uint8_t*)cxl_addr_);
							}
						}
					}
				}else{
					queues[broker].push(msg_to_order[broker]);
					//Insert to skipped messages
					auto it = skipped_msg.find(client);
					if (it == skipped_msg.end()) {
						absl::btree_map<size_t, std::pair<int, MessageHeader*>> new_map;
						new_map.emplace(client_order, std::make_pair(broker, msg_to_order[broker]));
						skipped_msg.emplace(client, std::move(new_map));
					} else {
						it->second.emplace(client_order, std::make_pair(broker, msg_to_order[broker]));
					}
				}
				msg_to_order[broker] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker] + msg_to_order[broker]->next_msg_diff);
			}
		} // end broker loop
		if(yield){
			GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			last_updated = std::chrono::steady_clock::now();
			std::this_thread::yield();
		}else if(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
					- last_updated).count() >= HEARTBEAT_INTERVAL){
			GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			last_updated = std::chrono::steady_clock::now();
		}
	}// end while
}

// Does not support multi-client, dynamic message size, dynamic batch 
void CXLManager::Sequencer3(std::array<char, TOPIC_NAME_SIZE> topic){
	VLOG(5) << "[DEBUG] ************** Sequencer 3 **************";
	struct TInode *tinode = GetTInode(topic.data());
	struct MessageHeader* msg_to_order[NUM_MAX_BROKERS];
	absl::btree_set<int> registered_brokers;
	static size_t seq = 0;
	static size_t batch_seq = 0;

	GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
	//auto last_updated = std::chrono::steady_clock::now();
	size_t num_brokers = registered_brokers.size();

	while(!stop_threads_){
		//bool yield = true;
		for(auto broker : registered_brokers){
		// NOTE: Legacy code - complete flag removed
		// This busy-wait is no longer needed with batch-level completion
		/* while(msg_to_order[broker]->complete == 0){
			if(stop_threads_)
				return;
			std::this_thread::yield();
		} */
			size_t num_msg_per_batch = BATCH_SIZE / msg_to_order[broker]->paddedSize;
			size_t msg_logical_off = (batch_seq/num_brokers)*num_msg_per_batch;
			size_t n = msg_logical_off + num_msg_per_batch;
			while(!stop_threads_ && msg_logical_off < n){
				size_t written = tinode->offsets[broker].written;
				if(written == (size_t)-1){
					continue;
				}
				written = std::min(written, n-1);
				while(!stop_threads_ && msg_logical_off <= written && msg_to_order[broker]->next_msg_diff != 0 
						&& msg_to_order[broker]->logical_offset != (size_t)-1){
					msg_to_order[broker]->total_order = seq;
					// Flush & Poll principle: Sequencer must flush after writing total_order
					CXL::flush_cacheline(msg_to_order[broker]);
					CXL::store_fence();
					seq++;
					UpdateTinodeOrder(topic.data(), tinode, broker, msg_logical_off, (uint8_t*)msg_to_order[broker] - (uint8_t*)cxl_addr_);
					msg_to_order[broker] = (struct MessageHeader*)((uint8_t*)msg_to_order[broker] + msg_to_order[broker]->next_msg_diff);
					msg_logical_off++;
				}
			}
			batch_seq++;
		}
		/*
			 if(yield){
			 GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			 last_updated = std::chrono::steady_clock::now();
			 std::this_thread::yield();
			 }else if(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
			 - last_updated).count() >= HEARTBEAT_INTERVAL){
			 GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			 last_updated = std::chrono::steady_clock::now();
			 }
			 */
	}
}

// Return sequentially ordered logical offset + 1 and 
// if end offset's physical address should be stored in end_offset_logical_to_physical_
size_t CXLManager::SequentialOrderTracker::InsertAndGetSequentiallyOrdered(size_t offset, size_t size){
	//absl::MutexLock lock(&range_mu_);
	size_t end = offset + size;

	// Find the first range that starts after our offset
	auto next_it = ordered_ranges_.upper_bound(offset);

	// Check if we can merge with the previous range
	if (next_it != ordered_ranges_.begin()) {
		auto prev_it = std::prev(next_it);
		if (prev_it->second >= offset) {
			// Our range overlaps with the previous one
			offset = prev_it->first;
			end = std::max(end, prev_it->second);
			ordered_ranges_.erase(prev_it);
			end_offset_logical_to_physical_.erase(prev_it->second);
		}
	}

	// Merge with any subsequent overlapping ranges
	// Do not have to be while as ranges will neve overlap but keep it for now
	while (next_it != ordered_ranges_.end() && next_it->first <= end) {
		size_t next_end_logical = next_it->second; // Store logical end before erasing
		auto to_erase = next_it++;
		ordered_ranges_.erase(to_erase);

		if(end < next_end_logical){
			end_offset_logical_to_physical_.erase(end);
			end = next_end_logical;
		}else if (end > next_end_logical){
			end_offset_logical_to_physical_.erase(next_end_logical);
		}
	}

	// Insert the merged range
	ordered_ranges_[offset] = end;

	return GetSequentiallyOrdered();
	// Find the lateset squentially ordered message offset
	if (ordered_ranges_.empty() || ordered_ranges_.begin()->first > 0) {
		return 0;
	}

	return ordered_ranges_.begin()->second;
	// Start with the range that begins at offset 0
	auto current_range_it = ordered_ranges_.begin();
	size_t current_end = current_range_it ->second;

	// Look for adjacent or overlapping ordered_ranges
	auto it = std::next(current_range_it );
	while (it != ordered_ranges_.end() && it->first <= current_end) {
		current_end = std::max(current_end, it->second);
		++it;
	}

	return current_end;
}

} // End of namespace Embarcadero
