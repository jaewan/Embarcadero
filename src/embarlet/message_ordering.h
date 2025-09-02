#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include "absl/container/btree_set.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "../common/common.h"

namespace Embarcadero {

/**
 * MessageOrdering handles message sequencing and ordering logic
 * Extracted from Topic class to separate ordering concerns
 */
class MessageOrdering {
public:
    using GetRegisteredBrokersFunc = std::function<bool(absl::btree_set<int>&, TInode*)>;
    
    MessageOrdering(void* cxl_addr, TInode* tinode, int broker_id);
    ~MessageOrdering();

    // Start sequencer based on type and order
    void StartSequencer(SequencerType seq_type, int order, const std::string& topic_name);
    
    // Stop all sequencer threads
    void StopSequencer();

    // Get ordered message count
    size_t GetOrderedCount() const { return tinode_->offsets[broker_id_].ordered; }

private:
    // Sequencer implementations
    void Sequencer4();
    void BrokerScannerWorker(int broker_id);
    void StartScalogLocalSequencer(const std::string& topic_name);
    
    // Helper methods for order assignment
    void AssignOrder(BatchHeader* batch_to_order, size_t start_total_order, BatchHeader*& header_for_sub);
    bool ProcessSkipped(absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>>& skipped_batches,
                       BatchHeader*& header_for_sub);

    // Member variables
    void* cxl_addr_;
    TInode* tinode_;
    int broker_id_;
    std::atomic<bool> stop_threads_{false};
    
    // Sequencer thread
    std::thread sequencer_thread_;
    
    // Order tracking
    std::atomic<size_t> global_seq_{0};
    absl::Mutex global_seq_batch_seq_mu_;
    absl::flat_hash_map<size_t, size_t> next_expected_batch_seq_;
    
    // Callbacks
    GetRegisteredBrokersFunc get_registered_brokers_callback_;
};

/**
 * CombinerThread handles message combining logic
 * Runs as a separate component that can be started/stopped
 */
class MessageCombiner {
public:
    MessageCombiner(void* cxl_addr, 
                    void* first_message_addr,
                    TInode* tinode,
                    TInode* replica_tinode,
                    int broker_id);
    ~MessageCombiner();

    // Start/stop combiner thread
    void Start();
    void Stop();

    // Get combined message info
    size_t GetLogicalOffset() const { return logical_offset_; }
    size_t GetWrittenLogicalOffset() const { return written_logical_offset_; }
    void* GetWrittenPhysicalAddr() const { return written_physical_addr_; }

private:
    void CombinerThread();
    void UpdateTInodeWritten(size_t written, size_t written_addr);

    // Member variables
    void* cxl_addr_;
    void* first_message_addr_;
    TInode* tinode_;
    TInode* replica_tinode_;
    int broker_id_;
    
    std::atomic<bool> stop_thread_{false};
    std::thread combiner_thread_;
    
    // Tracking variables
    std::atomic<size_t> logical_offset_{0};
    std::atomic<size_t> written_logical_offset_{static_cast<size_t>(-1)};
    std::atomic<void*> written_physical_addr_{nullptr};
};

} // namespace Embarcadero
