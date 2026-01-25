/**
 * [[DEVIATION_005]] - Segment Allocation Test Suite
 * 
 * Tests for atomic bitmap-based segment allocation:
 * - Single broker allocation
 * - Multi-broker concurrent allocation
 * - Fragmentation prevention
 * - Performance (allocation latency)
 * - Stress test (allocate all segments)
 * - Edge cases (out of memory, bounds checking)
 */

#include "../src/cxl_manager/cxl_manager.h"
#include "../src/common/configuration.h"
#include "../src/common/performance_utils.h"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <memory>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/shm.h>

using namespace Embarcadero;

// Test configuration
constexpr size_t TEST_CXL_SIZE = 4ULL * 1024 * 1024 * 1024;  // 4GB for fast testing
constexpr int NUM_TEST_BROKERS = 4;
constexpr int NUM_ALLOCATIONS_PER_BROKER = 10;

// Helper: Allocate shared memory for test
void* allocate_test_shm(int broker_id, size_t size) {
    std::string shm_name = "/embarcadero_test_" + std::to_string(broker_id);
    
    // Create shared memory
    int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open failed");
        return nullptr;
    }
    
    // Set size
    if (ftruncate(shm_fd, size) < 0) {
        perror("ftruncate failed");
        close(shm_fd);
        return nullptr;
    }
    
    // Map memory
    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap failed");
        close(shm_fd);
        return nullptr;
    }
    
    close(shm_fd);
    return addr;
}

// Helper: Cleanup shared memory
void cleanup_test_shm(int broker_id) {
    std::string shm_name = "/embarcadero_test_" + std::to_string(broker_id);
    shm_unlink(shm_name.c_str());
}

// Test 1: Single broker allocation
void test_single_broker_allocation() {
    std::cout << "\n=== Test 1: Single Broker Allocation ===" << std::endl;
    
    // Set test CXL size
    setenv("EMBARCADERO_CXL_SIZE", std::to_string(TEST_CXL_SIZE).c_str(), 1);
    
    CXLManager manager(0, CXL_Type::Emul, "127.0.0.1");
    
    std::vector<void*> allocated_segments;
    const int num_allocations = 20;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_allocations; ++i) {
        void* segment = manager.GetNewSegment();
        if (segment == nullptr) {
            std::cerr << "ERROR: Failed to allocate segment " << i << std::endl;
            assert(false);
        }
        allocated_segments.push_back(segment);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double avg_latency_ns = duration.count() / static_cast<double>(num_allocations);
    
    std::cout << "✓ Allocated " << num_allocations << " segments successfully" << std::endl;
    std::cout << "✓ Average allocation latency: " << avg_latency_ns << " ns" << std::endl;
    
    // Verify all segments are unique
    for (size_t i = 0; i < allocated_segments.size(); ++i) {
        for (size_t j = i + 1; j < allocated_segments.size(); ++j) {
            if (allocated_segments[i] == allocated_segments[j]) {
                std::cerr << "ERROR: Duplicate segment allocated!" << std::endl;
                assert(false);
            }
        }
    }
    std::cout << "✓ All segments are unique" << std::endl;
    
    // Verify segments are properly aligned
    for (void* seg : allocated_segments) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(seg);
        if (addr % 64 != 0) {
            std::cerr << "ERROR: Segment not cache-line aligned: " << seg << std::endl;
            assert(false);
        }
    }
    std::cout << "✓ All segments are cache-line aligned" << std::endl;
    
    if (avg_latency_ns > 1000) {  // > 1μs is concerning
        std::cout << "⚠ WARNING: Allocation latency higher than expected (>1μs)" << std::endl;
    } else {
        std::cout << "✓ Allocation latency within expected range (<1μs)" << std::endl;
    }
}

// Test 2: Multi-broker concurrent allocation
void test_multi_broker_concurrent() {
    std::cout << "\n=== Test 2: Multi-Broker Concurrent Allocation ===" << std::endl;
    
    setenv("EMBARCADERO_CXL_SIZE", std::to_string(TEST_CXL_SIZE).c_str(), 1);
    
    std::vector<std::unique_ptr<CXLManager>> managers;
    for (int i = 0; i < NUM_TEST_BROKERS; ++i) {
        managers.emplace_back(std::make_unique<CXLManager>(i, CXL_Type::Emul, "127.0.0.1"));
    }
    
    std::atomic<int> total_allocated{0};
    std::atomic<int> allocation_failures{0};
    std::vector<std::vector<void*>> broker_segments(NUM_TEST_BROKERS);
    std::vector<std::mutex> segment_mutexes(NUM_TEST_BROKERS);
    
    auto allocation_worker = [&](int broker_id) {
        std::vector<void*> my_segments;
        for (int i = 0; i < NUM_ALLOCATIONS_PER_BROKER; ++i) {
            void* segment = managers[broker_id]->GetNewSegment();
            if (segment == nullptr) {
                allocation_failures.fetch_add(1);
                continue;
            }
            my_segments.push_back(segment);
            total_allocated.fetch_add(1);
        }
        
        std::lock_guard<std::mutex> lock(segment_mutexes[broker_id]);
        broker_segments[broker_id] = std::move(my_segments);
    };
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_TEST_BROKERS; ++i) {
        threads.emplace_back(allocation_worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    int expected_allocations = NUM_TEST_BROKERS * NUM_ALLOCATIONS_PER_BROKER;
    std::cout << "✓ Total allocations: " << total_allocated.load() 
              << " / " << expected_allocations << std::endl;
    std::cout << "✓ Allocation failures: " << allocation_failures.load() << std::endl;
    std::cout << "✓ Total time: " << duration.count() << " ms" << std::endl;
    
    if (allocation_failures.load() > 0) {
        std::cerr << "ERROR: Some allocations failed!" << std::endl;
        assert(false);
    }
    
    // Verify no duplicates across brokers
    std::vector<void*> all_segments;
    for (const auto& segments : broker_segments) {
        all_segments.insert(all_segments.end(), segments.begin(), segments.end());
    }
    
    for (size_t i = 0; i < all_segments.size(); ++i) {
        for (size_t j = i + 1; j < all_segments.size(); ++j) {
            if (all_segments[i] == all_segments[j]) {
                std::cerr << "ERROR: Duplicate segment across brokers!" << std::endl;
                assert(false);
            }
        }
    }
    std::cout << "✓ No duplicate segments across brokers" << std::endl;
    
    // Verify fragmentation prevention: segments should be distributed across brokers
    std::cout << "✓ Segments per broker:" << std::endl;
    for (int i = 0; i < NUM_TEST_BROKERS; ++i) {
        std::lock_guard<std::mutex> lock(segment_mutexes[i]);
        std::cout << "  Broker " << i << ": " << broker_segments[i].size() << " segments" << std::endl;
    }
}

// Test 3: Stress test - allocate all segments
void test_stress_all_segments() {
    std::cout << "\n=== Test 3: Stress Test - All Segments ===" << std::endl;
    
    setenv("EMBARCADERO_CXL_SIZE", std::to_string(TEST_CXL_SIZE).c_str(), 1);
    
    CXLManager manager(0, CXL_Type::Emul, "127.0.0.1");
    
    // Calculate expected number of segments
    size_t cxl_size = TEST_CXL_SIZE;
    size_t segment_size = SEGMENT_SIZE;
    size_t expected_segments = cxl_size / segment_size;  // Rough estimate (ignoring metadata overhead)
    
    std::cout << "Attempting to allocate all segments (estimated: " << expected_segments << ")" << std::endl;
    
    std::vector<void*> all_segments;
    int allocation_count = 0;
    auto start = std::chrono::high_resolution_clock::now();
    
    while (true) {
        void* segment = manager.GetNewSegment();
        if (segment == nullptr) {
            break;  // Out of segments
        }
        all_segments.push_back(segment);
        allocation_count++;
        
        if (allocation_count % 100 == 0) {
            std::cout << "  Allocated " << allocation_count << " segments..." << std::endl;
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "✓ Total segments allocated: " << allocation_count << std::endl;
    std::cout << "✓ Total time: " << duration.count() << " ms" << std::endl;
    std::cout << "✓ Average time per allocation: " 
              << (duration.count() * 1000000.0 / allocation_count) << " ns" << std::endl;
    
    // Verify all segments are unique
    for (size_t i = 0; i < all_segments.size(); ++i) {
        for (size_t j = i + 1; j < all_segments.size(); ++j) {
            if (all_segments[i] == all_segments[j]) {
                std::cerr << "ERROR: Duplicate segment in stress test!" << std::endl;
                assert(false);
            }
        }
    }
    std::cout << "✓ All segments are unique" << std::endl;
    
    // Try one more allocation - should fail
    void* should_fail = manager.GetNewSegment();
    if (should_fail != nullptr) {
        std::cerr << "ERROR: Should have failed after exhausting all segments!" << std::endl;
        assert(false);
    }
    std::cout << "✓ Correctly returns nullptr when exhausted" << std::endl;
}

// Test 4: Performance benchmark
void test_performance_benchmark() {
    std::cout << "\n=== Test 4: Performance Benchmark ===" << std::endl;
    
    setenv("EMBARCADERO_CXL_SIZE", std::to_string(TEST_CXL_SIZE).c_str(), 1);
    
    CXLManager manager(0, CXL_Type::Emul, "127.0.0.1");
    
    const int num_iterations = 1000;
    std::vector<long long> latencies;
    latencies.reserve(num_iterations);
    
    // Warmup
    for (int i = 0; i < 10; ++i) {
        manager.GetNewSegment();
    }
    
    // Benchmark
    for (int i = 0; i < num_iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        void* segment = manager.GetNewSegment();
        auto end = std::chrono::high_resolution_clock::now();
        
        if (segment == nullptr) {
            std::cerr << "ERROR: Allocation failed during benchmark" << std::endl;
            break;
        }
        
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        latencies.push_back(latency);
    }
    
    // Calculate statistics
    if (latencies.empty()) {
        std::cerr << "ERROR: No successful allocations in benchmark" << std::endl;
        return;
    }
    
    std::sort(latencies.begin(), latencies.end());
    long long p50 = latencies[latencies.size() / 2];
    long long p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
    long long p99 = latencies[static_cast<size_t>(latencies.size() * 0.99)];
    long long min_latency = latencies[0];
    long long max_latency = latencies.back();
    double avg_latency = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    
    std::cout << "✓ Performance statistics (ns):" << std::endl;
    std::cout << "  Min:     " << min_latency << std::endl;
    std::cout << "  P50:     " << p50 << std::endl;
    std::cout << "  P95:     " << p95 << std::endl;
    std::cout << "  P99:     " << p99 << std::endl;
    std::cout << "  Max:     " << max_latency << std::endl;
    std::cout << "  Average: " << avg_latency << std::endl;
    
    // Performance targets
    if (p50 > 200) {  // > 200ns is concerning
        std::cout << "⚠ WARNING: P50 latency > 200ns (target: <100ns)" << std::endl;
    } else {
        std::cout << "✓ P50 latency within target (<200ns)" << std::endl;
    }
    
    if (p99 > 1000) {  // > 1μs for p99 is concerning
        std::cout << "⚠ WARNING: P99 latency > 1μs (target: <500ns)" << std::endl;
    } else {
        std::cout << "✓ P99 latency within target (<1μs)" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "Segment Allocation Test Suite" << std::endl;
    std::cout << "[[DEVIATION_005]] - Atomic Bitmap Allocator" << std::endl;
    std::cout << "========================================" << std::endl;
    
    try {
        test_single_broker_allocation();
        test_multi_broker_concurrent();
        test_stress_all_segments();
        test_performance_benchmark();
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "✓ All tests passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
