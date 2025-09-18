#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <random>
#include "../src/common/performance_utils.h"
#include "../src/common/fine_grained_lock.h"
#include "../src/embarlet/zero_copy_buffer.h"

using namespace Embarcadero;

// Benchmark string interning
static void BM_StringInterning(benchmark::State& state) {
    std::vector<std::string> topics;
    for (int i = 0; i < 1000; ++i) {
        topics.push_back("topic_" + std::to_string(i));
    }
    
    StringInternPool& pool = StringInternPool::Instance();
    
    for (auto _ : state) {
        for (const auto& topic : topics) {
            const char* interned = pool.Intern(topic);
            benchmark::DoNotOptimize(interned);
        }
    }
    
    state.SetItemsProcessed(state.iterations() * topics.size());
}
BENCHMARK(BM_StringInterning)->ThreadRange(1, 4)->Iterations(1000);

// Benchmark zero-copy vs regular memcpy
static void BM_RegularMemcpy(benchmark::State& state) {
    size_t size = state.range(0);
    std::vector<char> src(size, 'A');
    std::vector<char> dst(size);
    
    for (auto _ : state) {
        std::memcpy(dst.data(), src.data(), size);
        benchmark::ClobberMemory();
    }
    
    state.SetBytesProcessed(state.iterations() * size);
}
BENCHMARK(BM_RegularMemcpy)->Range(64, 1<<16)->Iterations(10000);

static void BM_OptimizedMemcpy(benchmark::State& state) {
    size_t size = state.range(0);
    std::vector<char> src(size, 'A');
    std::vector<char> dst(size);
    
    for (auto _ : state) {
        OptimizedMemcpy(dst.data(), src.data(), size);
        benchmark::ClobberMemory();
    }
    
    state.SetBytesProcessed(state.iterations() * size);
}
BENCHMARK(BM_OptimizedMemcpy)->Range(64, 1<<16)->Iterations(10000);

// Benchmark zero-copy buffer operations
static void BM_ZeroCopyBuffer(benchmark::State& state) {
    size_t buffer_size = 1 << 20; // 1MB
    std::vector<char> buffer(buffer_size);
    ZeroCopyBuffer zcb(buffer.data(), buffer_size);
    
    for (auto _ : state) {
        // Simulate processing without copying
        auto view = zcb.AsStringView();
        benchmark::DoNotOptimize(view.size());
        
        // Slice operations
        auto slice = zcb.Slice(1024, 4096);
        benchmark::DoNotOptimize(slice.Size());
    }
    
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_ZeroCopyBuffer);

// Benchmark striped locking vs single mutex
static void BM_SingleMutex(benchmark::State& state) {
    std::mutex mutex;
    std::atomic<int> counter{0};
    
    for (auto _ : state) {
        std::lock_guard<std::mutex> lock(mutex);
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SingleMutex)->ThreadRange(1, 8)->Iterations(100000);

static void BM_StripedLock(benchmark::State& state) {
    StripedLock<int, 64> striped;
    std::atomic<int> counter{0};
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 999);
    
    for (auto _ : state) {
        int key = dis(gen);
        StripedLock<int, 64>::ExclusiveLock lock(striped, key);
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StripedLock)->ThreadRange(1, 8)->Iterations(100000);

// Benchmark SPSC queue
static void BM_SPSCQueue(benchmark::State& state) {
    SPSCQueue<int, 1024> queue;
    
    if (state.thread_index() == 0) {
        // Producer
        int value = 0;
        for (auto _ : state) {
            while (!queue.TryPush(value)) {
                std::this_thread::yield();
            }
            ++value;
        }
    } else {
        // Consumer
        int value;
        for (auto _ : state) {
            while (!queue.TryPop(value)) {
                std::this_thread::yield();
            }
            benchmark::DoNotOptimize(value);
        }
    }
    
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue)->ThreadRange(2, 2);

// Benchmark cache-aligned vs non-aligned structures
struct NonAligned {
    std::atomic<int> counter1{0};
    std::atomic<int> counter2{0};
};

struct CacheAligned {
    alignas(64) std::atomic<int> counter1{0};
    alignas(64) std::atomic<int> counter2{0};
};

static void BM_NonAlignedFalseSharing(benchmark::State& state) {
    static NonAligned data;
    
    if (state.thread_index() == 0) {
        for (auto _ : state) {
            data.counter1.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        for (auto _ : state) {
            data.counter2.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NonAlignedFalseSharing)->ThreadRange(2, 2);

static void BM_CacheAlignedNoFalseSharing(benchmark::State& state) {
    static CacheAligned data;
    
    if (state.thread_index() == 0) {
        for (auto _ : state) {
            data.counter1.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        for (auto _ : state) {
            data.counter2.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CacheAlignedNoFalseSharing)->ThreadRange(2, 2);

BENCHMARK_MAIN();
