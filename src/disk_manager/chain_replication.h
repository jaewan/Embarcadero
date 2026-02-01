#ifndef EMBARCADERO_CHAIN_REPLICATION_H_
#define EMBARCADERO_CHAIN_REPLICATION_H_

#include <atomic>
#include <thread>
#include <vector>
#include <array>
#include "../cxl_manager/cxl_datastructure.h"

namespace Embarcadero {

/**
 * ChainReplicationManager - Phase 2 chain replication protocol
 *
 * Protocol:
 *   1. Replicas poll GOI for new entries (written by sequencer)
 *   2. Replicas copy data from BLog to local disk IN PARALLEL
 *   3. Replicas serialize ACKs via num_replicated token:
 *      - Replica R_i waits for num_replicated == i
 *      - Replica R_i increments num_replicated = i + 1 (passes token to next)
 *   4. Tail replica (R_f) updates CompletionVector after replication
 *
 * Benefits:
 *   - Parallel data copy (low latency)
 *   - Serialized ACKs (ordered durability confirmation)
 *   - No per-replica counters (single num_replicated field)
 */
class ChainReplicationManager {
public:
    ChainReplicationManager(
        int replica_id,             // My replica ID (0 = head, f = tail)
        int replication_factor,     // Total replicas (f+1)
        void* cxl_addr,             // CXL base address
        GOIEntry* goi,              // GOI pointer
        CompletionVectorEntry* cv,  // CompletionVector pointer
        const std::string& disk_path  // Local disk path
    );

    ~ChainReplicationManager();

    // Start replication thread
    void Start();

    // Stop replication thread
    void Stop();

private:
    // Main replication loop
    void ReplicationThread();

    // Tail replica updates CompletionVector
    void UpdateCompletionVector(uint16_t broker_id, uint64_t pbr_index);

    int replica_id_;
    int replication_factor_;
    void* cxl_addr_;
    GOIEntry* goi_;
    CompletionVectorEntry* cv_;

    int disk_fd_;
    std::string disk_path_;

    std::atomic<uint64_t> next_goi_index_{0};  // Next GOI entry to replicate
    std::atomic<bool> stop_{false};
    std::thread replication_thread_;

    // Tail replica tracks per-broker highest contiguous pbr_index
    std::array<uint64_t, NUM_MAX_BROKERS> broker_cv_state_{};
};

} // namespace Embarcadero

#endif  // EMBARCADERO_CHAIN_REPLICATION_H_
