#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include "absl/container/btree_set.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "../cxl_manager/cxl_datastructure.h"

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
    // Inject registered brokers callback for microbench harness
    void SetGetRegisteredBrokersCallback(GetRegisteredBrokersFunc func) { get_registered_brokers_callback_ = std::move(func); }

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
    struct ClientState {
        absl::Mutex mu;
        size_t expected_seq = 0;
    };
    absl::Mutex client_states_mu_;
    absl::flat_hash_map<size_t, std::unique_ptr<ClientState>> client_states_;
    ClientState* GetOrCreateClientState(size_t client_id) {
        absl::MutexLock g(&client_states_mu_);
        auto it = client_states_.find(client_id);
        if (it != client_states_.end()) return it->second.get();
        auto st = std::make_unique<ClientState>();
        ClientState* ptr = st.get();
        client_states_.emplace(client_id, std::move(st));
        return ptr;
    }
    
    // Callbacks
    GetRegisteredBrokersFunc get_registered_brokers_callback_;

#ifdef BUILDING_ORDER_BENCH
public:
    struct SequencerThreadStats {
        // Counters
        uint64_t num_batches_seen = 0;
        uint64_t num_batches_ordered = 0;
        uint64_t num_batches_skipped = 0;
        uint64_t num_duplicates = 0;
        uint64_t atomic_fetch_add_count = 0;
        uint64_t atomic_claimed_msgs = 0;

        // Timings (nanoseconds)
        uint64_t lock_acquire_time_total_ns = 0;
        uint64_t time_in_assign_order_total_ns = 0;
        uint64_t time_waiting_on_complete_total_ns = 0;

        // Per-batch ordering latency samples (ns)
        std::vector<uint64_t> batch_order_latency_ns;
    };

    // Configure bench behavior
    void SetBenchFlushMetadata(bool enabled) { bench_flush_metadata_ = enabled; }
    void SetBenchPinSequencerCpus(const std::vector<int>& cpus) { bench_seq_cpus_ = cpus; }
    void SetBenchHeadersOnly(bool enabled) { bench_headers_only_ = enabled; }

    // Snapshot per-broker stats (copy out)
    void BenchGetStatsSnapshot(std::vector<std::pair<int, SequencerThreadStats>>& out_stats) {
        absl::MutexLock l(&bench_stats_mu_);
        out_stats.clear();
        for (auto& kv : bench_stats_by_broker_) {
            out_stats.emplace_back(kv.first, kv.second);
        }
    }

    void SetBenchBatchHeaderRing(int broker_id, BatchHeader* start, size_t num) {
        absl::MutexLock l(&bench_ring_mu_);
        bench_batch_header_rings_[broker_id] = {start, num};
    }
    void SetBenchExportHeaderRing(int broker_id, BatchHeader* start, size_t num) {
        absl::MutexLock l(&bench_ring_mu_);
        bench_export_header_rings_[broker_id] = {start, num};
    }
private:
    struct BenchRing { BatchHeader* start; size_t num; };
    absl::Mutex bench_ring_mu_;
    absl::flat_hash_map<int, BenchRing> bench_batch_header_rings_;
    absl::flat_hash_map<int, BenchRing> bench_export_header_rings_;

    // Bench instrumentation
    bool bench_flush_metadata_ = false;
    bool bench_headers_only_ = false;
    std::vector<int> bench_seq_cpus_;
    absl::Mutex bench_stats_mu_;
    absl::flat_hash_map<int, SequencerThreadStats> bench_stats_by_broker_;
#endif
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
