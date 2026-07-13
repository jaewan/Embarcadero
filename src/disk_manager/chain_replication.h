#ifndef EMBARCADERO_CHAIN_REPLICATION_H_
#define EMBARCADERO_CHAIN_REPLICATION_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "cxl_manager/cxl_datastructure.h"

namespace Embarcadero {

/**
 * Sink backend for ORDER=5 chain replication.
 * disk-durable: pwrite + fdatasync (media_durable ACK2 claim)
 * memory-copy: CXL→DRAM memcpy into bounded ring (replicated_ack_emulated)
 * memory-accounting: byte accounting only (replicated_ack_emulated)
 */
enum class ChainReplicationSinkMode {
    DiskDurable = 0,
    MemoryCopy = 1,
    MemoryAccounting = 2,
};

struct ChainReplicationConfig {
    ChainReplicationSinkMode sink_mode{ChainReplicationSinkMode::DiskDurable};
    size_t inmem_bytes_per_source{256UL * 1024UL * 1024UL};
    size_t sync_bytes{64UL * 1024UL * 1024UL};
    uint64_t sync_interval_ns{250ULL * 1000ULL * 1000ULL};

    bool IsMemorySink() const {
        return sink_mode == ChainReplicationSinkMode::MemoryCopy ||
               sink_mode == ChainReplicationSinkMode::MemoryAccounting;
    }
    bool IsMediaDurable() const {
        return sink_mode == ChainReplicationSinkMode::DiskDurable;
    }
};

// Parse env flags. Backward compatible with:
//   EMBARCADERO_CHAIN_REPLICATION_INMEM / INMEM_COPY / INMEM_BYTES_PER_SOURCE
// Optional override:
//   EMBARCADERO_CHAIN_REPLICATION_SINK=disk-durable|memory-copy|memory-accounting
// Sync thresholds:
//   EMBARCADERO_CHAIN_SYNC_BYTES / EMBARCADERO_CHAIN_SYNC_INTERVAL_MS
//   (defaults: 256 MiB / 250 ms)
// Disk striping:
//   EMBARCADERO_REPLICA_DISK_DIRS + optional EMBARCADERO_REPLICA_DISK_WEIGHTS
ChainReplicationConfig ParseChainReplicationConfig();

const char* ChainReplicationSinkModeName(ChainReplicationSinkMode mode);

// ACK2 claim label for publication metadata.
// memory modes must never be labeled media_durable.
const char* ChainReplicationAckClaimLabel(ChainReplicationSinkMode mode);

/**
 * ChainReplicationManager - Phase 2 chain replication protocol
 *
 * Pipeline (per active source):
 *   Dispatcher (GOI order) → SourcePipeline.sink → [sync] → token → CV (tail only)
 *
 * Protocol:
 *   1. Replicas poll GOI for new entries (written by sequencer)
 *   2. Replicas copy data from BLog in parallel (copy-ahead; no Stage-1 token gate)
 *   3. After local sink(+sync) completion, serialize ACKs via num_replicated token
 *   4. Tail replica updates CompletionVector (ack_level=2)
 */
class ChainReplicationManager {
public:
    ChainReplicationManager(
        int replica_id,
        int replication_factor,
        int local_broker_id,
        int num_brokers,
        void* cxl_addr,
        GOIEntry* goi,
        CompletionVectorEntry* cv,
        const std::string& disk_path
    );

    ~ChainReplicationManager();

    void Start();
    void Stop();

    const ChainReplicationConfig& config() const { return config_; }

private:
    void ReplicationThread();
    void UpdateCompletionVector(uint16_t broker_id, uint64_t pbr_index,
                                uint64_t cumulative_msg_count);

    int replica_id_;
    int replication_factor_;
    int local_broker_id_;
    int num_brokers_;
    void* cxl_addr_;
    GOIEntry* goi_;
    CompletionVectorEntry* cv_;

    std::string disk_path_;
    ChainReplicationConfig config_;
    std::vector<std::string> disk_dirs_;

    std::atomic<uint64_t> next_goi_index_{0};
    std::atomic<bool> stop_{false};
    std::thread replication_thread_;
};

}  // namespace Embarcadero

#endif  // EMBARCADERO_CHAIN_REPLICATION_H_
