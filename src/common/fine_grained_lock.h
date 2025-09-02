#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <shared_mutex>
#include <mutex>
#include <stdexcept>
#include <climits>
#include <absl/hash/hash.h>

namespace Embarcadero {

// Striped lock for fine-grained locking based on key hash
template<typename Key, size_t NumStripes = 64>
class StripedLock {
public:
    StripedLock() = default;
    
    // Get lock index for a given key
    size_t GetStripeIndex(const Key& key) const {
        return absl::Hash<Key>{}(key) % NumStripes;
    }
    
    // Lock for exclusive access
    void Lock(const Key& key) {
        locks_[GetStripeIndex(key)].mutex.lock();
    }
    
    // Unlock exclusive access
    void Unlock(const Key& key) {
        locks_[GetStripeIndex(key)].mutex.unlock();
    }
    
    // Lock for shared access
    void LockShared(const Key& key) {
        locks_[GetStripeIndex(key)].mutex.lock_shared();
    }
    
    // Unlock shared access
    void UnlockShared(const Key& key) {
        locks_[GetStripeIndex(key)].mutex.unlock_shared();
    }
    
    // RAII lock guard for exclusive access
    class ExclusiveLock {
    public:
        ExclusiveLock(StripedLock& striped, const Key& key)
            : striped_(striped), key_(key) {
            striped_.Lock(key_);
        }
        
        ~ExclusiveLock() {
            striped_.Unlock(key_);
        }
        
        ExclusiveLock(const ExclusiveLock&) = delete;
        ExclusiveLock& operator=(const ExclusiveLock&) = delete;
        
    private:
        StripedLock& striped_;
        Key key_;
    };
    
    // RAII lock guard for shared access
    class SharedLock {
    public:
        SharedLock(StripedLock& striped, const Key& key)
            : striped_(striped), key_(key) {
            striped_.LockShared(key_);
        }
        
        ~SharedLock() {
            striped_.UnlockShared(key_);
        }
        
        SharedLock(const SharedLock&) = delete;
        SharedLock& operator=(const SharedLock&) = delete;
        
    private:
        StripedLock& striped_;
        Key key_;
    };

private:
    struct alignas(64) CacheAlignedMutex {
        std::shared_mutex mutex;
    };
    std::array<CacheAlignedMutex, NumStripes> locks_;
};

// Optimistic locking with version numbers
template<typename T>
class OptimisticLock {
public:
    struct VersionedData {
        T data;
        std::atomic<uint64_t> version{0};
    };
    
    OptimisticLock() = default;
    
    // Read data with version
    std::pair<T, uint64_t> Read() const {
        uint64_t ver = version_.load(std::memory_order_acquire);
        T data = data_;
        std::atomic_thread_fence(std::memory_order_acquire);
        
        // Verify version hasn't changed during read
        if (version_.load(std::memory_order_relaxed) != ver) {
            // Retry with lock
            std::shared_lock lock(mutex_);
            ver = version_.load(std::memory_order_relaxed);
            data = data_;
        }
        
        return {data, ver};
    }
    
    // Try to update with expected version
    bool TryUpdate(const T& new_data, uint64_t expected_version) {
        std::unique_lock lock(mutex_);
        
        if (version_.load(std::memory_order_relaxed) != expected_version) {
            return false;
        }
        
        data_ = new_data;
        version_.fetch_add(1, std::memory_order_release);
        return true;
    }
    
    // Force update (ignores version)
    void ForceUpdate(const T& new_data) {
        std::unique_lock lock(mutex_);
        data_ = new_data;
        version_.fetch_add(1, std::memory_order_release);
    }

private:
    mutable std::shared_mutex mutex_;
    T data_;
    std::atomic<uint64_t> version_{0};
};

// Hierarchical locking to prevent deadlocks
class HierarchicalMutex {
public:
    explicit HierarchicalMutex(unsigned long level)
        : hierarchy_level_(level), previous_level_(0) {}
    
    void lock() {
        check_for_hierarchy_violation();
        internal_mutex_.lock();
        update_hierarchy_value();
    }
    
    void unlock() {
        if (thread_hierarchy_level_ != hierarchy_level_) {
            throw std::logic_error("Mutex hierarchy violated");
        }
        thread_hierarchy_level_ = previous_level_;
        internal_mutex_.unlock();
    }
    
    bool try_lock() {
        check_for_hierarchy_violation();
        if (!internal_mutex_.try_lock()) {
            return false;
        }
        update_hierarchy_value();
        return true;
    }

private:
    std::mutex internal_mutex_;
    unsigned long const hierarchy_level_;
    unsigned long previous_level_;
    static thread_local unsigned long thread_hierarchy_level_;
    
    void check_for_hierarchy_violation() {
        if (thread_hierarchy_level_ <= hierarchy_level_) {
            throw std::logic_error("Mutex hierarchy violated");
        }
    }
    
    void update_hierarchy_value() {
        previous_level_ = thread_hierarchy_level_;
        thread_hierarchy_level_ = hierarchy_level_;
    }
};

// Initialize thread-local storage
inline thread_local unsigned long HierarchicalMutex::thread_hierarchy_level_ = ULONG_MAX;

} // namespace Embarcadero
