#include "chain_replication.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <glog/logging.h>
#include "../common/performance_utils.h"
#include "../cxl_manager/cxl_datastructure.h"  // CXL namespace

namespace Embarcadero {

ChainReplicationManager::ChainReplicationManager(
    int replica_id,
    int replication_factor,
    void* cxl_addr,
    GOIEntry* goi,
    CompletionVectorEntry* cv,
    const std::string& disk_path)
    : replica_id_(replica_id)
    , replication_factor_(replication_factor)
    , cxl_addr_(cxl_addr)
    , goi_(goi)
    , cv_(cv)
    , disk_path_(disk_path) {

    // Open disk file for replication
    disk_fd_ = open(disk_path.c_str(), O_CREAT | O_RDWR | O_DIRECT, 0644);
    if (disk_fd_ < 0) {
        LOG(FATAL) << "Failed to open replication disk: " << disk_path
                   << " error: " << strerror(errno);
    }

    LOG(INFO) << "ChainReplicationManager: replica_id=" << replica_id
              << " replication_factor=" << replication_factor
              << " disk=" << disk_path;
}

ChainReplicationManager::~ChainReplicationManager() {
    Stop();
    if (disk_fd_ >= 0) {
        close(disk_fd_);
    }
}

void ChainReplicationManager::Start() {
    stop_.store(false, std::memory_order_release);
    replication_thread_ = std::thread(&ChainReplicationManager::ReplicationThread, this);
    LOG(INFO) << "ChainReplicationManager: Started replication thread";
}

void ChainReplicationManager::Stop() {
    stop_.store(true, std::memory_order_release);
    if (replication_thread_.joinable()) {
        replication_thread_.join();
    }
    LOG(INFO) << "ChainReplicationManager: Stopped replication thread";
}

void ChainReplicationManager::ReplicationThread() {
    LOG(INFO) << "ChainReplicationManager: Replication thread started (replica_id="
              << replica_id_ << ")";

    uint64_t batches_replicated = 0;
    size_t disk_offset = 0;

    while (!stop_.load(std::memory_order_acquire)) {
        uint64_t goi_index = next_goi_index_.load(std::memory_order_relaxed);
        GOIEntry* entry = &goi_[goi_index];

        // [[CRITICAL_FIX: Invalidate cache before reading GOI entry]]
        // Sequencer (head broker) writes GOI; replication thread must invalidate to see updates.
        // Without this, replication thread may never see entry->global_seq and spin forever.
        CXL::flush_cacheline(entry);
        CXL::load_fence();

        // Check if sequencer has written this entry (global_seq == goi_index means valid)
        if (entry->global_seq != goi_index) {
            // Entry not yet written by sequencer, yield
            std::this_thread::yield();
            continue;
        }

        // [[PHASE_2_CHAIN_PROTOCOL]] Step 1: Copy data in PARALLEL (all replicas do this)
        // Calculate BLog payload address
        void* payload = reinterpret_cast<uint8_t*>(cxl_addr_) + entry->blog_offset;
        size_t payload_size = entry->payload_size;

        // Write to local disk
        ssize_t written = pwrite(disk_fd_, payload, payload_size, disk_offset);
        if (written != static_cast<ssize_t>(payload_size)) {
            LOG(ERROR) << "ChainReplicationManager: pwrite failed, wrote " << written
                       << " expected " << payload_size << " errno=" << errno;
            continue;  // Retry same entry
        }

        // Ensure data is durable (fsync for reliability, can batch later for performance)
        fsync(disk_fd_);

        VLOG(3) << "ChainReplicationManager: Replica " << replica_id_
                << " copied GOI[" << goi_index << "] broker=" << entry->broker_id
                << " size=" << payload_size << " to disk_offset=" << disk_offset;

        disk_offset += payload_size;

        // [[PHASE_2_CHAIN_PROTOCOL]] Step 2: Token-passing via num_replicated
        // Wait for my turn (previous replica must increment first)
        while (entry->num_replicated.load(std::memory_order_acquire) != static_cast<uint16_t>(replica_id_)) {
            if (stop_.load(std::memory_order_acquire)) {
                return;  // Exit if stopping
            }
            std::this_thread::yield();
        }

        // My turn - increment token to pass to next replica
        entry->num_replicated.store(static_cast<uint16_t>(replica_id_ + 1), std::memory_order_release);
        CXL::flush_cacheline(entry);
        CXL::store_fence();

        VLOG(3) << "ChainReplicationManager: Replica " << replica_id_
                << " incremented num_replicated to " << (replica_id_ + 1)
                << " for GOI[" << goi_index << "]";

        // [[PHASE_2_CV_UPDATER]] Tail replica updates CompletionVector
        if (replica_id_ == replication_factor_ - 1) {  // I'm the tail
            UpdateCompletionVector(entry->broker_id, entry->pbr_index);
        }

        // Advance to next GOI entry
        next_goi_index_.fetch_add(1, std::memory_order_relaxed);
        batches_replicated++;

        // Periodic logging
        if (batches_replicated % 1000 == 0) {
            LOG(INFO) << "ChainReplicationManager: Replica " << replica_id_
                      << " replicated " << batches_replicated << " batches";
        }
    }

    LOG(INFO) << "ChainReplicationManager: Replication thread exiting, replicated "
              << batches_replicated << " batches";
}

void ChainReplicationManager::UpdateCompletionVector(uint16_t broker_id, uint32_t pbr_index) {
    // [[PHASE_2_FIX]] Tail replica tracks highest contiguous pbr_index per broker
    // CV[broker_id] is updated only when pbr_index is contiguous (monotonic, no gaps)
    // pbr_index is now absolute (never wraps) so contiguity tracking is simple

    uint64_t& cv_state = broker_cv_state_[broker_id];

    // [[PHASE_2_FIX]] Handle initial case (cv_state starts at 0, first batch is pbr_index=0)
    if (cv_state == 0 && pbr_index == 0) {
        // First batch for this broker
        cv_[broker_id].completed_pbr_head.store(pbr_index, std::memory_order_release);
        CXL::flush_cacheline(&cv_[broker_id]);
        CXL::store_fence();
        cv_state = pbr_index;
        VLOG(3) << "CV_UPDATER: Broker " << broker_id << " first batch, CV=" << pbr_index;
        return;
    }

    // Check if this batch is the next expected (contiguous)
    if (pbr_index == cv_state + 1) {
        // Update CV (monotonic)
        cv_[broker_id].completed_pbr_head.store(pbr_index, std::memory_order_release);
        CXL::flush_cacheline(&cv_[broker_id]);
        CXL::store_fence();

        // Update local tracker
        cv_state = pbr_index;

        VLOG(3) << "CV_UPDATER: Broker " << broker_id << " CV updated to pbr_index=" << pbr_index;
    } else if (pbr_index <= cv_state) {
        // Already completed (duplicate or reordered), ignore
        VLOG(4) << "CV_UPDATER: Broker " << broker_id << " pbr_index=" << pbr_index
                << " already completed (cv_state=" << cv_state << ")";
    } else {
        // Gap detected (pbr_index > cv_state + 1)
        // This can happen if sequencer processes batches out of pbr_index order
        // [[PHASE_2_FIX]] With absolute indices, gaps are true ordering issues (not ring wraparound)
        VLOG(4) << "CV_UPDATER: Broker " << broker_id << " gap detected, pbr_index=" << pbr_index
                << " cv_state=" << cv_state << " (waiting for contiguous)";

        // TODO: Track out-of-order entries and update CV when gap fills
        // For now, CV will lag until contiguous batches arrive
    }
}

} // namespace Embarcadero
