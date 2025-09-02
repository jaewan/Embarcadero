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

} // namespace Embarcadero
