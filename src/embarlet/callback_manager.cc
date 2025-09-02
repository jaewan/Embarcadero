#include "callback_manager.h"
#include <glog/logging.h>

namespace Embarcadero {

AsyncCallbackExecutor::AsyncCallbackExecutor(size_t num_threads) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&AsyncCallbackExecutor::WorkerThread, this);
    }
}

AsyncCallbackExecutor::~AsyncCallbackExecutor() {
    Stop();
}

void AsyncCallbackExecutor::Stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void AsyncCallbackExecutor::WorkerThread() {
    while (true) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            
            if (stop_ && tasks_.empty()) {
                return;
            }
            
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        
        task();
    }
}

} // namespace Embarcadero
