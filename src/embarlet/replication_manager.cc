#include "replication_manager.h"
#include "../client/corfu_client.h"
#include "../client/scalog_client.h"
#include "../common/performance_utils.h"
#include <glog/logging.h>

namespace Embarcadero {

ReplicationManager::ReplicationManager(const std::string& topic_name,
                                     int broker_id,
                                     int replication_factor,
                                     SequencerType seq_type,
                                     TInode* tinode,
                                     TInode* replica_tinode)
    : topic_name_(topic_name),
      broker_id_(broker_id),
      replication_factor_(replication_factor),
      seq_type_(seq_type),
      tinode_(tinode),
      replica_tinode_(replica_tinode) {}

ReplicationManager::~ReplicationManager() = default;

bool ReplicationManager::Initialize() {
    if (replication_factor_ <= 0) {
        return true; // No replication needed
    }

    switch (seq_type_) {
        case CORFU:
            corfu_client_ = std::make_unique<Corfu::CorfuReplicationClient>(
                topic_name_,
                replication_factor_,
                "127.0.0.1:" + std::to_string(CORFU_REP_PORT)
            );
            
            if (!corfu_client_->Connect()) {
                LOG(ERROR) << "Corfu replication client failed to connect to replica";
                return false;
            }
            break;

        case SCALOG:
            scalog_client_ = std::make_unique<Scalog::ScalogReplicationClient>(
                topic_name_,
                replication_factor_,
                "localhost",
                broker_id_
            );
            
            if (!scalog_client_->Connect()) {
                LOG(ERROR) << "Scalog replication client failed to connect to replica";
                return false;
            }
            break;

        default:
            // Other sequencer types don't use replication clients
            break;
    }

    return true;
}

void ReplicationManager::ReplicateCorfuData(size_t log_idx, size_t total_size, void* data) {
    if (corfu_client_ && replication_factor_ > 0) {
        corfu_client_->ReplicateData(log_idx, total_size, data);
    }
}

void ReplicationManager::ReplicateScalogData(size_t log_idx, size_t total_size, size_t num_msg, void* data) {
    if (scalog_client_ && replication_factor_ > 0) {
        scalog_client_->ReplicateData(log_idx, total_size, num_msg, data);
    }
}

void ReplicationManager::UpdateReplicationDone(size_t last_offset, GetNumBrokersFunc get_num_brokers) {
    if (replication_factor_ <= 0) {
        return;
    }

    // [[PHASE_3_ALIGN_REPLICATION_SET]] - Use canonical replication set computation
    // Note: This is used by TopicRefactored (CORFU/SCALOG), not mainline ORDER=5
    // Mainline ORDER=5 uses DiskManager::ReplicateThread which has its own replication_done updates
    int num_brokers = get_num_brokers();
    for (int i = 0; i < replication_factor_; i++) {
        int b = GetReplicationSetBroker(broker_id_, replication_factor_, num_brokers, i);
        if (tinode_->replicate_tinode && replica_tinode_) {
            replica_tinode_->offsets[b].replication_done[broker_id_] = last_offset;
        }
        tinode_->offsets[b].replication_done[broker_id_] = last_offset;
    }
}

} // namespace Embarcadero
