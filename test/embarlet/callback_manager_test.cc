#include <gtest/gtest.h>
#include "../../src/embarlet/callback_manager.h"
#include <chrono>

using namespace Embarcadero;
using namespace std::chrono_literals;

class CallbackManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        callback_manager_ = std::make_unique<CallbackManager>();
    }
    
    std::unique_ptr<CallbackManager> callback_manager_;
};

TEST_F(CallbackManagerTest, RegisterAndRetrieveCallback) {
    // Register a callback
    auto test_callback = [](size_t start, size_t end) {
        return start + end;
    };
    
    callback_manager_->RegisterCallback<CallbackManager::BufferCompletionCallback>(
        "test_callback", test_callback
    );
    
    // Retrieve callback
    auto retrieved = callback_manager_->GetCallback<CallbackManager::BufferCompletionCallback>("test_callback");
    
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ((*retrieved)(10, 20), 30);
}

TEST_F(CallbackManagerTest, NonExistentCallback) {
    auto retrieved = callback_manager_->GetCallback<CallbackManager::BufferCompletionCallback>("nonexistent");
    EXPECT_FALSE(retrieved.has_value());
}

TEST_F(CallbackManagerTest, EventPublishSubscribe) {
    struct TestEvent {
        int value;
        std::string message;
    };
    
    bool event_received = false;
    TestEvent received_event;
    
    // Subscribe to event
    callback_manager_->Subscribe<TestEvent>(
        CallbackManager::EventType::BUFFER_ALLOCATED,
        [&](const TestEvent& event) {
            event_received = true;
            received_event = event;
        }
    );
    
    // Publish event
    TestEvent test_event{42, "test message"};
    callback_manager_->Publish(CallbackManager::EventType::BUFFER_ALLOCATED, test_event);
    
    EXPECT_TRUE(event_received);
    EXPECT_EQ(received_event.value, 42);
    EXPECT_EQ(received_event.message, "test message");
}

TEST_F(CallbackManagerTest, MultipleSubscribers) {
    int call_count = 0;
    
    // Subscribe multiple handlers
    for (int i = 0; i < 3; ++i) {
        callback_manager_->Subscribe<int>(
            CallbackManager::EventType::MESSAGE_ORDERED,
            [&call_count, i](const int& value) {
                call_count++;
                EXPECT_EQ(value, 100);
            }
        );
    }
    
    // Publish once
    callback_manager_->Publish(CallbackManager::EventType::MESSAGE_ORDERED, 100);
    
    // All subscribers should be called
    EXPECT_EQ(call_count, 3);
}

TEST_F(CallbackManagerTest, CallbackChain) {
    auto chain = callback_manager_->CreateChain<int, int>();
    
    chain.Then([](int x) { return x * 2; })
         .Then([](int x) { return x + 10; })
         .Then([](int x) { return x / 2; });
    
    int result = chain.Execute(5);
    EXPECT_EQ(result, 10); // (5 * 2 + 10) / 2 = 10
}

TEST_F(CallbackManagerTest, CallbackGuard) {
    bool cleanup_called = false;
    
    {
        CallbackGuard guard([&cleanup_called]() {
            cleanup_called = true;
        });
        
        EXPECT_FALSE(cleanup_called);
    } // Guard goes out of scope
    
    EXPECT_TRUE(cleanup_called);
}

TEST_F(CallbackManagerTest, AsyncCallbackExecutor) {
    AsyncCallbackExecutor executor(2); // 2 worker threads
    
    std::atomic<int> counter{0};
    std::vector<std::future<int>> futures;
    
    // Submit multiple async tasks
    for (int i = 0; i < 10; ++i) {
        futures.push_back(
            executor.ExecuteAsync([&counter, i]() {
                counter++;
                std::this_thread::sleep_for(10ms);
                return i * 2;
            })
        );
    }
    
    // Wait for all tasks
    for (size_t i = 0; i < futures.size(); ++i) {
        EXPECT_EQ(futures[i].get(), i * 2);
    }
    
    EXPECT_EQ(counter, 10);
}

TEST_F(CallbackManagerTest, ThreadSafety) {
    const int num_threads = 4;
    const int ops_per_thread = 100;
    std::atomic<int> success_count{0};
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, &success_count]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                // Register callback
                std::string name = "callback_" + std::to_string(t) + "_" + std::to_string(i);
                callback_manager_->RegisterCallback<CallbackManager::BrokerInfoCallback>(
                    name, [t, i]() { return t * 1000 + i; }
                );
                
                // Retrieve and verify
                auto cb = callback_manager_->GetCallback<CallbackManager::BrokerInfoCallback>(name);
                if (cb.has_value() && (*cb)() == t * 1000 + i) {
                    success_count++;
                }
                
                // Publish event
                callback_manager_->Publish(CallbackManager::EventType::ERROR_OCCURRED, 
                                         std::string("Error from thread " + std::to_string(t)));
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(success_count, num_threads * ops_per_thread);
}
