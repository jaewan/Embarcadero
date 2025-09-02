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
 * ReplicationManager handles all replication logic for different sequencer types
 * Extracted from Topic class to separate replication concerns
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
