#include "chain_replication.h"
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <thread>
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
#include "../common/env_flags.h"
#include "../cxl_manager/cxl_datastructure.h"  // CXL namespace

namespace Embarcadero {
namespace {
constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
constexpr size_t kDefaultInMemSinkBytesPerSource = 256UL * 1024UL * 1024UL;  // 256 MiB

struct PendingTokenUpdate {
    uint64_t goi_index{0};
    uint16_t source_broker{0};
    uint16_t owner_broker{0};
    uint64_t pbr_index{0};
    uint64_t cumulative_msg_count{0};
    uint16_t role{0};
    bool cv_updated{false};
};

inline void RefreshGOIEntry(GOIEntry* entry) {
    CXL::flush_cacheline(entry);
    CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(entry) + 64);
    // GOI lives in non-coherent CXL memory. CLFLUSHOPT visibility must be ordered with a
    // full fence before subsequent metadata loads, otherwise readers can observe stale
    // publication/token state even after the producer flushed it.
    CXL::full_fence();
}

inline void RefreshGOIToken(GOIEntry* entry) {
    // num_replicated is in the first cache line of GOIEntry; avoid touching line 2 in token hot path.
    CXL::flush_cacheline(entry);
    // Token handoff correctness relies on seeing the producer's flushed num_replicated update.
    // A load fence is insufficient for CLFLUSHOPT ordering on non-coherent CXL.
    CXL::full_fence();
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

inline bool ShouldUseInMemorySink() {
    static const bool enabled = ReadEnvBoolStrict("EMBARCADERO_CHAIN_REPLICATION_INMEM", false);
    return enabled;
}

inline size_t InMemorySinkBytesPerSource() {
    static const size_t bytes = []() {
        const char* env = std::getenv("EMBARCADERO_CHAIN_REPLICATION_INMEM_BYTES_PER_SOURCE");
        if (!env || !env[0]) return kDefaultInMemSinkBytesPerSource;
        char* end = nullptr;
        unsigned long long v = std::strtoull(env, &end, 10);
        if (end == env || v < 4096ULL) return kDefaultInMemSinkBytesPerSource;
        return static_cast<size_t>(v);
    }();
    return bytes;
}

inline bool ShouldCopyInMemorySinkPayload() {
    static const bool enabled = ReadEnvBoolStrict("EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY", false);
    return enabled;
}

inline bool ShouldEnableOrder5Trace() {
    static const bool enabled = ReadEnvBoolStrict("EMBARCADERO_ORDER5_TRACE", false);
    return enabled;
}

inline void CopyIntoRing(std::vector<uint8_t>& ring, size_t& write_pos, const void* src, size_t len) {
    if (ring.empty() || len == 0) return;
    const size_t cap = ring.size();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
    if (len >= cap) {
        // Keep the latest 'cap' bytes if payload is larger than ring capacity.
        std::memcpy(ring.data(), p + (len - cap), cap);
        write_pos = 0;
        return;
    }
    const size_t first = std::min(len, cap - write_pos);
    std::memcpy(ring.data() + write_pos, p, first);
    if (first < len) {
        std::memcpy(ring.data(), p + first, len - first);
    }
    write_pos = (write_pos + len) % cap;
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
    const bool in_memory_sink = ShouldUseInMemorySink();

    const char* dirs_env = std::getenv("EMBARCADERO_REPLICA_DISK_DIRS");
    if (!in_memory_sink && dirs_env) {
        disk_dirs_ = SplitCsv(dirs_env);
    }
    if (!in_memory_sink && disk_dirs_.empty()) {
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
    if (!in_memory_sink && disk_dirs_.empty()) {
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
    if (!in_memory_sink) {
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
    }

    for (int src = 0; src < num_brokers_; ++src) {
        if (in_memory_sink) {
            source_disk_paths_[static_cast<size_t>(src)] = "[in-memory-sink]";
            continue;
        }
        const std::string& dir = disk_dirs_[static_cast<size_t>(src) % disk_dirs_.size()];
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string file_path = dir + "/replica_b" + std::to_string(local_broker_id_) +
                                      "_src" + std::to_string(src) + ".dat";
        int fd = open(file_path.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd < 0) {
            // Fallback to /tmp so replication remains available even if configured directory
            // permissions are restrictive on this host.
            const std::string fallback_path = "/tmp/replica_b" + std::to_string(local_broker_id_) +
                                              "_src" + std::to_string(src) + ".dat";
            fd = open(fallback_path.c_str(), O_CREAT | O_RDWR, 0644);
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

    if (in_memory_sink) {
        LOG(WARNING) << "ChainReplicationManager: benchmark in-memory sink enabled via "
                     << "EMBARCADERO_CHAIN_REPLICATION_INMEM=1; ACK2 no longer implies disk durability.";
    }

    LOG(INFO) << "ChainReplicationManager: replica_id=" << replica_id
              << " local_broker_id=" << local_broker_id
              << " num_brokers=" << num_brokers
              << " replication_factor=" << replication_factor
              << " disk_path=" << disk_path
              << " disk_dirs=" << disk_dirs_.size()
              << " in_memory_sink=" << in_memory_sink;
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
    uint64_t entries_invalid_source = 0;
    uint64_t entries_suspicious_payload = 0;
    uint64_t last_progress_log_batches = 0;
    std::array<std::deque<PendingTokenUpdate>, NUM_MAX_BROKERS> pending_by_source;
    std::array<uint64_t, NUM_MAX_BROKERS> token_poll_not_before_ns{};
    std::array<uint64_t, NUM_MAX_BROKERS> token_wait_backoff_ns{};
    std::array<uint64_t, NUM_MAX_BROKERS> token_stall_log_deadline_ns{};
    int token_scan_start_src = 0;
    std::vector<int8_t> source_role_map;
    std::array<std::deque<PendingTokenUpdate>, NUM_MAX_BROKERS> write_queues;
    std::array<std::mutex, NUM_MAX_BROKERS> write_mu;
    std::array<std::condition_variable, NUM_MAX_BROKERS> write_cv;
    std::mutex completed_mu;
    std::deque<PendingTokenUpdate> completed_writes;
    std::atomic<size_t> queued_writes{0};
    std::atomic<bool> write_error{false};
    std::atomic<bool> shutdown_workers{false};
    std::vector<std::thread> write_workers;
    const bool in_memory_sink = ShouldUseInMemorySink();
    const bool in_memory_copy = in_memory_sink && ShouldCopyInMemorySinkPayload();
    std::vector<std::vector<uint8_t>> in_memory_buffers;
    std::vector<size_t> in_memory_write_pos;
    if (in_memory_sink) {
        in_memory_write_pos.assign(static_cast<size_t>(std::max(0, num_brokers_)), 0);
    }
    if (in_memory_copy) {
        const size_t sink_bytes = InMemorySinkBytesPerSource();
        in_memory_buffers.resize(static_cast<size_t>(std::max(0, num_brokers_)));
        for (int src = 0; src < num_brokers_; ++src) {
            in_memory_buffers[static_cast<size_t>(src)].resize(sink_bytes);
        }
    }
    // Bound speculative copy-ahead so token/CV progression stays close to data ingest.
    // Large pending windows create heavy token re-poll churn with little throughput benefit.
    // Keep a deeper in-flight window so data copy can stay ahead while token ownership catches up.
    // A shallow window throttles role>0 replicas into token-wait stalls under ORDER=5 ACK2 load.
    constexpr size_t kMaxPendingTokenUpdates = 8192;
    constexpr uint64_t kMinWaitPollBackoffNs = 1'000;    // 1us
    constexpr uint64_t kMaxWaitPollBackoffNs = 64'000;   // 64us
    constexpr uint64_t kControlRefreshIntervalEntries = 256;
    ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
    uint64_t cached_committed_seq = UINT64_MAX;
    uint16_t cached_epoch = 0;
    uint64_t last_control_refresh_goi = 0;

    auto pending_total = [&pending_by_source]() -> size_t {
        size_t n = 0;
        for (const auto& q : pending_by_source) n += q.size();
        return n;
    };

    auto pending_and_inflight = [&]() -> size_t {
        return pending_total() + queued_writes.load(std::memory_order_acquire);
    };
    token_wait_backoff_ns.fill(kMinWaitPollBackoffNs);
    source_role_map.assign(static_cast<size_t>(std::max(0, num_brokers_)), static_cast<int8_t>(-1));
    for (int src = 0; src < num_brokers_; ++src) {
        const int role = Embarcadero::GetReplicationChainRole(
            local_broker_id_, src, replication_factor_, num_brokers_);
        const int8_t role8 = (role < 0) ? static_cast<int8_t>(-1) : static_cast<int8_t>(role);
        source_role_map[static_cast<size_t>(src)] = role8;
        LOG(INFO) << "ChainReplicationManager: local_broker_id=" << local_broker_id_
                  << " source=" << src << " role=" << static_cast<int>(role8);
    }
    auto refresh_control_state = [&](uint64_t goi_index) {
        CXL::flush_cacheline(control_block);
        // ControlBlock lives in non-coherent CXL just like GOI metadata. A load fence does not
        // order CLFLUSHOPT, so replicas can strand on a stale committed_seq/epoch snapshot and
        // stop ingesting the final committed GOI tail.
        CXL::full_fence();
        cached_committed_seq = control_block->committed_seq.load(std::memory_order_acquire);
        cached_epoch = static_cast<uint16_t>(
            control_block->epoch.load(std::memory_order_acquire) & 0xFFFFu);
        last_control_refresh_goi = goi_index;
    };

    // Parallel data-copy workers (one queue per source broker) while token/CV stays serialized here.
    for (int src = 0; src < num_brokers_; ++src) {
        write_workers.emplace_back([&, src]() {
            while (true) {
                PendingTokenUpdate task;
                {
                    std::unique_lock<std::mutex> lk(write_mu[src]);
                    write_cv[src].wait(lk, [&] {
                        return shutdown_workers.load(std::memory_order_acquire) ||
                               !write_queues[src].empty();
                    });
                    if (write_queues[src].empty()) {
                        if (shutdown_workers.load(std::memory_order_acquire)) {
                            break;
                        }
                        continue;
                    }
                    task = std::move(write_queues[src].front());
                    write_queues[src].pop_front();
                }

                GOIEntry* entry = &goi_[task.goi_index];
                const size_t payload_size = static_cast<size_t>(entry->payload_size);
                const size_t src_idx = static_cast<size_t>(src);
                void* payload = reinterpret_cast<uint8_t*>(cxl_addr_) + entry->blog_offset;
                if (in_memory_sink) {
                    if (in_memory_copy) {
                        CopyIntoRing(in_memory_buffers[src_idx], in_memory_write_pos[src_idx], payload, payload_size);
                    } else {
                        // Benchmark-only fast sink path: account bytes without full payload copy.
                        in_memory_write_pos[src_idx] += payload_size;
                    }
                    std::lock_guard<std::mutex> lk(completed_mu);
                    completed_writes.push_back(std::move(task));
                } else {
                    const int write_fd = source_disk_fds_[src_idx];
                    const size_t write_off = source_disk_offsets_[src_idx];
                    source_disk_offsets_[src_idx] += payload_size;
                    ssize_t written = -1;
                    while (true) {
                        written = pwrite(write_fd, payload, payload_size, write_off);
                        if (written == static_cast<ssize_t>(payload_size)) {
                            break;
                        }
                        if (written < 0 && (errno == EINTR || errno == EAGAIN)) {
                            continue;
                        }
                        LOG(ERROR) << "ChainReplicationManager: pwrite failed, wrote " << written
                                   << " expected " << payload_size << " errno=" << errno
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
                }
                queued_writes.fetch_sub(1, std::memory_order_release);
            }
        });
    }

    auto completed_writes_pending = [&]() -> bool {
        std::lock_guard<std::mutex> lk(completed_mu);
        return !completed_writes.empty();
    };

    while (true) {
        const bool stop_requested = stop_.load(std::memory_order_acquire);
        if (stop_requested) {
            const uint64_t goi_index = next_goi_index_.load(std::memory_order_relaxed);
            refresh_control_state(goi_index);
            const bool more_committed_goi =
                (cached_committed_seq != UINT64_MAX && goi_index <= cached_committed_seq);
            if (!more_committed_goi &&
                pending_total() == 0 &&
                queued_writes.load(std::memory_order_acquire) == 0 &&
                !completed_writes_pending()) {
                break;
            }
        }

        bool made_progress = false;

        // Stage 1: ingest GOI entries and copy payloads without waiting on token progression.
        bool stage1_blocked_on_chain_token = false;
        while (pending_and_inflight() < kMaxPendingTokenUpdates) {
            const uint64_t goi_index = next_goi_index_.load(std::memory_order_relaxed);

            if (cached_committed_seq == UINT64_MAX ||
                goi_index > cached_committed_seq ||
                goi_index - last_control_refresh_goi >= kControlRefreshIntervalEntries) {
                refresh_control_state(goi_index);
            }
            // Only consume GOI entries that are in the sequencer-committed contiguous prefix.
            if (cached_committed_seq == UINT64_MAX || goi_index > cached_committed_seq) {
                break;
            }
            const uint16_t current_epoch = cached_epoch;

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
                                     << " committed_seq=" << cached_committed_seq
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
                                     << " committed_seq=" << cached_committed_seq
                                     << " entry_epoch=" << entry->epoch_sequenced
                                     << " current_epoch=" << current_epoch;
                    }
                    next_goi_index_.fetch_add(1, std::memory_order_relaxed);
                    made_progress = true;
                }
                break;
            }

            const int my_role = (source_broker >= 0 &&
                                 source_broker < static_cast<int>(source_role_map.size()))
                                    ? static_cast<int>(source_role_map[static_cast<size_t>(source_broker)])
                                    : -1;

            if (my_role < 0) {
                next_goi_index_.fetch_add(1, std::memory_order_relaxed);
                made_progress = true;
                continue;
            }

            // Role r>0 runs on a different process than role 0. Without this gate, the tail can finish
            // pwrite and enter token/CV stages while the head has not yet published num_replicated >= r
            // in CXL, yielding token=0 stalls and stuck ACK2 frontiers (ORDER=5 RF=2 latency runs).
            if (my_role > 0) {
                RefreshGOIToken(entry);
                const uint32_t tok = entry->num_replicated.load(std::memory_order_acquire);
                if (tok < static_cast<uint32_t>(my_role)) {
                    stage1_blocked_on_chain_token = true;
                    break;
                }
            }

            stage1_blocked_on_chain_token = false;

            const size_t src_idx = static_cast<size_t>(source_broker);
            if (!in_memory_sink &&
                (src_idx >= source_disk_fds_.size() || source_disk_fds_[src_idx] < 0)) {
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
        if (stage1_blocked_on_chain_token) {
            std::this_thread::yield();
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
            if (ShouldEnableOrder5Trace() && completed.cumulative_msg_count >= 1'040'000) {
                LOG(INFO) << "[ORDER5_TRACE_CR_COMPLETED]"
                          << " local_broker=" << local_broker_id_
                          << " source=" << completed.source_broker
                          << " owner=" << completed.owner_broker
                          << " goi=" << completed.goi_index
                          << " role=" << completed.role
                          << " pbr=" << completed.pbr_index
                          << " cumulative=" << completed.cumulative_msg_count;
            }

            GOIEntry* entry = &goi_[completed.goi_index];
            if (completed.role == 0) {
                // Role-0 owns first token step (CAS: single winner publishes 0→1 for CXL visibility).
                RefreshGOIToken(entry);
                uint32_t token = entry->num_replicated.load(std::memory_order_acquire);
                if (token == 0) {
                    uint32_t expected = 0;
                    constexpr uint32_t kAfterHead = 1;
                    if (entry->num_replicated.compare_exchange_strong(
                            expected, kAfterHead, std::memory_order_release, std::memory_order_acquire)) {
                        CXL::flush_cacheline(entry);
                        CXL::store_fence();
                        token = 1;
                        if (ShouldEnableOrder5Trace() && completed.cumulative_msg_count >= 1'040'000) {
                            LOG(INFO) << "[ORDER5_TRACE_CR_TOKEN_SET]"
                                      << " local_broker=" << local_broker_id_
                                      << " source=" << completed.source_broker
                                      << " owner=" << completed.owner_broker
                                      << " goi=" << completed.goi_index
                                      << " role=" << completed.role
                                      << " token=" << token
                                      << " pbr=" << completed.pbr_index
                                      << " cumulative=" << completed.cumulative_msg_count;
                        }
                    } else {
                        token = expected;
                    }
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
            }
        }

        // Stage 2b: token progression and ACK2 frontier update.
        // Poll only queue heads per source; this avoids O(total_pending) repoll churn under wait.
        const uint64_t now_ns = SteadyNowNs();
        constexpr int kMaxPerSourceTokenOpsPerPass = 8;
        for (int i = 0; i < num_brokers_; ++i) {
            const int src = (token_scan_start_src + i) % num_brokers_;
            if (token_poll_not_before_ns[src] != 0 && now_ns < token_poll_not_before_ns[src]) {
                continue;
            }
            auto& q = pending_by_source[src];
            int ops_this_src = 0;
            while (!q.empty()) {
                PendingTokenUpdate& h = q.front();
                GOIEntry* entry = &goi_[h.goi_index];
                RefreshGOIToken(entry);

                uint32_t token = entry->num_replicated.load(std::memory_order_acquire);
                if (token < h.role) {
                    if (ShouldEnableOrder5Trace()) {
                        const uint64_t now_trace_ns = SteadyNowNs();
                        if (now_trace_ns >= token_stall_log_deadline_ns[src]) {
                            LOG(INFO) << "[ORDER5_TRACE_CR_TOKEN_WAIT]"
                                      << " local_broker=" << local_broker_id_
                                      << " source=" << src
                                      << " owner=" << h.owner_broker
                                      << " goi=" << h.goi_index
                                      << " role=" << h.role
                                      << " token=" << token
                                      << " pbr=" << h.pbr_index
                                      << " cumulative=" << h.cumulative_msg_count
                                      << " pending_src=" << q.size()
                                      << " pending_total=" << pending_total();
                            token_stall_log_deadline_ns[src] = now_trace_ns + 250'000'000ULL;
                        }
                    }
                    const uint64_t backoff = token_wait_backoff_ns[src];
                    token_poll_not_before_ns[src] = now_ns + backoff;
                    token_wait_backoff_ns[src] = std::min(backoff << 1, kMaxWaitPollBackoffNs);
                    break;  // Head not ready yet; later entries for same source are unlikely to be ready.
                }
                token_poll_not_before_ns[src] = 0;
                token_wait_backoff_ns[src] = kMinWaitPollBackoffNs;
                if (token == h.role) {
                    uint32_t expected = static_cast<uint32_t>(h.role);
                    const uint32_t desired = static_cast<uint32_t>(h.role) + 1u;
                    if (entry->num_replicated.compare_exchange_strong(
                            expected, desired, std::memory_order_release, std::memory_order_acquire)) {
                        CXL::flush_cacheline(entry);
                        CXL::store_fence();
                        token = desired;
                        made_progress = true;
                        if (ShouldEnableOrder5Trace() && h.cumulative_msg_count >= 1'040'000) {
                            LOG(INFO) << "[ORDER5_TRACE_CR_TOKEN_ADVANCE]"
                                      << " local_broker=" << local_broker_id_
                                      << " source=" << src
                                      << " owner=" << h.owner_broker
                                      << " goi=" << h.goi_index
                                      << " role=" << h.role
                                      << " token=" << token
                                      << " pbr=" << h.pbr_index
                                      << " cumulative=" << h.cumulative_msg_count;
                        }
                    } else {
                        token = expected;
                    }
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
                    ops_this_src++;
                    if (ops_this_src >= kMaxPerSourceTokenOpsPerPass) {
                        break;
                    }
                    continue;
                }
                break;
            }
        }
        if (num_brokers_ > 0) {
            token_scan_start_src = (token_scan_start_src + 1) % num_brokers_;
        }

        // Periodic logging
        if (batches_replicated > 0 &&
            (batches_replicated - last_progress_log_batches) >= 1000) {
            LOG(INFO) << "ChainReplicationManager: Replica " << replica_id_
                      << " replicated " << batches_replicated
                      << " batches"
                      << " pending=" << pending_total()
                      << " invalid_source=" << entries_invalid_source
                      << " suspicious_payload=" << entries_suspicious_payload;
            last_progress_log_batches = batches_replicated;
        }

        if (!made_progress) {
            if (pending_total() > 0 || queued_writes.load(std::memory_order_acquire) > 0) {
                // Pending work exists but token ownership is not ready yet; short sleep avoids
                // burning CPU without adding tens-of-microseconds ACK2 jitter.
                std::this_thread::sleep_for(std::chrono::microseconds(2));
            } else {
                std::this_thread::yield();
            }
        }
    }

    shutdown_workers.store(true, std::memory_order_release);
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
    static std::atomic<uint64_t> cv_update_logs{0};
    const uint64_t log_idx = cv_update_logs.fetch_add(1, std::memory_order_relaxed);
    if (log_idx < 16 || (log_idx % 512) == 0 ||
        (ShouldEnableOrder5Trace() && cumulative_msg_count >= 1000000)) {
        LOG(INFO) << "ChainReplicationManager: UpdateCompletionVector local_broker_id="
                  << local_broker_id_
                  << " broker_id=" << broker_id
                  << " pbr_index=" << pbr_index
                  << " cumulative_msg_count=" << cumulative_msg_count
                  << " replica_id=" << replica_id_;
    }

    // Tail replica owns ACK2 durability frontier.
    // All three data fields (completed_pbr_head, sequencer_logical_offset, completed_logical_offset)
    // reside in the first 24 bytes of the 128B-aligned entry — a single 64B cache line.
    CXL::flush_cacheline(&cv_[broker_id]);
    CXL::full_fence();
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
