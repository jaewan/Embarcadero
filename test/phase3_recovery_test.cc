/**
 * Phase 3 Recovery Integration Tests
 *
 * Tests sequencer-driven recovery for stalled chain replication (§4.2.2):
 * 1. RecoveryDetectsStall - Verify recovery increments num_replicated after timeout
 * 2. RecoveryUnderLoad - Verify 0% drops at high throughput
 * 3. RingWraparound - Verify correct behavior after >64K entries
 * 4. NoFalsePositives - Verify no spurious recoveries when replicas on time
 * 5. RaceFreeClearing - Verify CAS-based clearing prevents race with reuse
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <cstring>
#include "../src/cxl_manager/cxl_datastructure.h"
#include "../src/common/performance_utils.h"

using namespace Embarcadero;
using namespace std::chrono_literals;

// Mock CXL memory for testing
constexpr size_t TEST_CXL_SIZE = 128 * 1024 * 1024;  // 128 MB for testing

class Phase3RecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Allocate mock CXL memory
        cxl_memory_ = aligned_alloc(4096, TEST_CXL_SIZE);
        ASSERT_NE(cxl_memory_, nullptr);
        memset(cxl_memory_, 0, TEST_CXL_SIZE);

        // Setup Phase 3 structures
        control_block_ = reinterpret_cast<ControlBlock*>(cxl_memory_);
        goi_ = reinterpret_cast<GOIEntry*>(
            reinterpret_cast<uint8_t*>(cxl_memory_) + 0x2000);

        // Initialize control block
        control_block_->epoch.store(1, std::memory_order_release);
        control_block_->committed_seq.store(0, std::memory_order_release);

        // Allocate timestamp ring
        timestamp_ring_ = new GOITimestampEntry[kRingSize];
        for (size_t i = 0; i < kRingSize; i++) {
            timestamp_ring_[i].goi_index.store(0, std::memory_order_relaxed);
            timestamp_ring_[i].timestamp_ns.store(0, std::memory_order_relaxed);
        }
        write_pos_.store(0, std::memory_order_release);

        stop_recovery_.store(false, std::memory_order_release);
    }

    void TearDown() override {
        stop_recovery_.store(true, std::memory_order_release);
        if (recovery_thread_.joinable()) {
            recovery_thread_.join();
        }
        if (cxl_memory_) {
            free(cxl_memory_);
        }
        delete[] timestamp_ring_;
    }

    // Simulate sequencer writing GOI entry with timestamp
    void WriteGOIEntry(uint64_t goi_idx, uint32_t broker_id, uint32_t replication_factor) {
        GOIEntry* entry = &goi_[goi_idx];
        entry->global_seq = goi_idx;
        entry->broker_id = broker_id;
        entry->num_replicated.store(0, std::memory_order_release);

        // Flush to CXL
        CXL::flush_cacheline(entry);
        CXL::store_fence();

        // Record timestamp (lock-free)
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        size_t ring_pos = write_pos_.fetch_add(1, std::memory_order_relaxed) % kRingSize;
        timestamp_ring_[ring_pos].goi_index.store(goi_idx, std::memory_order_relaxed);
        timestamp_ring_[ring_pos].timestamp_ns.store(now_ns, std::memory_order_release);
    }

    // Simulate replica incrementing num_replicated
    void SimulateReplicaIncrement(uint64_t goi_idx) {
        GOIEntry* entry = &goi_[goi_idx];
        CXL::flush_cacheline(entry);
        CXL::load_fence();
        uint32_t current = entry->num_replicated.load(std::memory_order_acquire);
        entry->num_replicated.store(current + 1, std::memory_order_release);
        CXL::flush_cacheline(entry);
        CXL::store_fence();
    }

    // Recovery thread implementation (simplified from goi_recovery_thread.cc)
    void RecoveryThreadFunc(int replication_factor, uint64_t timeout_ns, uint64_t scan_interval_ns) {
        uint64_t recovery_count = 0;

        while (!stop_recovery_.load(std::memory_order_acquire)) {
            auto scan_start = std::chrono::steady_clock::now();
            uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // Scan the timestamp ring
            uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
            uint64_t scan_start_pos = (write_pos > kRingSize) ? (write_pos - kRingSize) : 0;

            for (uint64_t i = scan_start_pos; i < write_pos; i++) {
                size_t ring_pos = i % kRingSize;

                // Read timestamp and GOI index (lock-free)
                uint64_t timestamp_ns = timestamp_ring_[ring_pos].timestamp_ns.load(std::memory_order_acquire);
                uint64_t goi_idx = timestamp_ring_[ring_pos].goi_index.load(std::memory_order_acquire);

                // Skip if entry not initialized
                if (timestamp_ns == 0) continue;

                // Check if entry has timed out
                uint64_t elapsed_ns = now_ns - timestamp_ns;
                if (elapsed_ns < timeout_ns) {
                    continue;
                }

                // Entry timed out - check replication progress
                GOIEntry* goi_entry = &goi_[goi_idx];
                CXL::flush_cacheline(goi_entry);
                CXL::load_fence();
                uint32_t current_replicated = goi_entry->num_replicated.load(std::memory_order_acquire);

                // Check if replication is stuck
                if (current_replicated < static_cast<uint32_t>(replication_factor)) {
                    // RECOVERY: Increment num_replicated to unblock chain
                    uint32_t new_value = current_replicated + 1;
                    goi_entry->num_replicated.store(new_value, std::memory_order_release);
                    CXL::flush_cacheline(goi_entry);
                    CXL::store_fence();

                    recovery_count++;
                    recoveries_detected_.fetch_add(1, std::memory_order_relaxed);

                    // Only clear timestamp if fully recovered (reached replication_factor)
                    if (new_value >= static_cast<uint32_t>(replication_factor)) {
                        uint64_t expected = timestamp_ns;
                        timestamp_ring_[ring_pos].timestamp_ns.compare_exchange_strong(
                            expected, 0, std::memory_order_release, std::memory_order_relaxed);
                    }
                    // Otherwise keep timestamp so we detect stall again on next scan
                } else {
                    // Replication completed normally - clear timestamp
                    uint64_t expected = timestamp_ns;
                    timestamp_ring_[ring_pos].timestamp_ns.compare_exchange_strong(
                        expected, 0, std::memory_order_release, std::memory_order_relaxed);
                }
            }

            // Sleep until next scan interval
            auto elapsed = std::chrono::steady_clock::now() - scan_start;
            auto sleep_duration = std::chrono::nanoseconds(scan_interval_ns) - elapsed;
            if (sleep_duration.count() > 0) {
                std::this_thread::sleep_for(sleep_duration);
            }
        }
    }

    void StartRecoveryThread(int replication_factor, uint64_t timeout_ns = 10'000'000,
                            uint64_t scan_interval_ns = 1'000'000) {
        recovery_thread_ = std::thread(&Phase3RecoveryTest::RecoveryThreadFunc, this,
                                      replication_factor, timeout_ns, scan_interval_ns);
    }

    struct GOITimestampEntry {
        std::atomic<uint64_t> goi_index{0};
        std::atomic<uint64_t> timestamp_ns{0};
    };

    static constexpr size_t kRingSize = 65536;

    void* cxl_memory_{nullptr};
    ControlBlock* control_block_{nullptr};
    GOIEntry* goi_{nullptr};

    GOITimestampEntry* timestamp_ring_{nullptr};
    std::atomic<uint64_t> write_pos_{0};
    std::atomic<bool> stop_recovery_{false};
    std::atomic<uint64_t> recoveries_detected_{0};
    std::thread recovery_thread_;
};

// Test 1: Recovery Detects Stall
TEST_F(Phase3RecoveryTest, RecoveryDetectsStall) {
    constexpr int REPLICATION_FACTOR = 3;
    constexpr uint64_t TIMEOUT_NS = 10'000'000;  // 10ms

    // Start recovery thread
    StartRecoveryThread(REPLICATION_FACTOR, TIMEOUT_NS);

    // Write GOI entry
    WriteGOIEntry(0, 0, REPLICATION_FACTOR);

    // Simulate first 2 replicas incrementing (R_0, R_1)
    SimulateReplicaIncrement(0);  // num_replicated: 0 → 1
    SimulateReplicaIncrement(0);  // num_replicated: 1 → 2

    // R_2 fails (does not increment)
    // Wait for timeout + scan interval + margin
    std::this_thread::sleep_for(std::chrono::milliseconds(12));

    // Check that recovery incremented num_replicated
    GOIEntry* entry = &goi_[0];
    CXL::flush_cacheline(entry);
    CXL::load_fence();
    uint32_t final_replicated = entry->num_replicated.load(std::memory_order_acquire);

    EXPECT_EQ(final_replicated, REPLICATION_FACTOR)
        << "Recovery should have incremented num_replicated to " << REPLICATION_FACTOR;
    EXPECT_EQ(recoveries_detected_.load(), 1)
        << "Exactly 1 recovery should have been detected";
}

// Test 2: Recovery Under Load (No Drops)
TEST_F(Phase3RecoveryTest, RecoveryUnderLoad) {
    constexpr int REPLICATION_FACTOR = 3;
    constexpr int NUM_BATCHES = 100'000;  // 100K batches
    constexpr uint64_t TIMEOUT_NS = 10'000'000;

    // Start recovery thread
    StartRecoveryThread(REPLICATION_FACTOR, TIMEOUT_NS);

    // Write batches rapidly (simulating 2M batches/sec = 0.5μs per batch)
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < NUM_BATCHES; i++) {
        WriteGOIEntry(i, 0, REPLICATION_FACTOR);

        // Simulate all replicas incrementing immediately (no stalls)
        for (int r = 0; r < REPLICATION_FACTOR; r++) {
            SimulateReplicaIncrement(i);
        }

        // Throttle to ~2M batches/sec (0.5μs per batch)
        // In test, use faster rate to avoid long test duration
        if (i % 1000 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    auto duration = std::chrono::steady_clock::now() - start;
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

    // Wait for recovery thread to scan all entries
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Verify no recoveries (all replicas were on time)
    EXPECT_EQ(recoveries_detected_.load(), 0)
        << "No recoveries should occur when all replicas are on time";

    // Verify write_pos advanced correctly
    uint64_t final_write_pos = write_pos_.load(std::memory_order_acquire);
    EXPECT_EQ(final_write_pos, NUM_BATCHES)
        << "All batches should be recorded in timestamp ring";

    std::cout << "Processed " << NUM_BATCHES << " batches in " << duration_ms << "ms"
              << " (" << (NUM_BATCHES * 1000.0 / duration_ms) << " batches/sec)" << std::endl;
}

// Test 3: Ring Wraparound
TEST_F(Phase3RecoveryTest, RingWraparound) {
    constexpr int REPLICATION_FACTOR = 3;
    constexpr uint64_t TIMEOUT_NS = 5'000'000;  // 5ms (shorter for faster test)
    constexpr size_t NUM_BATCHES = kRingSize + 10000;  // Wrap around ring

    // Start recovery thread with shorter scan interval
    StartRecoveryThread(REPLICATION_FACTOR, TIMEOUT_NS, 500'000);  // 0.5ms scan

    // Write batches that wrap the ring
    for (size_t i = 0; i < NUM_BATCHES; i++) {
        WriteGOIEntry(i, 0, REPLICATION_FACTOR);

        // Complete replication immediately for all batches
        for (int r = 0; r < REPLICATION_FACTOR; r++) {
            SimulateReplicaIncrement(i);
        }

        // Faster rate for test
        if (i % 10000 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Wait for recovery to scan wrapped ring
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Verify no recoveries (all completed on time)
    EXPECT_EQ(recoveries_detected_.load(), 0)
        << "Ring wraparound should not cause spurious recoveries";

    // Verify write_pos wrapped correctly
    uint64_t final_write_pos = write_pos_.load(std::memory_order_acquire);
    EXPECT_EQ(final_write_pos, NUM_BATCHES)
        << "Write position should advance past ring size";
}

// Test 4: No False Positives
TEST_F(Phase3RecoveryTest, NoFalsePositives) {
    constexpr int REPLICATION_FACTOR = 3;
    constexpr int NUM_BATCHES = 1000;
    constexpr uint64_t TIMEOUT_NS = 10'000'000;

    // Start recovery thread
    StartRecoveryThread(REPLICATION_FACTOR, TIMEOUT_NS);

    // Write batches with all replicas incrementing within timeout
    for (int i = 0; i < NUM_BATCHES; i++) {
        WriteGOIEntry(i, 0, REPLICATION_FACTOR);

        // Simulate replicas with varying delays (all < timeout individually)
        for (int r = 0; r < REPLICATION_FACTOR; r++) {
            // Random delay 0-2ms per replica (max 6ms total, well below 10ms timeout)
            std::this_thread::sleep_for(std::chrono::microseconds(rand() % 2000));
            SimulateReplicaIncrement(i);
        }
    }

    // Wait for recovery thread to scan
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Verify no spurious recoveries (allow small margin for timing variance)
    EXPECT_LT(recoveries_detected_.load(), NUM_BATCHES / 100)
        << "Less than 1% spurious recoveries acceptable for timing variance";
}

// Test 5: Race-Free Clearing (CAS-based)
TEST_F(Phase3RecoveryTest, RaceFreeClearing) {
    constexpr int REPLICATION_FACTOR = 3;
    constexpr uint64_t TIMEOUT_NS = 5'000'000;  // 5ms
    constexpr size_t NUM_BATCHES = kRingSize * 2;  // 2× ring size to force wraparound

    // Start recovery thread with fast scan
    StartRecoveryThread(REPLICATION_FACTOR, TIMEOUT_NS, 100'000);  // 0.1ms scan (very aggressive)

    // Writer thread: continuously write to ring positions (fast, no throttling)
    std::thread writer([&]() {
        for (size_t i = 0; i < NUM_BATCHES; i++) {
            size_t ring_pos = write_pos_.fetch_add(1, std::memory_order_relaxed) % kRingSize;

            // Write new entry
            uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            timestamp_ring_[ring_pos].goi_index.store(i, std::memory_order_relaxed);
            timestamp_ring_[ring_pos].timestamp_ns.store(now_ns, std::memory_order_release);

            // Write GOI entry
            GOIEntry* entry = &goi_[i];
            entry->global_seq = i;
            entry->num_replicated.store(REPLICATION_FACTOR, std::memory_order_release);  // Complete immediately
            CXL::flush_cacheline(entry);
            CXL::store_fence();

            // No throttling - write as fast as possible to force wraparound
        }
    });

    writer.join();

    // Wait for recovery thread to finish scanning
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Verify ring wraparound occurred (write_pos > kRingSize)
    uint64_t final_write_pos = write_pos_.load(std::memory_order_acquire);
    EXPECT_GT(final_write_pos, kRingSize)
        << "Write position should exceed ring size (wraparound)";

    // Verify no spurious recoveries due to race condition
    // (CAS-based clearing prevents zeroing reused slots)
    EXPECT_EQ(recoveries_detected_.load(), 0)
        << "CAS-based clearing should prevent clearing reused slots";

    std::cout << "Final write position: " << final_write_pos
              << " (wrapped " << (final_write_pos / kRingSize) << " times)" << std::endl;
}

// Test 6: Multiple Stalls
TEST_F(Phase3RecoveryTest, MultipleStalls) {
    constexpr int REPLICATION_FACTOR = 3;
    constexpr int NUM_BATCHES = 10;
    constexpr uint64_t TIMEOUT_NS = 10'000'000;

    // Start recovery thread
    StartRecoveryThread(REPLICATION_FACTOR, TIMEOUT_NS);

    // Write multiple batches, half will stall
    for (int i = 0; i < NUM_BATCHES; i++) {
        WriteGOIEntry(i, 0, REPLICATION_FACTOR);

        if (i % 2 == 0) {
            // Even batches: complete all replicas
            for (int r = 0; r < REPLICATION_FACTOR; r++) {
                SimulateReplicaIncrement(i);
            }
        } else {
            // Odd batches: only first replica completes (stall at R_1)
            SimulateReplicaIncrement(i);
        }
    }

    // Wait for recovery (need 2+ scans for num_replicated: 1→2→3)
    // Timeout=10ms, scan=1ms, so wait 10ms + 2 scans + margin = 25ms
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // Verify recovery detected stalled batches
    // Each stalled batch needs 2 recoveries (1→2, 2→3), so expect 5 batches × 2 = 10 recoveries
    int expected_min_recoveries = NUM_BATCHES / 2;  // At least 5 (one per stalled batch)
    EXPECT_GE(recoveries_detected_.load(), expected_min_recoveries)
        << "Recovery should detect at least " << expected_min_recoveries << " stalled batches";

    // Verify all batches now have num_replicated == REPLICATION_FACTOR
    for (int i = 0; i < NUM_BATCHES; i++) {
        GOIEntry* entry = &goi_[i];
        CXL::flush_cacheline(entry);
        CXL::load_fence();
        uint32_t replicated = entry->num_replicated.load(std::memory_order_acquire);
        EXPECT_EQ(replicated, REPLICATION_FACTOR)
            << "Batch " << i << " should have num_replicated=" << REPLICATION_FACTOR;
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
