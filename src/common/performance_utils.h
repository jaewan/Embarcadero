#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <atomic>
#include <array>
#include <cstdint>
#include <chrono>
#include <glog/logging.h>  // For CHECK_LT

// Architecture-specific intrinsics
#ifdef __x86_64__
#include <immintrin.h>  // For _mm_clflushopt, _mm_sfence, _mm_lfence
#include <xmmintrin.h>  // For _mm_pause
#endif

// Forward declaration for BlogMessageHeader (defined in cxl_datastructure.h)
namespace Embarcadero {
struct BlogMessageHeader;
}

namespace Embarcadero {

// String interning pool for topic names to avoid repeated allocations
// Uses sharding to reduce lock contention in multi-threaded scenarios
class StringInternPool {
public:
    static constexpr size_t kNumShards = 16;
    
    static StringInternPool& Instance() {
        static StringInternPool instance;
        return instance;
    }

    // Intern a string and return a pointer to the interned version
    const char* Intern(const std::string_view& str) {
        size_t shard_idx = std::hash<std::string_view>{}(str) % kNumShards;
        auto& shard = shards_[shard_idx];
        
        // Try read lock first for better performance
        {
            std::shared_lock lock(shard.mutex);
            auto it = shard.pool.find(std::string(str));
            if (it != shard.pool.end()) {
                return it->first.c_str();
            }
        }
        
        // Need to insert, take write lock
        std::unique_lock lock(shard.mutex);
        // Double-check after acquiring write lock
        auto it = shard.pool.find(std::string(str));
        if (it != shard.pool.end()) {
            return it->first.c_str();
        }
        
        auto [inserted_it, success] = shard.pool.emplace(std::string(str), true);
        return inserted_it->first.c_str();
    }

    // Get interned string without locking (for read-only access)
    const char* GetInterned(const std::string_view& str) const {
        size_t shard_idx = std::hash<std::string_view>{}(str) % kNumShards;
        auto& shard = shards_[shard_idx];
        
        std::shared_lock lock(shard.mutex);
        auto it = shard.pool.find(std::string(str));
        if (it != shard.pool.end()) {
            return it->first.c_str();
        }
        return nullptr;
    }

private:
    struct alignas(64) Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, bool> pool;
    };
    
    mutable std::array<Shard, kNumShards> shards_;
    
    StringInternPool() = default;
    StringInternPool(const StringInternPool&) = delete;
    StringInternPool& operator=(const StringInternPool&) = delete;
};

// Zero-copy buffer view for message passing
class ZeroCopyBuffer {
public:
    ZeroCopyBuffer(void* data, size_t size) 
        : data_(data), size_(size) {}
    
    // Get a view of the buffer without copying
    std::string_view AsStringView() const {
        return std::string_view(static_cast<const char*>(data_), size_);
    }
    
    // Direct memory access
    void* Data() { return data_; }
    const void* Data() const { return data_; }
    size_t Size() const { return size_; }
    
    // Zero-copy slice
    ZeroCopyBuffer Slice(size_t offset, size_t length) const {
        if (offset + length > size_) {
            throw std::out_of_range("Slice out of bounds");
        }
        return ZeroCopyBuffer(static_cast<char*>(data_) + offset, length);
    }

private:
    void* data_;
    size_t size_;
};

// Use standard memcpy which is already highly optimized
inline void OptimizedMemcpy(void* dest, const void* src, size_t size) {
    std::memcpy(dest, src, size);
}

// Lock-free single producer single consumer queue for message passing
template<typename T, size_t Size>
class SPSCQueue {
public:
    SPSCQueue() : head_(0), tail_(0) {}
    
    bool TryPush(const T& item) {
        size_t next_head = (head_.load(std::memory_order_relaxed) + 1) % Size;
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue full
        }
        
        buffer_[head_.load(std::memory_order_relaxed)] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }
    
    bool TryPop(T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        if (current_tail == head_.load(std::memory_order_acquire)) {
            return false; // Queue empty
        }
        
        item = buffer_[current_tail];
        tail_.store((current_tail + 1) % Size, std::memory_order_release);
        return true;
    }
    
    bool Empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    alignas(64) T buffer_[Size];
};

// Cache coherence primitives for non-coherent CXL memory
// Note: DEV-002 (batched flushes) is planned but not yet implemented
// See docs/memory-bank/spec_deviation.md DEV-002 for future optimization
namespace CXL {

/**
 * @brief Flush a cache line containing the given address to CXL memory
 *
 * @threading Thread-safe (CPU instruction, no synchronization needed)
 * @ownership Does not take ownership of addr (read-only parameter)
 * @alignment Works on any address (automatically rounds down to cache line boundary)
 * @paper_ref Paper §4.2 - Uses clflushopt for non-coherent CXL writes
 *
 * @param addr Pointer to any address within the cache line to flush
 *
 * Implementation:
 * - x86-64: _mm_clflushopt (optimized, non-blocking flush)
 * - ARM64: __builtin___clear_cache (DC CVAC instruction)
 * - Generic: No-op (assumes cache-coherent memory)
 *
 * Performance: HOT PATH - Called millions of times per second
 * Constraints: Must be followed by store_fence() for ordering guarantees
 *
 * Usage pattern (Paper spec):
 *   msg_header->field = value;
 *   CXL::flush_cacheline(msg_header);
 *   CXL::store_fence();
 *
 * Future optimization (DEV-002):
 *   Multiple field writes to same cache line → single flush
 */
inline void flush_cacheline(const void* addr) {
#ifdef __x86_64__
    // Safe cast: _mm_clflushopt doesn't modify memory, only flushes cache line
    _mm_clflushopt(const_cast<void*>(addr));
#elif defined(__aarch64__)
    // ARM64: Clear cache for 64-byte aligned region
    const uintptr_t aligned_addr = reinterpret_cast<uintptr_t>(addr) & ~63UL;
    __builtin___clear_cache(
        reinterpret_cast<char*>(aligned_addr),
        reinterpret_cast<char*>(aligned_addr + 64)
    );
#else
    // Generic fallback: assume cache-coherent memory (no-op)
    (void)addr;
#endif
}

/**
 * @brief Store fence - ensures all prior stores and flushes are visible
 *
 * @threading Thread-safe (CPU instruction)
 * @ownership No ownership semantics
 * @paper_ref Paper §4.2 - sfence after clflushopt for ordering
 *
 * Guarantees:
 * - All stores before this fence complete before stores after
 * - All cache flushes before this fence complete before stores after
 * - Required after flush_cacheline() to ensure CXL visibility
 *
 * Implementation:
 * - x86-64: _mm_sfence() (store fence instruction)
 * - ARM64: dmb st (data memory barrier for stores)
 * - Generic: __atomic_thread_fence(__ATOMIC_RELEASE)
 *
 * Performance: HOT PATH - Called after every cache flush
 * Critical: Must be called after flush_cacheline() for correctness
 */
inline void store_fence() {
#ifdef __x86_64__
    _mm_sfence();
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb st" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

/**
 * @brief Selective cache flush for BlogMessageHeader receiver region (bytes 0-15)
 *
 * @threading Thread-safe (CPU instruction, no synchronization needed)
 * @ownership Does not take ownership of hdr (read-only parameter)
 * @alignment Works on any BlogMessageHeader address
 * @paper_ref Paper §2.B Table 4 - Receiver writes bytes 0-15 only
 *
 * @param hdr Pointer to BlogMessageHeader
 *
 * Flushes only the cache line containing bytes 0-15 (receiver region).
 * This is more efficient than flushing the entire 64-byte header when only
 * the receiver region has been modified.
 *
 * Performance: HOT PATH - Called after every message receive
 * Usage: After writing size, received, ts fields
 *
 * Note: For BlogMessageHeader, bytes 0-15 are in the first cache line (bytes 0-63).
 * All three regions (0-15, 16-31, 32-47) share the same cache line, so all three
 * flush functions target the same cache line. The separate functions provide
 * semantic clarity about which stage is flushing.
 */
inline void flush_blog_receiver_region(const BlogMessageHeader* hdr) {
    // Bytes 0-15 are in the first cache line (bytes 0-63)
    // Flush the cache line containing the header start
    flush_cacheline(hdr);
}

/**
 * @brief Selective cache flush for BlogMessageHeader delegation region (bytes 16-31)
 *
 * @threading Thread-safe (CPU instruction, no synchronization needed)
 * @ownership Does not take ownership of hdr (read-only parameter)
 * @alignment Works on any BlogMessageHeader address
 * @paper_ref Paper §2.B Table 4 - Delegation writes bytes 16-31 only
 *
 * @param hdr Pointer to BlogMessageHeader
 *
 * Flushes only the cache line containing bytes 16-31 (delegation region).
 * Bytes 16-31 are in the first cache line (bytes 0-63), so this flushes
 * the same cache line as receiver region, but semantically indicates
 * we're flushing the delegation fields.
 *
 * Performance: HOT PATH - Called after every message delegation
 * Usage: After writing counter, flags, processed_ts fields
 */
inline void flush_blog_delegation_region(const BlogMessageHeader* hdr) {
    // Bytes 16-31 are in the first cache line (bytes 0-63)
    // Flush the cache line containing the header start
    flush_cacheline(hdr);
}

/**
 * @brief Selective cache flush for BlogMessageHeader sequencer region (bytes 32-47)
 *
 * @threading Thread-safe (CPU instruction, no synchronization needed)
 * @ownership Does not take ownership of hdr (read-only parameter)
 * @alignment Works on any BlogMessageHeader address
 * @paper_ref Paper §2.B Table 4 - Sequencer writes bytes 32-47 only
 *
 * @param hdr Pointer to BlogMessageHeader
 *
 * Flushes only the cache line containing bytes 32-47 (sequencer region).
 * Bytes 32-47 are in the first cache line (bytes 0-63), so this flushes
 * the same cache line as receiver/delegation regions, but semantically indicates
 * we're flushing the sequencer fields.
 *
 * Performance: HOT PATH - Called after every message ordering
 * Usage: After writing total_order, ordered_ts fields
 *
 * Note: For BlogMessageHeader, all three regions (0-15, 16-31, 32-47) are in
 * the same cache line, so all three flush functions target the same cache line.
 * The separate functions provide semantic clarity about which stage is flushing.
 */
inline void flush_blog_sequencer_region(const BlogMessageHeader* hdr) {
    // Bytes 32-47 are in the first cache line (bytes 0-63)
    // Flush the cache line containing the header start
    flush_cacheline(hdr);
}

/**
 * @brief Load fence - ensures all prior loads are visible
 *
 * @threading Thread-safe (CPU instruction)
 * @ownership No ownership semantics
 * @paper_ref Paper §4.2 - lfence for load ordering (used in polling loops)
 *
 * Guarantees:
 * - All loads before this fence complete before loads after
 * - Does NOT invalidate cache or force refetch from memory
 * - For fresh reads from non-coherent CXL, use __atomic_load_n with ACQUIRE
 *
 * Implementation:
 * - x86-64: _mm_lfence() (load fence instruction)
 * - ARM64: dmb ld (data memory barrier for loads)
 * - Generic: __atomic_thread_fence(__ATOMIC_ACQUIRE)
 *
 * Performance: COLD PATH - Used in polling loops, not hot write path
 */
inline void load_fence() {
#ifdef __x86_64__
    _mm_lfence();
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb ld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief Cache prefetch hint - prefetch data into cache
 *
 * @threading Thread-safe (CPU instruction)
 * @ownership No ownership semantics
 * @paper_ref Performance optimization - prefetch next batch/message
 *
 * Purpose:
 * - Prefetch data into cache before it's needed
 * - Reduces cache miss latency in hot loops
 * - Improves pipeline utilization
 *
 * @param addr Address to prefetch (will be rounded to cache line)
 * @param locality Locality hint: 0=no locality, 1=low, 2=moderate, 3=high (default: 3)
 *
 * Implementation:
 * - x86-64: _mm_prefetch() (PREFETCH instruction)
 *   - Locality 0: _MM_HINT_NTA (non-temporal, all levels)
 *   - Locality 1: _MM_HINT_T1 (L2 and above)
 *   - Locality 2: _MM_HINT_T2 (L3 and above)
 *   - Locality 3: _MM_HINT_T0 (all cache levels) - default
 * - ARM64: __builtin_prefetch()
 * - Generic: __builtin_prefetch()
 *
 * Performance: HOT PATH - Used in tight loops to prefetch next iteration
 *
 * Example:
 *   // Prefetch next batch header while processing current
 *   CXL::prefetch_cacheline(next_batch_header);
 */
inline void prefetch_cacheline(const void* addr, int locality = 3) {
#ifdef __x86_64__
    // _MM_HINT_* are enum values that can be cast to int
    int hint_val;
    switch (locality) {
        case 0: hint_val = static_cast<int>(_MM_HINT_NTA); break;  // Non-temporal
        case 1: hint_val = static_cast<int>(_MM_HINT_T1); break;   // L2 and above
        case 2: hint_val = static_cast<int>(_MM_HINT_T2); break;   // L3 and above
        case 3: 
        default: hint_val = static_cast<int>(_MM_HINT_T0); break;  // All cache levels (default)
    }
    _mm_prefetch(reinterpret_cast<const char*>(addr), static_cast<_mm_hint>(hint_val));
#elif defined(__aarch64__)
    __builtin_prefetch(addr, 0, locality);
#else
    __builtin_prefetch(addr, 0, locality);
#endif
}

/**
 * @brief CPU pause hint for spin-wait loops
 *
 * @threading Thread-safe (CPU instruction)
 * @ownership No ownership semantics
 * @paper_ref Paper §3 - Polling loops in receiver/delegation threads
 *
 * Purpose:
 * - Reduces power consumption during busy-wait
 * - Improves performance on hyperthreaded CPUs
 * - Prevents pipeline stalls in tight polling loops
 *
 * Implementation:
 * - x86-64: _mm_pause() (PAUSE instruction)
 * - ARM64: yield hint (YIELD instruction)
 * - Generic: compiler barrier
 *
 * Performance: HOT PATH - Called in every iteration of polling loops
 * Usage: Always use in busy-wait loops to avoid wasting CPU cycles
 *
 * Example:
 *   while (!flag) {
 *       CXL::cpu_pause();
 *   }
 */
inline void cpu_pause() {
#ifdef __x86_64__
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    // Generic fallback: compiler barrier
    __asm__ __volatile__("" ::: "memory");
#endif
}

/**
 * @brief Read Time Stamp Counter - high-resolution CPU cycle timestamp
 *
 * @threading Thread-safe (CPU instruction, no synchronization needed)
 * @ownership No ownership semantics
 * @paper_ref Paper §3 - Timestamp fields in BlogMessageHeader (ts, processed_ts, ordered_ts)
 *
 * Purpose:
 * - Provides high-resolution timestamps for latency measurements
 * - CPU cycle-accurate timing (typically ~3-4 GHz = ~0.25-0.33 ns resolution)
 * - Used in BlogMessageHeader for tracking message processing stages
 *
 * Returns:
 * - uint64_t: Current value of CPU's Time Stamp Counter register
 * - Monotonically increasing (except on CPU migration or frequency changes)
 * - Wraps around after ~200 years at 3 GHz (2^64 / 3e9 seconds)
 *
 * Implementation:
 * - x86-64: RDTSC instruction (via __rdtsc() intrinsic or inline assembly)
 * - ARM64: System timer via CNTVCT_EL0 register (virtual counter)
 * - Generic: std::chrono::steady_clock fallback (lower resolution)
 *
 * Performance: HOT PATH - Called for every message timestamp
 * Constraints: Must be fast (<10 cycles ideally)
 *
 * Usage in BlogMessageHeader:
 *   blog_hdr->ts = CXL::rdtsc();              // Receiver stage
 *   blog_hdr->processed_ts = CXL::rdtsc();    // Delegation stage
 *   blog_hdr->ordered_ts = CXL::rdtsc();      // Sequencer stage
 *
 * Note: TSC frequency may vary with CPU frequency scaling. For absolute time
 * conversion, use TSC frequency calibration (not provided here - use for relative
 * latency measurements only).
 *
 * Example latency calculation:
 *   uint64_t start = CXL::rdtsc();
 *   // ... do work ...
 *   uint64_t end = CXL::rdtsc();
 *   uint64_t cycles = end - start;
 *   // Convert to nanoseconds: cycles / tsc_frequency_ghz
 */
inline uint64_t rdtsc() {
#ifdef __x86_64__
    // Use compiler intrinsic if available (GCC/Clang)
    #if defined(__GNUC__) || defined(__clang__)
        return __rdtsc();
    #else
        // Fallback to inline assembly
        uint64_t low, high;
        __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
        return (high << 32) | low;
    #endif
#elif defined(__aarch64__)
    // ARM64: Read virtual counter (CNTVCT_EL0)
    // This provides a virtualized counter that's consistent across cores
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    // Generic fallback: Use steady_clock for cross-platform compatibility
    // Note: Lower resolution than TSC, but provides monotonic timestamps
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    // Scale to approximate TSC frequency (assume 3 GHz for conversion)
    // This is approximate - for accurate measurements, use TSC on x86-64
    return static_cast<uint64_t>(nanoseconds * 3);  // 3 GHz = 3 cycles per nanosecond
#endif
}

} // namespace CXL

/**
 * @brief Compute replication set for a given broker (replication_factor includes self)
 *
 * @threading Thread-safe (pure function, no shared state)
 * @ownership No ownership semantics
 * @paper_ref Paper §3.4 - Stage 4: Replication threads per broker
 *
 * Semantics: replication_factor INCLUDES self
 * - replication_factor=1: {self} (local durability only)
 * - replication_factor=2: {self, next_broker} (self + 1 replica)
 * - replication_factor=N: {self, next_broker, ..., (self+N-1) % num_brokers}
 *
 * @param broker_id The broker whose replication set to compute
 * @param replication_factor Number of replicas (including self)
 * @param num_brokers Total number of brokers in cluster
 * @param replica_index Index in [0, replication_factor) to get specific replica
 * @return Broker ID of the replica at replica_index
 *
 * Example:
 *   GetReplicationSetBroker(0, 2, 4, 0) -> 0 (self)
 *   GetReplicationSetBroker(0, 2, 4, 1) -> 1 (next broker)
 */
inline int GetReplicationSetBroker(int broker_id, int replication_factor, int num_brokers, int replica_index) {
	// Pattern: (broker_id + replica_index) % num_brokers
	// This ensures self is always at index 0, next broker at index 1, etc.
	if (replica_index >= replication_factor) {
		LOG(FATAL) << "replica_index (" << replica_index << ") must be < replication_factor (" << replication_factor << ")";
	}
	return (broker_id + replica_index) % num_brokers;
}

} // namespace Embarcadero
