#pragma once

#include <memory>
#include <functional>
#include "../common/common.h"

namespace Embarcadero {

// Forward declarations
namespace Corfu {
    class CorfuReplicationClient;
}
namespace Scalog {
    class ScalogReplicationClient;
}

/**
 * ReplicationManager handles replication logic for CORFU and SCALOG sequencer types
 * 
 * [[PHASE_5_REFACTOR_LEGACY_PATHS]] - Ownership and Usage Clarification:
 * 
 * OWNERSHIP:
 * - Used ONLY by TopicRefactored (refactored pipeline, not mainline)
 * - Mainline Topic class uses DiskManager::ReplicateThread for EMBARCADERO sequencer
 * - These are parallel architectures - not interchangeable
 * 
 * CURRENT STATUS:
 * - TopicRefactored: Uses ReplicationManager (CORFU/SCALOG only)
 * - Topic (mainline): Uses DiskManager::ReplicateThread (EMBARCADERO ORDER=5)
 * 
 * MIGRATION STATUS:
 * - TopicRefactored is experimental/refactored code path
 * - Mainline replication is DiskManager-based (authoritative for ORDER=5)
 * - Do NOT mix ReplicationManager with mainline Topic/DiskManager replication
 * 
 * TODO: Decide on architecture:
 * - Option A: Fully migrate to TopicRefactored + ReplicationManager (remove DiskManager replication)
 * - Option B: Keep DiskManager replication, remove/archive ReplicationManager
 * - Option C: Keep both but clearly document separation and ownership
 */
class ReplicationManager {
public:
    using GetNumBrokersFunc = std::function<int()>;
    
    ReplicationManager(const std::string& topic_name,
                      int broker_id,
                      int replication_factor,
                      SequencerType seq_type,
                      TInode* tinode,
                      TInode* replica_tinode);
    ~ReplicationManager();

    // Initialize replication clients based on sequencer type
    bool Initialize();

    // Replicate data for different sequencer types
    void ReplicateCorfuData(size_t log_idx, size_t total_size, void* data);
    void ReplicateScalogData(size_t log_idx, size_t total_size, size_t num_msg, void* data);
    
    // Update replication done markers
    void UpdateReplicationDone(size_t last_offset, GetNumBrokersFunc get_num_brokers);

    // Get replication clients (for buffer manager callbacks)
    Corfu::CorfuReplicationClient* GetCorfuClient() { return corfu_client_.get(); }
    Scalog::ScalogReplicationClient* GetScalogClient() { return scalog_client_.get(); }

private:
    std::string topic_name_;
    int broker_id_;
    int replication_factor_;
    SequencerType seq_type_;
    TInode* tinode_;
    TInode* replica_tinode_;

    // Replication clients
    std::unique_ptr<Corfu::CorfuReplicationClient> corfu_client_;
    std::unique_ptr<Scalog::ScalogReplicationClient> scalog_client_;
};

} // namespace Embarcadero
