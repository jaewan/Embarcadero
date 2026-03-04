#include "chain_replication.h"
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <deque>
#include <glog/logging.h>
#include "../common/performance_utils.h"
#include "../cxl_manager/cxl_datastructure.h"  // CXL namespace

namespace Embarcadero {
namespace {
constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);

struct PendingTokenUpdate {
    uint64_t goi_index{0};
    uint16_t source_broker{0};
    uint16_t owner_broker{0};
    uint64_t pbr_index{0};
    uint64_t cumulative_msg_count{0};
    uint16_t role{0};
    bool bypass_counted{false};
    bool cv_updated{false};
};

inline void RefreshGOIEntry(GOIEntry* entry) {
    CXL::flush_cacheline(entry);
    CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(entry) + 64);
    CXL::load_fence();
}
}

ChainReplicationManager::ChainReplicationManager(
    int replica_id,
    int replication_factor,
    int local_broker_id,
    int num_brokers,
    void* cxl_addr,
    GOIEntry* goi,
    CompletionVectorEntry* cv,
    const std::string& disk_path)
    : replica_id_(replica_id)
    , replication_factor_(replication_factor)
    , local_broker_id_(local_broker_id)
    , num_brokers_(num_brokers)
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
              << " local_broker_id=" << local_broker_id
              << " num_brokers=" << num_brokers
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
    if (cxl_addr_ == nullptr || goi_ == nullptr || cv_ == nullptr) {
        LOG(ERROR) << "ChainReplicationManager: invalid pointers, not starting replication thread";
        return;
    }
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
    uint64_t entries_skipped_not_member = 0;
    uint64_t entries_token_bypassed = 0;
    uint64_t entries_invalid_source = 0;
    uint64_t entries_suspicious_payload = 0;
    size_t disk_offset = 0;
    std::deque<PendingTokenUpdate> pending;
    constexpr size_t kMaxPendingTokenUpdates = 4096;

    while (!stop_.load(std::memory_order_acquire)) {
        bool made_progress = false;

        // Stage 1: ingest GOI entries and copy payloads without waiting on token progression.
        while (pending.size() < kMaxPendingTokenUpdates) {
            const uint64_t goi_index = next_goi_index_.load(std::memory_order_relaxed);
            GOIEntry* entry = &goi_[goi_index];
            RefreshGOIEntry(entry);

            // global_seq is written last by sequencer; mismatch means entry is not ready yet.
            if (entry->global_seq != goi_index) {
                break;
            }

            const int source_broker = static_cast<int>(entry->broker_id);
            const size_t payload_size = entry->payload_size;
            const uint32_t message_count = entry->message_count;
            if (source_broker < 0 || source_broker >= num_brokers_ ||
                payload_size == 0 || message_count == 0) {
                entries_suspicious_payload++;
                if (entries_suspicious_payload <= 8 || (entries_suspicious_payload % 1024) == 0) {
                    LOG(WARNING) << "ChainReplicationManager: GOI entry not ready or invalid metadata"
                                 << " goi_index=" << goi_index
                                 << " source_broker=" << source_broker
                                 << " num_brokers=" << num_brokers_
                                 << " payload_size=" << payload_size
                                 << " message_count=" << message_count
                                 << " global_seq=" << entry->global_seq;
                }
                break;
            }

            int my_role = -1;
            for (int i = 0; i < replication_factor_; ++i) {
                const int replica_broker = Embarcadero::GetReplicationSetBroker(
                    source_broker, replication_factor_, num_brokers_, i);
                if (replica_broker == local_broker_id_) {
                    my_role = i;
                    break;
                }
            }

            if (my_role < 0) {
                entries_skipped_not_member++;
                next_goi_index_.fetch_add(1, std::memory_order_relaxed);
                made_progress = true;
                continue;
            }

            void* payload = reinterpret_cast<uint8_t*>(cxl_addr_) + entry->blog_offset;
            const size_t write_size = (payload_size + 4095) & ~4095;
            const ssize_t written = pwrite(disk_fd_, payload, write_size, disk_offset);
            if (written != static_cast<ssize_t>(write_size)) {
                LOG(ERROR) << "ChainReplicationManager: pwrite failed, wrote " << written
                           << " expected " << write_size << " errno=" << errno
                           << " goi_index=" << goi_index;
                break;  // Retry this same GOI index on next loop.
            }

            PendingTokenUpdate pending_update;
            pending_update.goi_index = goi_index;
            pending_update.source_broker = static_cast<uint16_t>(source_broker);
            pending_update.owner_broker = entry->broker_id;
            pending_update.pbr_index = entry->pbr_index;
            pending_update.cumulative_msg_count = entry->cumulative_message_count;
            pending_update.role = static_cast<uint16_t>(my_role);
            pending.push_back(pending_update);

            disk_offset += write_size;
            next_goi_index_.fetch_add(1, std::memory_order_relaxed);
            batches_replicated++;
            made_progress = true;
        }

        // Stage 2: token progression and ACK2 frontier update.
        for (auto it = pending.begin(); it != pending.end();) {
            GOIEntry* entry = &goi_[it->goi_index];
            RefreshGOIEntry(entry);

            uint16_t token = static_cast<uint16_t>(entry->num_replicated.load(std::memory_order_acquire));

            if (token == it->role) {
                entry->num_replicated.store(static_cast<uint16_t>(it->role + 1), std::memory_order_release);
                CXL::flush_cacheline(entry);
                CXL::store_fence();
                token = static_cast<uint16_t>(it->role + 1);
                made_progress = true;
            } else if (token > it->role && !it->bypass_counted) {
                entries_token_bypassed++;
                it->bypass_counted = true;
            }

            if (it->role == static_cast<uint16_t>(replication_factor_ - 1) &&
                token >= static_cast<uint16_t>(replication_factor_) &&
                !it->cv_updated) {
                UpdateCompletionVector(it->owner_broker, it->pbr_index, it->cumulative_msg_count);
                it->cv_updated = true;
                made_progress = true;
            }

            const bool token_stage_done = token >= static_cast<uint16_t>(it->role + 1);
            const bool tail_cv_done = (it->role != static_cast<uint16_t>(replication_factor_ - 1)) || it->cv_updated;
            if (token_stage_done && tail_cv_done) {
                it = pending.erase(it);
                continue;
            }

            ++it;
        }

        // Periodic logging
        if (batches_replicated % 1000 == 0) {
            LOG(INFO) << "ChainReplicationManager: Replica " << replica_id_
                      << " replicated " << batches_replicated
                      << " batches"
                      << " pending=" << pending.size()
                      << " skipped_not_member=" << entries_skipped_not_member
                      << " token_bypassed=" << entries_token_bypassed
                      << " invalid_source=" << entries_invalid_source
                      << " suspicious_payload=" << entries_suspicious_payload;
        }

        if (!made_progress) {
            if (!pending.empty()) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            } else {
                std::this_thread::yield();
            }
        }
    }

    LOG(INFO) << "ChainReplicationManager: Replication thread exiting, replicated "
              << batches_replicated << " batches"
              << " pending_final=" << pending.size();
}

void ChainReplicationManager::UpdateCompletionVector(uint16_t broker_id, uint64_t pbr_index, uint64_t cumulative_msg_count) {
    // Tail replica owns ACK2 durability frontier.
    // Use max semantics directly on both fields:
    // - completed_pbr_head: export/recovery visibility
    // - completed_logical_offset: durable ACK2 frontier (message count)
    uint64_t cur = cv_[broker_id].completed_pbr_head.load(std::memory_order_acquire);
    while (cur == kNoProgress || pbr_index > cur) {
        if (cv_[broker_id].completed_pbr_head.compare_exchange_strong(
                cur, pbr_index, std::memory_order_release)) {
            break;
        }
    }

    uint64_t cur_logical = cv_[broker_id].completed_logical_offset.load(std::memory_order_acquire);
    while (cumulative_msg_count > cur_logical) {
        if (cv_[broker_id].completed_logical_offset.compare_exchange_strong(
                cur_logical, cumulative_msg_count, std::memory_order_release)) {
            break;
        }
    }

    CXL::flush_cacheline(&cv_[broker_id]);
    CXL::store_fence();
}

} // namespace Embarcadero
