#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <any>
#include <typeindex>
#include <mutex>
#include "../common/common.h"

namespace Embarcadero {

/**
 * Modern callback management system using C++17 features
 * Replaces old-style function pointers with type-safe std::function
 */
class CallbackManager {
public:
    // Common callback types
    using BufferCompletionCallback = std::function<void(size_t start_offset, size_t end_offset)>;
    using SegmentAllocationCallback = std::function<void*(size_t size, size_t msg_size, size_t& segment_size, SegmentMetadata& metadata)>;
    using BrokerInfoCallback = std::function<int()>;
    using BrokerSetCallback = std::function<bool(absl::btree_set<int>&, TInode*)>;
    using ReplicationCallback = std::function<void(size_t log_idx, size_t total_size, void* data)>;
    using ErrorCallback = std::function<void(const std::string& error_msg)>;
    
    // Event types for publish-subscribe pattern
    enum class EventType {
        BUFFER_ALLOCATED,
        SEGMENT_ALLOCATED,
        MESSAGE_ORDERED,
        REPLICATION_COMPLETE,
        ERROR_OCCURRED
    };

    CallbackManager() = default;
    ~CallbackManager() = default;

    // Register typed callbacks
    template<typename CallbackType>
    void RegisterCallback(const std::string& name, CallbackType callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        typed_callbacks_[std::type_index(typeid(CallbackType))][name] = callback;
    }

    // Get typed callback
    template<typename CallbackType>
    std::optional<CallbackType> GetCallback(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto type_it = typed_callbacks_.find(std::type_index(typeid(CallbackType)));
        if (type_it != typed_callbacks_.end()) {
            auto cb_it = type_it->second.find(name);
            if (cb_it != type_it->second.end()) {
                return std::any_cast<CallbackType>(cb_it->second);
            }
        }
        return std::nullopt;
    }

    // Event subscription
    template<typename EventData>
    void Subscribe(EventType event, std::function<void(const EventData&)> handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        event_handlers_[event].emplace_back(
            [handler](const std::any& data) {
                handler(std::any_cast<const EventData&>(data));
            }
        );
    }

    // Event publishing
    template<typename EventData>
    void Publish(EventType event, const EventData& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = event_handlers_.find(event);
        if (it != event_handlers_.end()) {
            for (const auto& handler : it->second) {
                handler(data);
            }
        }
    }

    // Callback chaining
    template<typename Result, typename... Args>
    class CallbackChain {
    public:
        using Callback = std::function<Result(Args...)>;
        
        CallbackChain& Then(Callback callback) {
            callbacks_.push_back(callback);
            return *this;
        }

        Result Execute(Args... args) {
            Result result{};
            for (const auto& callback : callbacks_) {
                result = callback(args...);
            }
            return result;
        }

    private:
        std::vector<Callback> callbacks_;
    };

    // Create callback chain
    template<typename Result, typename... Args>
    CallbackChain<Result, Args...> CreateChain() {
        return CallbackChain<Result, Args...>();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, std::unordered_map<std::string, std::any>> typed_callbacks_;
    std::unordered_map<EventType, std::vector<std::function<void(const std::any&)>>> event_handlers_;
};

/**
 * Scoped callback guard for automatic cleanup
 */
class CallbackGuard {
public:
    CallbackGuard(std::function<void()> cleanup) : cleanup_(cleanup) {}
    ~CallbackGuard() { if (cleanup_) cleanup_(); }
    
    // Disable copy
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
    
    // Enable move
    CallbackGuard(CallbackGuard&& other) noexcept : cleanup_(std::move(other.cleanup_)) {
        other.cleanup_ = nullptr;
    }

private:
    std::function<void()> cleanup_;
};

/**
 * Async callback executor with thread pool
 */
class AsyncCallbackExecutor {
public:
    AsyncCallbackExecutor(size_t num_threads = 4);
    ~AsyncCallbackExecutor();

    // Execute callback asynchronously
    template<typename Callback, typename... Args>
    auto ExecuteAsync(Callback&& callback, Args&&... args) 
        -> std::future<decltype(callback(args...))> {
        using ReturnType = decltype(callback(args...));
        
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [callback = std::forward<Callback>(callback), 
             ... args = std::forward<Args>(args)]() {
                return callback(args...);
            }
        );
        
        auto future = task->get_future();
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            tasks_.emplace([task]() { (*task)(); });
        }
        
        condition_.notify_one();
        return future;
    }

    void Stop();

private:
    void WorkerThread();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_{false};
};

} // namespace Embarcadero
