#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "common/fault_injection.h"

namespace Embarcadero {
// Connection admission queue, not a per-message data-path queue. Closing wakes
// both producers and consumers without inserting sentinels into a full queue.
template <class T> class CancellableQueue {
public:
    explicit CancellableQueue(size_t capacity) : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("queue capacity must be positive");
    }
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mu_);
#if defined(EMBARCADERO_ENABLE_FAULT_INJECTION) && EMBARCADERO_ENABLE_FAULT_INJECTION
        if (!closed_ && items_.size() == capacity_ &&
            !fault::Pause("queue.push.blocked",
                {UINT64_MAX, UINT64_MAX, UINT64_MAX, items_.size(), capacity_})) return false;
#endif
        changed_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_) return false;
        items_.push_back(std::move(item));
        changed_.notify_all();
        return true;
    }
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mu_);
        changed_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (closed_) return false;
        item = std::move(items_.front());
        items_.pop_front();
        changed_.notify_all();
        return true;
    }
    std::deque<T> close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        std::deque<T> remaining;
        remaining.swap(items_);
        changed_.notify_all();
        return remaining;
    }
private:
    const size_t capacity_;
    std::mutex mu_;
    std::condition_variable changed_;
    std::deque<T> items_;
    bool closed_{false};
};
}  // namespace Embarcadero
