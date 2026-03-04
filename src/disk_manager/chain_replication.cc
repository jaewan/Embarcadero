#include "chain_replication.h"
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <array>
#include <deque>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <condition_variable>
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

inline void RefreshGOIToken(GOIEntry* entry) {
    // num_replicated is in the first cache line of GOIEntry; avoid touching line 2 in token hot path.
    CXL::flush_cacheline(entry);
    CXL::load_fence();
}

std::vector<std::string> SplitCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

inline uint64_t SteadyNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
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
    source_disk_fds_.assign(static_cast<size_t>(std::max(0, num_brokers_)), -1);
    source_disk_paths_.assign(static_cast<size_t>(std::max(0, num_brokers_)), std::string());
    source_disk_offsets_.assign(static_cast<size_t>(std::max(0, num_brokers_)), 0);

    if (const char* dirs_env = std::getenv("EMBARCADERO_REPLICA_DISK_DIRS")) {
        disk_dirs_ = SplitCsv(dirs_env);
    }
    if (disk_dirs_.empty()) {
        if (const char* root_env = std::getenv("EMBARCADERO_REPLICA_DISK_ROOT")) {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(root_env, ec)) {
                if (ec) break;
                if (e.is_directory()) {
                    const std::string n = e.path().filename().string();
                    if (n.rfind("disk", 0) == 0) {
                        disk_dirs_.push_back(e.path().string());
                    }
                }
            }
        }
    }
    if (disk_dirs_.empty()) {
        // Best-effort auto-discovery for local dev/bench scripts.
        const std::array<std::string, 3> defaults = {
            "../../.Replication",
            "../.Replication",
            ".Replication"
        };
        for (const auto& root : defaults) {
            std::error_code ec;
            if (!std::filesystem::exists(root, ec) || ec) continue;
            for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
                if (ec) break;
                if (e.is_directory()) {
                    const std::string n = e.path().filename().string();
                    if (n.rfind("disk", 0) == 0) {
                        disk_dirs_.push_back(e.path().string());
                    }
                }
            }
            if (!disk_dirs_.empty()) break;
        }
    }
    std::sort(disk_dirs_.begin(), disk_dirs_.end());
    disk_dirs_.erase(std::unique(disk_dirs_.begin(), disk_dirs_.end()), disk_dirs_.end());
    // Keep only writable directories (some mounts may be root-owned in dev envs).
    std::vector<std::string> writable_dirs;
    writable_dirs.reserve(disk_dirs_.size());
    for (const auto& d : disk_dirs_) {
        if (d.empty()) continue;
        if (::access(d.c_str(), W_OK | X_OK) == 0) {
            writable_dirs.push_back(d);
        } else {
            LOG(WARNING) << "ChainReplicationManager: skipping non-writable replication dir: " << d;
        }
    }
    disk_dirs_.swap(writable_dirs);
    if (disk_dirs_.empty()) {
        // Fallback to directory of explicit disk_path.
        disk_dirs_.push_back(std::filesystem::path(disk_path_).parent_path().string());
    }

    for (int src = 0; src < num_brokers_; ++src) {
        const std::string& dir = disk_dirs_[static_cast<size_t>(src) % disk_dirs_.size()];
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string file_path = dir + "/replica_b" + std::to_string(local_broker_id_) +
                                      "_src" + std::to_string(src) + ".dat";
        int fd = open(file_path.c_str(), O_CREAT | O_RDWR | O_DIRECT, 0644);
        if (fd < 0) {
            // Fallback to /tmp so replication remains available even if configured directory
            // permissions are restrictive on this host.
            const std::string fallback_path = "/tmp/replica_b" + std::to_string(local_broker_id_) +
                                              "_src" + std::to_string(src) + ".dat";
            fd = open(fallback_path.c_str(), O_CREAT | O_RDWR | O_DIRECT, 0644);
            if (fd < 0) {
                LOG(FATAL) << "Failed to open replication disk file: " << file_path
                           << " and fallback: " << fallback_path
                           << " error: " << strerror(errno);
            }
            LOG(WARNING) << "ChainReplicationManager: using fallback replication file " << fallback_path
                         << " (primary failed: " << file_path << ")";
            source_disk_paths_[static_cast<size_t>(src)] = fallback_path;
        } else {
            source_disk_paths_[static_cast<size_t>(src)] = file_path;
        }
        source_disk_fds_[static_cast<size_t>(src)] = fd;
    }

    LOG(INFO) << "ChainReplicationManager: replica_id=" << replica_id
              << " local_broker_id=" << local_broker_id
              << " num_brokers=" << num_brokers
              << " replication_factor=" << replication_factor
              << " disk_path=" << disk_path
              << " disk_dirs=" << disk_dirs_.size();
    for (int src = 0; src < num_brokers_; ++src) {
        LOG(INFO) << "ChainReplicationManager: source " << src
                  << " -> " << source_disk_paths_[static_cast<size_t>(src)];
    }
}

ChainReplicationManager::~ChainReplicationManager() {
    Stop();
    for (int fd : source_disk_fds_) {
        if (fd >= 0) {
            close(fd);
        }
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
    uint64_t last_progress_log_batches = 0;
    uint64_t token_checks = 0;
    uint64_t token_waits = 0;
    uint64_t token_progressions = 0;
    uint64_t token_loop_no_progress = 0;
    size_t pending_peak = 0;
    std::array<std::deque<PendingTokenUpdate>, NUM_MAX_BROKERS> pending_by_source;
    std::array<uint64_t, NUM_MAX_BROKERS> token_poll_not_before_ns{};
    std::array<uint64_t, NUM_MAX_BROKERS> token_wait_backoff_ns{};
    std::array<std::deque<PendingTokenUpdate>, NUM_MAX_BROKERS> write_queues;
    std::array<std::mutex, NUM_MAX_BROKERS> write_mu;
    std::array<std::condition_variable, NUM_MAX_BROKERS> write_cv;
    std::mutex completed_mu;
    std::deque<PendingTokenUpdate> completed_writes;
    std::atomic<size_t> queued_writes{0};
    std::atomic<bool> write_error{false};
    std::vector<std::thread> write_workers;
    // Bound speculative copy-ahead so token/CV progression stays close to data ingest.
    // Large pending windows create heavy token re-poll churn with little throughput benefit.
    // Keep a deeper in-flight window so data copy can stay ahead while token ownership catches up.
    // A shallow window throttles role>0 replicas into token-wait stalls under ORDER=5 ACK2 load.
    constexpr size_t kMaxPendingTokenUpdates = 8192;
    constexpr uint64_t kMinWaitPollBackoffNs = 1'000;    // 1us
    constexpr uint64_t kMaxWaitPollBackoffNs = 64'000;   // 64us
    ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);

    auto pending_total = [&pending_by_source]() -> size_t {
        size_t n = 0;
        for (const auto& q : pending_by_source) n += q.size();
        return n;
    };

    auto pending_and_inflight = [&]() -> size_t {
        return pending_total() + queued_writes.load(std::memory_order_acquire);
    };
    token_wait_backoff_ns.fill(kMinWaitPollBackoffNs);

    // Parallel data-copy workers (one queue per source broker) while token/CV stays serialized here.
    for (int src = 0; src < num_brokers_; ++src) {
        write_workers.emplace_back([&, src]() {
            while (true) {
                PendingTokenUpdate task;
                {
                    std::unique_lock<std::mutex> lk(write_mu[src]);
                    write_cv[src].wait(lk, [&] {
                        return stop_.load(std::memory_order_acquire) || !write_queues[src].empty();
                    });
                    if (write_queues[src].empty()) {
                        if (stop_.load(std::memory_order_acquire)) {
                            break;
                        }
                        continue;
                    }
                    task = std::move(write_queues[src].front());
                    write_queues[src].pop_front();
                }

                GOIEntry* entry = &goi_[task.goi_index];
                const size_t payload_size = static_cast<size_t>(entry->payload_size);
                const size_t write_size = (payload_size + 4095) & ~4095;
                const size_t src_idx = static_cast<size_t>(src);
                const int write_fd = source_disk_fds_[src_idx];
                const size_t write_off = source_disk_offsets_[src_idx];
                source_disk_offsets_[src_idx] += write_size;
                void* payload = reinterpret_cast<uint8_t*>(cxl_addr_) + entry->blog_offset;

                ssize_t written = -1;
                while (true) {
                    written = pwrite(write_fd, payload, write_size, write_off);
                    if (written == static_cast<ssize_t>(write_size)) {
                        break;
                    }
                    if (written < 0 && (errno == EINTR || errno == EAGAIN)) {
                        continue;
                    }
                    LOG(ERROR) << "ChainReplicationManager: pwrite failed, wrote " << written
                               << " expected " << write_size << " errno=" << errno
                               << " goi_index=" << task.goi_index
                               << " source_broker=" << src;
                    write_error.store(true, std::memory_order_release);
                    stop_.store(true, std::memory_order_release);
                    break;
                }

                if (!write_error.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lk(completed_mu);
                    completed_writes.push_back(std::move(task));
                }
                queued_writes.fetch_sub(1, std::memory_order_release);
            }
        });
    }

    while (!stop_.load(std::memory_order_acquire)) {
        bool made_progress = false;

        // Stage 1: ingest GOI entries and copy payloads without waiting on token progression.
        while (pending_and_inflight() < kMaxPendingTokenUpdates) {
            const uint64_t goi_index = next_goi_index_.load(std::memory_order_relaxed);

            // Only consume GOI entries that are in the sequencer-committed contiguous prefix.
            // This avoids misclassifying zero-initialized/stale slots (e.g., GOI[0].global_seq==0)
            // as published entries before the current run commits them.
            CXL::flush_cacheline(control_block);
            CXL::load_fence();
            const uint64_t committed_seq = control_block->committed_seq.load(std::memory_order_acquire);
            if (committed_seq == UINT64_MAX || goi_index > committed_seq) {
                break;
            }
            const uint16_t current_epoch = static_cast<uint16_t>(
                control_block->epoch.load(std::memory_order_acquire) & 0xFFFFu);

            GOIEntry* entry = &goi_[goi_index];
            RefreshGOIEntry(entry);

            // global_seq is written last by sequencer; mismatch means entry is not ready yet.
            if (entry->global_seq != goi_index) {
                // A committed index with mismatched publication marker can be either:
                // 1) stale carry-over from a prior epoch (safe to skip), or
                // 2) transient visibility race (must retry, not skip).
                if (entry->epoch_sequenced != current_epoch) {
                    entries_invalid_source++;
                    if (entries_invalid_source <= 8 || (entries_invalid_source % 1024) == 0) {
                        LOG(WARNING) << "ChainReplicationManager: skipping stale GOI entry with global_seq mismatch"
                                     << " goi_index=" << goi_index
                                     << " committed_seq=" << committed_seq
                                     << " observed_global_seq=" << entry->global_seq
                                     << " entry_epoch=" << entry->epoch_sequenced
                                     << " current_epoch=" << current_epoch;
                    }
                    next_goi_index_.fetch_add(1, std::memory_order_relaxed);
                    made_progress = true;
                }
                break;
            }

            const int source_broker = static_cast<int>(entry->broker_id);
            const size_t payload_size = entry->payload_size;
            const uint32_t message_count = entry->message_count;
            if (source_broker < 0 || source_broker >= num_brokers_ ||
                payload_size == 0 || message_count == 0) {
                if (entry->epoch_sequenced != current_epoch) {
                    entries_suspicious_payload++;
                    if (entries_suspicious_payload <= 8 || (entries_suspicious_payload % 1024) == 0) {
                        LOG(WARNING) << "ChainReplicationManager: skipping stale GOI entry with invalid metadata"
                                     << " goi_index=" << goi_index
                                     << " source_broker=" << source_broker
                                     << " num_brokers=" << num_brokers_
                                     << " payload_size=" << payload_size
                                     << " message_count=" << message_count
                                     << " global_seq=" << entry->global_seq
                                     << " committed_seq=" << committed_seq
                                     << " entry_epoch=" << entry->epoch_sequenced
                                     << " current_epoch=" << current_epoch;
                    }
                    next_goi_index_.fetch_add(1, std::memory_order_relaxed);
                    made_progress = true;
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

            const size_t src_idx = static_cast<size_t>(source_broker);
            if (src_idx >= source_disk_fds_.size() || source_disk_fds_[src_idx] < 0) {
                LOG(ERROR) << "ChainReplicationManager: invalid source disk mapping for source_broker="
                           << source_broker;
                break;
            }

            PendingTokenUpdate pending_update;
            pending_update.goi_index = goi_index;
            pending_update.source_broker = static_cast<uint16_t>(source_broker);
            pending_update.owner_broker = entry->broker_id;
            pending_update.pbr_index = entry->pbr_index;
            pending_update.cumulative_msg_count = entry->cumulative_message_count;
            pending_update.role = static_cast<uint16_t>(my_role);

            {
                std::lock_guard<std::mutex> lk(write_mu[source_broker]);
                write_queues[source_broker].push_back(std::move(pending_update));
            }
            write_cv[source_broker].notify_one();
            queued_writes.fetch_add(1, std::memory_order_release);
            next_goi_index_.fetch_add(1, std::memory_order_relaxed);
            made_progress = true;
        }

        // Stage 2a: move completed writes into token-serialization stage.
        while (true) {
            PendingTokenUpdate completed;
            {
                std::lock_guard<std::mutex> lk(completed_mu);
                if (completed_writes.empty()) break;
                completed = std::move(completed_writes.front());
                completed_writes.pop_front();
            }
            batches_replicated++;
            made_progress = true;

            GOIEntry* entry = &goi_[completed.goi_index];
            if (completed.role == 0) {
                // Role-0 owns first token step.
                RefreshGOIToken(entry);
                token_checks++;
                uint16_t token = static_cast<uint16_t>(entry->num_replicated.load(std::memory_order_acquire));
                if (token == 0) {
                    entry->num_replicated.store(static_cast<uint16_t>(1), std::memory_order_release);
                    CXL::flush_cacheline(entry);
                    CXL::store_fence();
                    token = 1;
                    token_progressions++;
                } else if (token > 0) {
                    entries_token_bypassed++;
                }
                if (replication_factor_ == 1 &&
                    token >= static_cast<uint16_t>(replication_factor_)) {
                    UpdateCompletionVector(completed.owner_broker,
                                           completed.pbr_index,
                                           completed.cumulative_msg_count);
                }
            } else {
                // Roles >0 must wait for predecessor token; track by source broker and poll queue head.
                const int owner = static_cast<int>(completed.owner_broker);
                if (owner < 0 || owner >= NUM_MAX_BROKERS) {
                    entries_invalid_source++;
                    continue;
                }
                pending_by_source[owner].push_back(std::move(completed));
                const size_t cur_pending = pending_total();
                if (cur_pending > pending_peak) {
                    pending_peak = cur_pending;
                }
            }
        }

        // Stage 2b: token progression and ACK2 frontier update.
        // Poll only queue heads per source; this avoids O(total_pending) repoll churn under wait.
        const uint64_t now_ns = SteadyNowNs();
        for (int src = 0; src < NUM_MAX_BROKERS; ++src) {
            if (token_poll_not_before_ns[src] != 0 && now_ns < token_poll_not_before_ns[src]) {
                continue;
            }
            auto& q = pending_by_source[src];
            while (!q.empty()) {
                PendingTokenUpdate& h = q.front();
                GOIEntry* entry = &goi_[h.goi_index];
                RefreshGOIToken(entry);
                token_checks++;

                uint16_t token = static_cast<uint16_t>(entry->num_replicated.load(std::memory_order_acquire));
                if (token < h.role) {
                    token_waits++;
                    const uint64_t backoff = token_wait_backoff_ns[src];
                    token_poll_not_before_ns[src] = now_ns + backoff;
                    token_wait_backoff_ns[src] = std::min(backoff << 1, kMaxWaitPollBackoffNs);
                    break;  // Head not ready yet; later entries for same source are unlikely to be ready.
                }
                token_poll_not_before_ns[src] = 0;
                token_wait_backoff_ns[src] = kMinWaitPollBackoffNs;
                if (token == h.role) {
                    entry->num_replicated.store(static_cast<uint16_t>(h.role + 1), std::memory_order_release);
                    CXL::flush_cacheline(entry);
                    CXL::store_fence();
                    token = static_cast<uint16_t>(h.role + 1);
                    token_progressions++;
                    made_progress = true;
                } else if (!h.bypass_counted) {
                    entries_token_bypassed++;
                    h.bypass_counted = true;
                }

                if (h.role == static_cast<uint16_t>(replication_factor_ - 1) &&
                    token >= static_cast<uint16_t>(replication_factor_) &&
                    !h.cv_updated) {
                    UpdateCompletionVector(h.owner_broker, h.pbr_index, h.cumulative_msg_count);
                    h.cv_updated = true;
                    made_progress = true;
                }

                const bool token_stage_done = token >= static_cast<uint16_t>(h.role + 1);
                const bool tail_cv_done =
                    (h.role != static_cast<uint16_t>(replication_factor_ - 1)) || h.cv_updated;
                if (token_stage_done && tail_cv_done) {
                    q.pop_front();
                    continue;
                }
                break;
            }
        }

        // Periodic logging
        if (batches_replicated > 0 &&
            (batches_replicated - last_progress_log_batches) >= 1000) {
            LOG(INFO) << "ChainReplicationManager: Replica " << replica_id_
                      << " replicated " << batches_replicated
                      << " batches"
                      << " pending=" << pending_total()
                      << " pending_peak=" << pending_peak
                      << " skipped_not_member=" << entries_skipped_not_member
                      << " token_bypassed=" << entries_token_bypassed
                      << " token_checks=" << token_checks
                      << " token_waits=" << token_waits
                      << " token_progressions=" << token_progressions
                      << " token_idle_loops=" << token_loop_no_progress
                      << " invalid_source=" << entries_invalid_source
                      << " suspicious_payload=" << entries_suspicious_payload;
            last_progress_log_batches = batches_replicated;
        }

        if (!made_progress) {
            if (pending_total() > 0 || queued_writes.load(std::memory_order_acquire) > 0) {
                token_loop_no_progress++;
            }
            if (pending_total() > 0 || queued_writes.load(std::memory_order_acquire) > 0) {
                // Pending work exists but token ownership is not ready yet; short sleep avoids
                // burning CPU without adding tens-of-microseconds ACK2 jitter.
                std::this_thread::sleep_for(std::chrono::microseconds(2));
            } else {
                std::this_thread::yield();
            }
        }
    }

    stop_.store(true, std::memory_order_release);
    for (int src = 0; src < num_brokers_; ++src) {
        write_cv[src].notify_all();
    }
    for (auto& t : write_workers) {
        if (t.joinable()) t.join();
    }

    LOG(INFO) << "ChainReplicationManager: Replication thread exiting, replicated "
              << batches_replicated << " batches"
              << " pending_final=" << pending_total();
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
