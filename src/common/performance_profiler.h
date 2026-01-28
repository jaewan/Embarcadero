#pragma once

#include "performance_utils.h"
#include <atomic>
#include <fstream>
#include <string>
#include <cstdint>

namespace Embarcadero {
namespace Profiler {

// Performance measurement point structure
struct MeasurementPoint {
    uint64_t t1_publish_start;      // Stage 1: Publish() entry
    uint64_t t2a_buffer_write;      // Stage 2a: Buffer::Write() complete
    uint64_t t2b_batch_sealed;      // Stage 2b: Batch sealed (Buffer::Seal())
    uint64_t t2c_network_sent;      // Stage 2c: All data sent to network
    uint64_t t3a_header_received;   // Stage 3a: Batch header received
    uint64_t t3b_data_received;     // Stage 3b: All message data received
    uint64_t t3c_batch_complete;    // Stage 3c: batch_complete=1 set + flushed
    uint64_t t4a_ordering_start;    // Stage 4a: Sequencer detected batch
    uint64_t t4b_ordering_done;     // Stage 4b: total_order assigned
    uint64_t t5a_ack_check_start;   // Stage 5a: AckThread eligibility check
    uint64_t t5b_ack_cache_inval;   // Stage 5b: Cache invalidation + read ordered
    uint64_t t5c_ack_eligible;      // Stage 5c: ACK condition met
    uint64_t t5d_ack_sent;          // Stage 5d: ACK sent to network
    uint64_t t6a_ack_received;      // Stage 6a: Publisher received ACK
    uint64_t t6b_poll_complete;     // Stage 6b: Poll() returned (end-to-end)

    size_t batch_seq;               // Batch sequence number for correlation
    size_t message_count;           // Number of messages in batch
    int broker_id;                  // Broker processing this batch
    size_t client_id;               // Publisher client ID
};

// Thread-local measurement buffer (1M entries = ~64MB per thread)
constexpr size_t MEASUREMENT_BUFFER_SIZE = 1024 * 1024;
extern thread_local MeasurementPoint measurements[MEASUREMENT_BUFFER_SIZE];
extern thread_local size_t measurement_idx;
extern thread_local size_t measurement_wrap_count;

// Global enable flag (set via env var EMBARCADERO_ENABLE_PROFILING=1)
extern std::atomic<bool> g_profiling_enabled;

// Initialize profiling (call once at program start)
inline void InitProfiling() {
    const char* env = std::getenv("EMBARCADERO_ENABLE_PROFILING");
    if (env && std::atoi(env) == 1) {
        g_profiling_enabled.store(true, std::memory_order_relaxed);
    }
}

// Check if profiling is enabled (inline for minimal overhead)
inline bool IsProfilingEnabled() {
    return g_profiling_enabled.load(std::memory_order_relaxed);
}

// Get current measurement point for writing
inline MeasurementPoint& GetCurrentMeasurement() {
    return measurements[measurement_idx % MEASUREMENT_BUFFER_SIZE];
}

// Advance to next measurement (call after completing a batch)
inline void NextMeasurement() {
    ++measurement_idx;
    if (measurement_idx % MEASUREMENT_BUFFER_SIZE == 0) {
        ++measurement_wrap_count;
    }
}

// Flush measurements to CSV file
inline void FlushMeasurements(const std::string& filename) {
    if (!IsProfilingEnabled()) return;

    std::ofstream out(filename, std::ios::app);
    if (!out) {
        LOG(ERROR) << "Failed to open profiling output: " << filename;
        return;
    }

    // Write CSV header if file is empty
    out.seekp(0, std::ios::end);
    if (out.tellp() == 0) {
        out << "batch_seq,message_count,broker_id,client_id,"
            << "t1_publish_start,t2a_buffer_write,t2b_batch_sealed,t2c_network_sent,"
            << "t3a_header_received,t3b_data_received,t3c_batch_complete,"
            << "t4a_ordering_start,t4b_ordering_done,"
            << "t5a_ack_check_start,t5b_ack_cache_inval,t5c_ack_eligible,t5d_ack_sent,"
            << "t6a_ack_received,t6b_poll_complete\n";
    }

    // Write measurements (only up to current index, handle wrapping)
    size_t count = (measurement_wrap_count > 0) ? MEASUREMENT_BUFFER_SIZE : measurement_idx;
    for (size_t i = 0; i < count; ++i) {
        const auto& m = measurements[i];
        if (m.batch_seq == 0) continue; // Skip uninitialized entries

        out << m.batch_seq << ","
            << m.message_count << ","
            << m.broker_id << ","
            << m.client_id << ","
            << m.t1_publish_start << ","
            << m.t2a_buffer_write << ","
            << m.t2b_batch_sealed << ","
            << m.t2c_network_sent << ","
            << m.t3a_header_received << ","
            << m.t3b_data_received << ","
            << m.t3c_batch_complete << ","
            << m.t4a_ordering_start << ","
            << m.t4b_ordering_done << ","
            << m.t5a_ack_check_start << ","
            << m.t5b_ack_cache_inval << ","
            << m.t5c_ack_eligible << ","
            << m.t5d_ack_sent << ","
            << m.t6a_ack_received << ","
            << m.t6b_poll_complete << "\n";
    }

    out.close();
    LOG(INFO) << "Flushed " << count << " measurements to " << filename;
}

} // namespace Profiler
} // namespace Embarcadero

// Convenience macros for minimal-overhead instrumentation
#define PROFILE_TIMESTAMP(field) \
    if (Embarcadero::Profiler::IsProfilingEnabled()) { \
        Embarcadero::Profiler::GetCurrentMeasurement().field = Embarcadero::CXL::rdtsc(); \
    }

#define PROFILE_SET_METADATA(batch_seq_val, msg_count, broker, client) \
    if (Embarcadero::Profiler::IsProfilingEnabled()) { \
        auto& m = Embarcadero::Profiler::GetCurrentMeasurement(); \
        m.batch_seq = batch_seq_val; \
        m.message_count = msg_count; \
        m.broker_id = broker; \
        m.client_id = client; \
    }

#define PROFILE_NEXT() \
    if (Embarcadero::Profiler::IsProfilingEnabled()) { \
        Embarcadero::Profiler::NextMeasurement(); \
    }
