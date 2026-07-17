#include "chain_replication.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "common/env_flags.h"
#include "common/performance_utils.h"
#include "common/replica_disk_dirs.h"
#include "cxl_manager/cxl_datastructure.h"

namespace Embarcadero {
namespace {

constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
constexpr size_t kDefaultInMemSinkBytesPerSource = 256UL * 1024UL * 1024UL;
// Prefer larger sync batches so fdatasync amortizes over more media bandwidth.
constexpr size_t kDefaultChainSyncBytes = 256UL * 1024UL * 1024UL;
constexpr uint64_t kDefaultChainSyncIntervalNs = 250ULL * 1000ULL * 1000ULL;
constexpr size_t kMaxPendingTokenUpdates = 8192;
constexpr uint64_t kTokenSpinBeforeYield = 4096;
constexpr uint64_t kTokenSpinBeforeSleep = 65536;
constexpr uint64_t kTokenWaitSleepNs = 1'000;
constexpr uint64_t kControlRefreshIntervalEntries = 256;
constexpr uint64_t kStatsLogIntervalNs = 1'000'000'000ULL;  // 1s

struct PendingTokenUpdate {
    uint64_t goi_index{0};
    uint16_t source_broker{0};
    uint16_t owner_broker{0};
    uint64_t pbr_index{0};
    uint64_t cumulative_msg_count{0};
    uint16_t role{0};
    size_t payload_size{0};
    size_t end_offset{0};
    bool cv_updated{false};
};

struct AwaitingMediaSync {
    PendingTokenUpdate task;
    size_t end_offset{0};
};

struct SourcePipelineStats {
    std::atomic<uint64_t> dispatched{0};
    std::atomic<uint64_t> copy_bytes{0};
    std::atomic<uint64_t> sync_count{0};
    std::atomic<uint64_t> sync_ns{0};
    std::atomic<uint64_t> sync_bytes{0};
    std::atomic<uint64_t> token_wait_ns{0};
    std::atomic<uint64_t> token_advances{0};
    std::atomic<uint64_t> cv_advances{0};
    std::atomic<uint64_t> sink_complete{0};
    std::atomic<uint64_t> sync_complete{0};
    std::atomic<uint64_t> first_blocking_goi{UINT64_MAX};
    std::atomic<uint16_t> first_blocking_owner{UINT16_MAX};
    std::atomic<size_t> sink_queue_depth{0};
    std::atomic<size_t> sync_queue_depth{0};
    std::atomic<size_t> token_queue_depth{0};
};

inline void RefreshGOIEntry(GOIEntry* entry) {
    CXL::flush_cacheline(entry);
    CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(entry) + 64);
    CXL::full_fence();
}

inline void RefreshGOIToken(GOIEntry* entry) {
    CXL::flush_cacheline(entry);
    CXL::full_fence();
}

inline uint64_t SteadyNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

inline bool ShouldInvalidatePayloadBeforeRead() {
    static const bool enabled = !ReadEnvBoolStrict("EMBARCADERO_CXL_COHERENT", false);
    return enabled;
}

inline bool ShouldEnableOrder5Trace() {
    static const bool enabled = ReadEnvBoolStrict("EMBARCADERO_ORDER5_TRACE", false);
    return enabled;
}

inline size_t ParseSizeEnv(const char* name, size_t default_val, size_t min_val) {
    const char* env = std::getenv(name);
    if (!env || !env[0]) return default_val;
    char* end = nullptr;
    unsigned long long v = std::strtoull(env, &end, 10);
    if (end == env || v < min_val) return default_val;
    return static_cast<size_t>(v);
}

inline uint64_t ParseMsEnvToNs(const char* name, uint64_t default_ns) {
    const char* env = std::getenv(name);
    if (!env || !env[0]) return default_ns;
    char* end = nullptr;
    unsigned long long v = std::strtoull(env, &end, 10);
    if (end == env || v == 0) return default_ns;
    return static_cast<uint64_t>(v) * 1'000'000ULL;
}

inline void CopyIntoRing(std::vector<uint8_t>& ring, size_t& write_pos, const void* src, size_t len) {
    if (ring.empty() || len == 0) return;
    const size_t cap = ring.size();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
    if (len >= cap) {
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

inline ChainReplicationSinkMode ParseSinkModeFromEnv() {
    if (const char* sink = std::getenv("EMBARCADERO_CHAIN_REPLICATION_SINK")) {
        std::string s(sink);
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (s == "disk-durable" || s == "disk" || s == "disk_durable") {
            return ChainReplicationSinkMode::DiskDurable;
        }
        if (s == "memory-copy" || s == "memory_copy" || s == "mem-copy" || s == "copy") {
            return ChainReplicationSinkMode::MemoryCopy;
        }
        if (s == "memory-accounting" || s == "memory_accounting" || s == "accounting" ||
            s == "mem-accounting") {
            return ChainReplicationSinkMode::MemoryAccounting;
        }
        LOG(WARNING) << "Unknown EMBARCADERO_CHAIN_REPLICATION_SINK='" << sink
                     << "'; falling back to INMEM/INMEM_COPY flags";
    }
    const bool inmem = ReadEnvBoolStrict("EMBARCADERO_CHAIN_REPLICATION_INMEM", false);
    if (!inmem) return ChainReplicationSinkMode::DiskDurable;
    if (ReadEnvBoolStrict("EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY", false)) {
        return ChainReplicationSinkMode::MemoryCopy;
    }
    return ChainReplicationSinkMode::MemoryAccounting;
}

struct SourcePipeline {
    int source_broker{-1};
    int role{-1};
    int fd{-1};
    std::string path;
    size_t write_offset{0};

    std::vector<uint8_t> mem_ring;
    size_t mem_write_pos{0};

    std::mutex sink_mu;
    std::condition_variable sink_cv;
    std::deque<PendingTokenUpdate> sink_q;

    std::mutex sync_mu;
    std::condition_variable sync_cv;
    std::deque<AwaitingMediaSync> awaiting;
    size_t bytes_since_sync{0};
    uint64_t last_sync_ns{0};
    size_t synced_offset{0};
    size_t written_high_water{0};
    uint64_t sync_generation{0};

    std::mutex token_mu;
    std::condition_variable token_cv;
    std::deque<PendingTokenUpdate> token_q;

    SourcePipelineStats stats;
    std::thread sink_thread;
    std::thread sync_thread;
    std::thread token_thread;

    std::atomic<size_t>* global_inflight{nullptr};
    std::atomic<bool>* shutdown{nullptr};
    std::atomic<bool>* write_error{nullptr};
};

}  // namespace

ChainReplicationConfig ParseChainReplicationConfig() {
    ChainReplicationConfig cfg;
    cfg.sink_mode = ParseSinkModeFromEnv();
    cfg.inmem_bytes_per_source = ParseSizeEnv(
        "EMBARCADERO_CHAIN_REPLICATION_INMEM_BYTES_PER_SOURCE",
        kDefaultInMemSinkBytesPerSource, 4096);
    cfg.sync_bytes = ParseSizeEnv("EMBARCADERO_CHAIN_SYNC_BYTES", kDefaultChainSyncBytes, 4096);
    cfg.sync_interval_ns =
        ParseMsEnvToNs("EMBARCADERO_CHAIN_SYNC_INTERVAL_MS", kDefaultChainSyncIntervalNs);
    return cfg;
}

const char* ChainReplicationSinkModeName(ChainReplicationSinkMode mode) {
    switch (mode) {
        case ChainReplicationSinkMode::DiskDurable:
            return "disk-durable";
        case ChainReplicationSinkMode::MemoryCopy:
            return "memory-copy";
        case ChainReplicationSinkMode::MemoryAccounting:
            return "memory-accounting";
    }
    return "unknown";
}

const char* ChainReplicationAckClaimLabel(ChainReplicationSinkMode mode) {
    return mode == ChainReplicationSinkMode::DiskDurable ? "media_durable"
                                                         : "replicated_ack_emulated";
}

ChainReplicationManager::ChainReplicationManager(int replica_id, int replication_factor,
                                                 int local_broker_id, int num_brokers,
                                                 void* cxl_addr, GOIEntry* goi,
                                                 CompletionVectorEntry* cv,
                                                 const std::string& disk_path)
    : replica_id_(replica_id),
      replication_factor_(replication_factor),
      local_broker_id_(local_broker_id),
      num_brokers_(num_brokers),
      cxl_addr_(cxl_addr),
      goi_(goi),
      cv_(cv),
      disk_path_(disk_path),
      config_(ParseChainReplicationConfig()) {
    if (!config_.IsMemorySink()) {
        disk_dirs_ = ResolveWritableReplicationDirs();
        if (disk_dirs_.empty()) {
            LOG(FATAL) << "ChainReplicationManager: no writable replication directories configured; "
                       << "refusing /tmp fallback (ACK2 requires media-durable replicas). "
                       << "Set EMBARCADERO_REPLICA_DISK_DIRS.";
        }
    }

    LOG(INFO) << "ChainReplicationManager: replica_id=" << replica_id
              << " local_broker_id=" << local_broker_id << " num_brokers=" << num_brokers
              << " replication_factor=" << replication_factor << " disk_path=" << disk_path
              << " sink_mode=" << ChainReplicationSinkModeName(config_.sink_mode)
              << " ack_claim=" << ChainReplicationAckClaimLabel(config_.sink_mode)
              << " inmem_bytes_per_source=" << config_.inmem_bytes_per_source
              << " sync_bytes=" << config_.sync_bytes
              << " sync_interval_ms=" << (config_.sync_interval_ns / 1'000'000ULL)
              << " disk_dirs=" << disk_dirs_.size();

    if (config_.IsMemorySink()) {
        LOG(WARNING) << "ChainReplicationManager: memory sink active ("
                     << ChainReplicationSinkModeName(config_.sink_mode)
                     << "); ACK2 must be labeled replicated_ack_emulated, never media_durable.";
    }
}

ChainReplicationManager::~ChainReplicationManager() { Stop(); }

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
    LOG(INFO) << "ChainReplicationManager: Replication thread started (replica_id=" << replica_id_
              << " sink=" << ChainReplicationSinkModeName(config_.sink_mode) << ")";

    const bool in_memory_sink = config_.IsMemorySink();
    const bool in_memory_copy = config_.sink_mode == ChainReplicationSinkMode::MemoryCopy;

    std::atomic<size_t> queued_inflight{0};
    std::atomic<bool> write_error{false};
    std::atomic<bool> shutdown_workers{false};

    std::array<std::unique_ptr<SourcePipeline>, NUM_MAX_BROKERS> pipelines{};
    std::vector<int> active_sources;
    active_sources.reserve(static_cast<size_t>(std::max(0, num_brokers_)));
    for (int src = 0; src < num_brokers_; ++src) {
        const int role =
            Embarcadero::GetReplicationChainRole(local_broker_id_, src, replication_factor_,
                                                 num_brokers_);
        if (role >= 0) active_sources.push_back(src);
    }

    std::vector<size_t> disk_assign;
    std::vector<uint64_t> disk_weights;
    if (!in_memory_sink && !disk_dirs_.empty()) {
        disk_weights = ResolveReplicationDiskWeights(disk_dirs_.size());
        disk_assign =
            AssignSourcesToReplicationDisks(active_sources, disk_dirs_.size(), disk_weights);
        std::ostringstream weight_ss;
        for (size_t i = 0; i < disk_weights.size(); ++i) {
            if (i) weight_ss << ',';
            weight_ss << disk_weights[i];
        }
        LOG(INFO) << "ChainReplicationManager: capacity-aware disk striping dirs="
                  << disk_dirs_.size() << " weights=" << weight_ss.str()
                  << " active_sources=" << active_sources.size();
    }

    for (size_t ai = 0; ai < active_sources.size(); ++ai) {
        const int src = active_sources[ai];
        const int role =
            Embarcadero::GetReplicationChainRole(local_broker_id_, src, replication_factor_,
                                                 num_brokers_);

        auto pipe = std::make_unique<SourcePipeline>();
        pipe->source_broker = src;
        pipe->role = role;
        pipe->global_inflight = &queued_inflight;
        pipe->shutdown = &shutdown_workers;
        pipe->write_error = &write_error;

        if (in_memory_sink) {
            pipe->path = "[in-memory-sink]";
            if (in_memory_copy) {
                pipe->mem_ring.resize(config_.inmem_bytes_per_source);
            }
        } else {
            const size_t disk_i =
                (ai < disk_assign.size()) ? disk_assign[ai]
                                          : (static_cast<size_t>(src) % disk_dirs_.size());
            const std::string& dir = disk_dirs_[disk_i];
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            pipe->path = dir + "/replica_b" + std::to_string(local_broker_id_) + "_src" +
                         std::to_string(src) + ".dat";
            pipe->fd = open(pipe->path.c_str(), O_CREAT | O_RDWR, 0644);
            if (pipe->fd < 0) {
                LOG(FATAL) << "Failed to open replication disk file: " << pipe->path
                           << " error: " << strerror(errno)
                           << " (ACK2 fail-closed: no /tmp fallback)";
            }
            LOG(INFO) << "ChainReplicationManager: source=" << src << " role=" << role
                      << " disk_idx=" << disk_i << " weight="
                      << ((disk_i < disk_weights.size()) ? disk_weights[disk_i] : 1) << " -> "
                      << pipe->path;
        }

        if (in_memory_sink) {
            LOG(INFO) << "ChainReplicationManager: source=" << src << " role=" << role << " -> "
                      << pipe->path;
        }
        pipelines[static_cast<size_t>(src)] = std::move(pipe);
    }

    auto enqueue_token = [](SourcePipeline* pipe, PendingTokenUpdate task) {
        {
            std::lock_guard<std::mutex> lk(pipe->token_mu);
            pipe->token_q.push_back(std::move(task));
            pipe->stats.token_queue_depth.store(pipe->token_q.size(), std::memory_order_relaxed);
        }
        pipe->token_cv.notify_one();
    };

    auto release_synced = [&](SourcePipeline* pipe, size_t synced_to) {
        std::deque<PendingTokenUpdate> ready;
        {
            std::lock_guard<std::mutex> lk(pipe->sync_mu);
            while (!pipe->awaiting.empty() && pipe->awaiting.front().end_offset <= synced_to) {
                ready.push_back(std::move(pipe->awaiting.front().task));
                pipe->awaiting.pop_front();
            }
            pipe->stats.sync_queue_depth.store(pipe->awaiting.size(), std::memory_order_relaxed);
        }
        for (auto& t : ready) {
            pipe->stats.sync_complete.fetch_add(1, std::memory_order_relaxed);
            if (ShouldEnableOrder5Trace() && t.cumulative_msg_count >= 1'040'000) {
                LOG(INFO) << "[ORDER5_TRACE_CR_SYNC_COMPLETE]"
                          << " local_broker=" << local_broker_id_ << " source=" << pipe->source_broker
                          << " owner=" << t.owner_broker << " goi=" << t.goi_index
                          << " role=" << t.role << " pbr=" << t.pbr_index
                          << " cumulative=" << t.cumulative_msg_count;
            }
            enqueue_token(pipe, std::move(t));
            queued_inflight.fetch_sub(1, std::memory_order_release);
        }
    };

    // Sink workers: copy/pwrite only. Disk mode hands off to sync worker.
    for (int src : active_sources) {
        SourcePipeline* pipe = pipelines[static_cast<size_t>(src)].get();
        pipe->sink_thread = std::thread([&, pipe]() {
            while (true) {
                PendingTokenUpdate task;
                {
                    std::unique_lock<std::mutex> lk(pipe->sink_mu);
                    pipe->sink_cv.wait(lk, [&] {
                        return shutdown_workers.load(std::memory_order_acquire) ||
                               !pipe->sink_q.empty();
                    });
                    if (pipe->sink_q.empty()) {
                        if (shutdown_workers.load(std::memory_order_acquire)) break;
                        continue;
                    }
                    task = std::move(pipe->sink_q.front());
                    pipe->sink_q.pop_front();
                    pipe->stats.sink_queue_depth.store(pipe->sink_q.size(),
                                                      std::memory_order_relaxed);
                }

                GOIEntry* entry = &goi_[task.goi_index];
                const size_t payload_size = static_cast<size_t>(entry->payload_size);
                task.payload_size = payload_size;
                void* payload = reinterpret_cast<uint8_t*>(cxl_addr_) + entry->blog_offset;
                if (ShouldInvalidatePayloadBeforeRead()) {
                    CXL::invalidate_cache_range_for_read(payload, payload_size);
                    CXL::load_fence();
                }

                if (in_memory_sink) {
                    if (in_memory_copy) {
                        CopyIntoRing(pipe->mem_ring, pipe->mem_write_pos, payload, payload_size);
                    } else {
                        pipe->mem_write_pos += payload_size;
                    }
                    pipe->stats.copy_bytes.fetch_add(payload_size, std::memory_order_relaxed);
                    pipe->stats.sink_complete.fetch_add(1, std::memory_order_relaxed);
                    if (ShouldEnableOrder5Trace() && task.cumulative_msg_count >= 1'040'000) {
                        LOG(INFO) << "[ORDER5_TRACE_CR_SINK_COMPLETE]"
                                  << " local_broker=" << local_broker_id_
                                  << " source=" << pipe->source_broker
                                  << " owner=" << task.owner_broker << " goi=" << task.goi_index
                                  << " role=" << task.role << " bytes=" << payload_size
                                  << " cumulative=" << task.cumulative_msg_count;
                    }
                    enqueue_token(pipe, std::move(task));
                    queued_inflight.fetch_sub(1, std::memory_order_release);
                    continue;
                }

                const size_t write_off = pipe->write_offset;
                pipe->write_offset += payload_size;
                task.end_offset = write_off + payload_size;
                ssize_t written = -1;
                while (true) {
                    written = pwrite(pipe->fd, payload, payload_size, write_off);
                    if (written == static_cast<ssize_t>(payload_size)) break;
                    if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                    LOG(ERROR) << "ChainReplicationManager: pwrite failed, wrote " << written
                               << " expected " << payload_size << " errno=" << errno
                               << " goi_index=" << task.goi_index
                               << " source_broker=" << pipe->source_broker;
                    write_error.store(true, std::memory_order_release);
                    stop_.store(true, std::memory_order_release);
                    queued_inflight.fetch_sub(1, std::memory_order_release);
                    break;
                }
                if (write_error.load(std::memory_order_acquire)) continue;

                pipe->stats.copy_bytes.fetch_add(payload_size, std::memory_order_relaxed);
                pipe->stats.sink_complete.fetch_add(1, std::memory_order_relaxed);
                if (ShouldEnableOrder5Trace() && task.cumulative_msg_count >= 1'040'000) {
                    LOG(INFO) << "[ORDER5_TRACE_CR_SINK_COMPLETE]"
                              << " local_broker=" << local_broker_id_
                              << " source=" << pipe->source_broker << " owner=" << task.owner_broker
                              << " goi=" << task.goi_index << " role=" << task.role
                              << " bytes=" << payload_size
                              << " end_offset=" << task.end_offset
                              << " cumulative=" << task.cumulative_msg_count;
                }

                {
                    std::lock_guard<std::mutex> lk(pipe->sync_mu);
                    pipe->written_high_water = task.end_offset;
                    pipe->awaiting.push_back(AwaitingMediaSync{std::move(task), task.end_offset});
                    pipe->bytes_since_sync += payload_size;
                    pipe->stats.sync_queue_depth.store(pipe->awaiting.size(),
                                                      std::memory_order_relaxed);
                }
                pipe->sync_cv.notify_one();
            }
        });
    }

    // Sync workers (disk only): fdatasync by byte/time thresholds; release <= synced offset.
    if (!in_memory_sink) {
        for (int src : active_sources) {
            SourcePipeline* pipe = pipelines[static_cast<size_t>(src)].get();
            pipe->sync_thread = std::thread([&, pipe]() {
                while (true) {
                    bool do_sync = false;
                    size_t high_water = 0;
                    {
                        std::unique_lock<std::mutex> lk(pipe->sync_mu);
                        pipe->sync_cv.wait_for(
                            lk, std::chrono::milliseconds(std::max<uint64_t>(
                                    1, config_.sync_interval_ns / 1'000'000ULL)),
                            [&] {
                                if (shutdown_workers.load(std::memory_order_acquire)) return true;
                                if (pipe->awaiting.empty()) return false;
                                const uint64_t now = SteadyNowNs();
                                return pipe->bytes_since_sync >= config_.sync_bytes ||
                                       pipe->last_sync_ns == 0 ||
                                       (now - pipe->last_sync_ns) >= config_.sync_interval_ns;
                            });
                        if (pipe->awaiting.empty()) {
                            if (shutdown_workers.load(std::memory_order_acquire)) break;
                            continue;
                        }
                        const uint64_t now = SteadyNowNs();
                        do_sync = pipe->bytes_since_sync >= config_.sync_bytes ||
                                  pipe->last_sync_ns == 0 ||
                                  (now - pipe->last_sync_ns) >= config_.sync_interval_ns ||
                                  shutdown_workers.load(std::memory_order_acquire);
                        high_water = pipe->written_high_water;
                    }
                    if (!do_sync) continue;

                    // [[SLOW_SYNC_INJECT]] Artificial post-fdatasync sleep for
                    // ordering-replication independence experiment. With
                    // EMBARCADERO_SYNC_SLEEP_MS set, the sync thread stalls after
                    // each fdatasync, raising ACK2 P99 while leaving network-receive
                    // threads (and thus ACK1) unaffected.
                    {
                        static const int64_t kSleepMs = [] {
                            const char* e = std::getenv("EMBARCADERO_SYNC_SLEEP_MS");
                            return (e && e[0]) ? std::atoll(e) : 0LL;
                        }();
                        if (kSleepMs > 0) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(kSleepMs));
                        }
                    }
                    const uint64_t sync_start = SteadyNowNs();
                    if (fdatasync(pipe->fd) < 0) {
                        LOG(ERROR) << "ChainReplicationManager: fdatasync failed errno=" << errno
                                   << " source_broker=" << pipe->source_broker
                                   << " path=" << pipe->path;
                        write_error.store(true, std::memory_order_release);
                        stop_.store(true, std::memory_order_release);
                        break;
                    }
                    const uint64_t sync_elapsed = SteadyNowNs() - sync_start;
                    size_t synced_bytes = 0;
                    {
                        std::lock_guard<std::mutex> lk(pipe->sync_mu);
                        synced_bytes = high_water > pipe->synced_offset
                                           ? (high_water - pipe->synced_offset)
                                           : 0;
                        pipe->synced_offset = high_water;
                        pipe->bytes_since_sync = 0;
                        pipe->last_sync_ns = SteadyNowNs();
                        ++pipe->sync_generation;
                    }
                    pipe->stats.sync_count.fetch_add(1, std::memory_order_relaxed);
                    pipe->stats.sync_ns.fetch_add(sync_elapsed, std::memory_order_relaxed);
                    pipe->stats.sync_bytes.fetch_add(synced_bytes, std::memory_order_relaxed);
                    release_synced(pipe, high_water);

                    if (shutdown_workers.load(std::memory_order_acquire)) {
                        std::lock_guard<std::mutex> lk(pipe->sync_mu);
                        if (pipe->awaiting.empty()) break;
                    }
                }
            });
        }
    }

    // Token workers: wait for predecessor AFTER sink/sync; only tail updates CV.
    for (int src : active_sources) {
        SourcePipeline* pipe = pipelines[static_cast<size_t>(src)].get();
        pipe->token_thread = std::thread([&, pipe]() {
            uint64_t stall_log_deadline_ns = 0;
            while (true) {
                PendingTokenUpdate task;
                {
                    std::unique_lock<std::mutex> lk(pipe->token_mu);
                    pipe->token_cv.wait(lk, [&] {
                        return shutdown_workers.load(std::memory_order_acquire) ||
                               !pipe->token_q.empty();
                    });
                    if (pipe->token_q.empty()) {
                        if (shutdown_workers.load(std::memory_order_acquire)) break;
                        continue;
                    }
                    // Peek head; only pop when token+CV stages complete.
                    task = pipe->token_q.front();
                }

                GOIEntry* entry = &goi_[task.goi_index];
                const uint64_t wait_start = SteadyNowNs();
                bool completed = false;
                bool wait_recorded = false;
                uint64_t wait_spins = 0;
                while (!completed) {
                    RefreshGOIToken(entry);
                    uint32_t token = entry->num_replicated.load(std::memory_order_acquire);
                    if (token < task.role) {
                        if (pipe->stats.first_blocking_goi.load(std::memory_order_relaxed) ==
                            UINT64_MAX) {
                            pipe->stats.first_blocking_goi.store(task.goi_index,
                                                                std::memory_order_relaxed);
                            pipe->stats.first_blocking_owner.store(task.owner_broker,
                                                                  std::memory_order_relaxed);
                        }
                        if (ShouldEnableOrder5Trace()) {
                            const uint64_t now = SteadyNowNs();
                            if (now >= stall_log_deadline_ns) {
                                LOG(INFO) << "[ORDER5_TRACE_CR_TOKEN_WAIT]"
                                          << " local_broker=" << local_broker_id_
                                          << " source=" << pipe->source_broker
                                          << " owner=" << task.owner_broker
                                          << " goi=" << task.goi_index << " role=" << task.role
                                          << " token=" << token << " pbr=" << task.pbr_index
                                          << " cumulative=" << task.cumulative_msg_count
                                          << " pending_src="
                                          << pipe->stats.token_queue_depth.load(
                                                 std::memory_order_relaxed);
                                stall_log_deadline_ns = now + 250'000'000ULL;
                            }
                        }
                        // Spin/pause on the hot path; only sleep after a long stall so
                        // predecessor token advances are not delayed by 64µs backoff.
                        ++wait_spins;
                        if (wait_spins < kTokenSpinBeforeYield) {
                            CXL::cpu_pause();
                        } else if (wait_spins < kTokenSpinBeforeSleep) {
                            for (int i = 0; i < 32; ++i) CXL::cpu_pause();
                            std::this_thread::yield();
                        } else {
                            std::this_thread::sleep_for(std::chrono::nanoseconds(kTokenWaitSleepNs));
                        }
                        continue;
                    }
                    if (!wait_recorded) {
                        pipe->stats.token_wait_ns.fetch_add(SteadyNowNs() - wait_start,
                                                           std::memory_order_relaxed);
                        wait_recorded = true;
                        wait_spins = 0;
                    }

                    if (token == task.role) {
                        uint32_t expected = static_cast<uint32_t>(task.role);
                        const uint32_t desired = static_cast<uint32_t>(task.role) + 1u;
                        if (entry->num_replicated.compare_exchange_strong(
                                expected, desired, std::memory_order_release,
                                std::memory_order_acquire)) {
                            CXL::store_fence();
                            CXL::flush_cacheline(entry);
                            CXL::store_fence();
                            token = desired;
                            pipe->stats.token_advances.fetch_add(1, std::memory_order_relaxed);
                            if (ShouldEnableOrder5Trace() &&
                                task.cumulative_msg_count >= 1'040'000) {
                                LOG(INFO) << "[ORDER5_TRACE_CR_TOKEN_ADVANCE]"
                                          << " local_broker=" << local_broker_id_
                                          << " source=" << pipe->source_broker
                                          << " owner=" << task.owner_broker
                                          << " goi=" << task.goi_index << " role=" << task.role
                                          << " token=" << token << " pbr=" << task.pbr_index
                                          << " cumulative=" << task.cumulative_msg_count;
                            }
                        } else {
                            token = expected;
                        }
                    }

                    if (task.role == static_cast<uint16_t>(replication_factor_ - 1) &&
                        token >= static_cast<uint16_t>(replication_factor_) && !task.cv_updated) {
                        UpdateCompletionVector(task.owner_broker, task.pbr_index,
                                               task.cumulative_msg_count);
                        task.cv_updated = true;
                        pipe->stats.cv_advances.fetch_add(1, std::memory_order_relaxed);
                        if (ShouldEnableOrder5Trace() && task.cumulative_msg_count >= 1'040'000) {
                            LOG(INFO) << "[ORDER5_TRACE_CR_CV_ADVANCE]"
                                      << " local_broker=" << local_broker_id_
                                      << " source=" << pipe->source_broker
                                      << " owner=" << task.owner_broker
                                      << " goi=" << task.goi_index << " pbr=" << task.pbr_index
                                      << " cumulative=" << task.cumulative_msg_count;
                        }
                    }

                    const bool token_done = token >= static_cast<uint16_t>(task.role + 1);
                    const bool tail_cv_done =
                        (task.role != static_cast<uint16_t>(replication_factor_ - 1)) ||
                        task.cv_updated;
                    if (token_done && tail_cv_done) {
                        std::lock_guard<std::mutex> lk(pipe->token_mu);
                        if (!pipe->token_q.empty() &&
                            pipe->token_q.front().goi_index == task.goi_index) {
                            pipe->token_q.pop_front();
                            pipe->stats.token_queue_depth.store(pipe->token_q.size(),
                                                               std::memory_order_relaxed);
                        }
                        completed = true;
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }

    ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
    uint64_t cached_committed_seq = UINT64_MAX;
    uint16_t cached_epoch = 0;
    uint64_t last_control_refresh_goi = 0;
    uint64_t entries_invalid_source = 0;
    uint64_t entries_suspicious_payload = 0;
    uint64_t batches_dispatched = 0;
    uint64_t last_stats_log_ns = SteadyNowNs();

    auto refresh_control_state = [&](uint64_t goi_index) {
        CXL::flush_cacheline(control_block);
        CXL::full_fence();
        cached_committed_seq = control_block->committed_seq.load(std::memory_order_acquire);
        cached_epoch = static_cast<uint16_t>(
            control_block->epoch.load(std::memory_order_acquire) & 0xFFFFu);
        last_control_refresh_goi = goi_index;
    };

    // Inflight covers sink+awaiting-sync. Token queue is post-release and counted separately.
    auto pending_and_inflight = [&]() -> size_t {
        size_t n = queued_inflight.load(std::memory_order_acquire);
        for (int src : active_sources) {
            SourcePipeline* pipe = pipelines[static_cast<size_t>(src)].get();
            n += pipe->stats.token_queue_depth.load(std::memory_order_relaxed);
        }
        return n;
    };

    auto log_pipeline_stats = [&](bool force) {
        const uint64_t now = SteadyNowNs();
        if (!force && (now - last_stats_log_ns) < kStatsLogIntervalNs) return;
        last_stats_log_ns = now;
        std::ostringstream oss;
        oss << "[CHAIN_PIPELINE_STATS] local_broker=" << local_broker_id_
            << " sink=" << ChainReplicationSinkModeName(config_.sink_mode)
            << " ack_claim=" << ChainReplicationAckClaimLabel(config_.sink_mode)
            << " dispatched=" << batches_dispatched
            << " inflight=" << queued_inflight.load(std::memory_order_relaxed);
        for (int src : active_sources) {
            SourcePipeline* pipe = pipelines[static_cast<size_t>(src)].get();
            const uint64_t sync_count = pipe->stats.sync_count.load(std::memory_order_relaxed);
            const uint64_t sync_ns = pipe->stats.sync_ns.load(std::memory_order_relaxed);
            oss << " | src=" << src << " role=" << pipe->role
                << " disp=" << pipe->stats.dispatched.load(std::memory_order_relaxed)
                << " copyB=" << pipe->stats.copy_bytes.load(std::memory_order_relaxed)
                << " sinkQ=" << pipe->stats.sink_queue_depth.load(std::memory_order_relaxed)
                << " syncQ=" << pipe->stats.sync_queue_depth.load(std::memory_order_relaxed)
                << " tokQ=" << pipe->stats.token_queue_depth.load(std::memory_order_relaxed)
                << " syncN=" << sync_count << " syncNs=" << sync_ns
                << " syncB=" << pipe->stats.sync_bytes.load(std::memory_order_relaxed)
                << " tokWaitNs=" << pipe->stats.token_wait_ns.load(std::memory_order_relaxed)
                << " tokAdv=" << pipe->stats.token_advances.load(std::memory_order_relaxed)
                << " cvAdv=" << pipe->stats.cv_advances.load(std::memory_order_relaxed)
                << " blockGoi=" << pipe->stats.first_blocking_goi.load(std::memory_order_relaxed)
                << " blockOwner="
                << pipe->stats.first_blocking_owner.load(std::memory_order_relaxed);
        }
        LOG(INFO) << oss.str();
    };

    while (true) {
        const bool stop_requested = stop_.load(std::memory_order_acquire);
        if (stop_requested) {
            const uint64_t goi_index = next_goi_index_.load(std::memory_order_relaxed);
            refresh_control_state(goi_index);
            const bool more_committed_goi =
                (cached_committed_seq != UINT64_MAX && goi_index <= cached_committed_seq);
            if (!more_committed_goi && pending_and_inflight() == 0) {
                break;
            }
        }

        bool made_progress = false;

        // Stage 1: ordered GOI dispatch with copy-ahead (no predecessor-token gate).
        while (pending_and_inflight() < kMaxPendingTokenUpdates) {
            const uint64_t goi_index = next_goi_index_.load(std::memory_order_relaxed);

            if (cached_committed_seq == UINT64_MAX || goi_index > cached_committed_seq ||
                goi_index - last_control_refresh_goi >= kControlRefreshIntervalEntries) {
                refresh_control_state(goi_index);
            }
            if (cached_committed_seq == UINT64_MAX || goi_index > cached_committed_seq) {
                break;
            }
            const uint16_t current_epoch = cached_epoch;

            GOIEntry* entry = &goi_[goi_index];
            RefreshGOIEntry(entry);

            if (entry->global_seq != goi_index) {
                if (entry->epoch_sequenced != current_epoch) {
                    entries_invalid_source++;
                    if (entries_invalid_source <= 8 || (entries_invalid_source % 1024) == 0) {
                        LOG(WARNING)
                            << "ChainReplicationManager: skipping stale GOI entry with global_seq "
                               "mismatch"
                            << " goi_index=" << goi_index << " committed_seq=" << cached_committed_seq
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
            if (source_broker < 0 || source_broker >= num_brokers_ || payload_size == 0 ||
                message_count == 0) {
                if (entry->epoch_sequenced != current_epoch) {
                    entries_suspicious_payload++;
                    if (entries_suspicious_payload <= 8 ||
                        (entries_suspicious_payload % 1024) == 0) {
                        LOG(WARNING)
                            << "ChainReplicationManager: skipping stale GOI entry with invalid "
                               "metadata"
                            << " goi_index=" << goi_index << " source_broker=" << source_broker
                            << " payload_size=" << payload_size
                            << " message_count=" << message_count;
                    }
                    next_goi_index_.fetch_add(1, std::memory_order_relaxed);
                    made_progress = true;
                }
                break;
            }

            SourcePipeline* pipe = nullptr;
            if (source_broker < NUM_MAX_BROKERS) {
                pipe = pipelines[static_cast<size_t>(source_broker)].get();
            }
            if (pipe == nullptr) {
                next_goi_index_.fetch_add(1, std::memory_order_relaxed);
                made_progress = true;
                continue;
            }

            PendingTokenUpdate pending_update;
            pending_update.goi_index = goi_index;
            pending_update.source_broker = static_cast<uint16_t>(source_broker);
            pending_update.owner_broker = entry->broker_id;
            pending_update.pbr_index = entry->pbr_index;
            pending_update.cumulative_msg_count = entry->cumulative_message_count;
            pending_update.role = static_cast<uint16_t>(pipe->role);
            pending_update.payload_size = payload_size;

            {
                std::lock_guard<std::mutex> lk(pipe->sink_mu);
                pipe->sink_q.push_back(std::move(pending_update));
                pipe->stats.sink_queue_depth.store(pipe->sink_q.size(), std::memory_order_relaxed);
            }
            pipe->sink_cv.notify_one();
            queued_inflight.fetch_add(1, std::memory_order_release);
            pipe->stats.dispatched.fetch_add(1, std::memory_order_relaxed);
            batches_dispatched++;
            next_goi_index_.fetch_add(1, std::memory_order_relaxed);
            made_progress = true;

            if (ShouldEnableOrder5Trace() &&
                entry->cumulative_message_count >= 1'040'000) {
                LOG(INFO) << "[ORDER5_TRACE_CR_DISPATCH]"
                          << " local_broker=" << local_broker_id_ << " source=" << source_broker
                          << " owner=" << entry->broker_id << " goi=" << goi_index
                          << " role=" << pipe->role << " pbr=" << entry->pbr_index
                          << " cumulative=" << entry->cumulative_message_count;
            }
        }

        log_pipeline_stats(false);

        if (!made_progress) {
            if (pending_and_inflight() > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(2));
            } else {
                std::this_thread::yield();
            }
        }
    }

    shutdown_workers.store(true, std::memory_order_release);
    for (int src : active_sources) {
        SourcePipeline* pipe = pipelines[static_cast<size_t>(src)].get();
        pipe->sink_cv.notify_all();
        pipe->sync_cv.notify_all();
        pipe->token_cv.notify_all();
    }
    for (int src : active_sources) {
        SourcePipeline* pipe = pipelines[static_cast<size_t>(src)].get();
        if (pipe->sink_thread.joinable()) pipe->sink_thread.join();
        if (pipe->sync_thread.joinable()) pipe->sync_thread.join();
        if (pipe->token_thread.joinable()) pipe->token_thread.join();
        if (pipe->fd >= 0) {
            close(pipe->fd);
            pipe->fd = -1;
        }
    }

    log_pipeline_stats(true);
    LOG(INFO) << "ChainReplicationManager: Replication thread exiting, dispatched "
              << batches_dispatched << " batches"
              << " invalid_source=" << entries_invalid_source
              << " suspicious_payload=" << entries_suspicious_payload;
}

void ChainReplicationManager::UpdateCompletionVector(uint16_t broker_id, uint64_t pbr_index,
                                                     uint64_t cumulative_msg_count) {
    static std::atomic<uint64_t> cv_update_logs{0};
    const uint64_t log_idx = cv_update_logs.fetch_add(1, std::memory_order_relaxed);
    if (log_idx < 16 || (log_idx % 512) == 0 ||
        (ShouldEnableOrder5Trace() && cumulative_msg_count >= 1000000)) {
        LOG(INFO) << "ChainReplicationManager: UpdateCompletionVector local_broker_id="
                  << local_broker_id_ << " broker_id=" << broker_id << " pbr_index=" << pbr_index
                  << " cumulative_msg_count=" << cumulative_msg_count
                  << " replica_id=" << replica_id_;
    }

    CXL::flush_cacheline(&cv_[broker_id]);
    CXL::full_fence();
    uint64_t cur = cv_[broker_id].completed_pbr_head.load(std::memory_order_acquire);
    while (cur == kNoProgress || pbr_index > cur) {
        if (cv_[broker_id].completed_pbr_head.compare_exchange_strong(cur, pbr_index,
                                                                      std::memory_order_release)) {
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

    CXL::store_fence();
    CXL::flush_cacheline(&cv_[broker_id]);
    CXL::store_fence();
}

}  // namespace Embarcadero
