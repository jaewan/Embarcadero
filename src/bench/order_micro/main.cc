#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <random>
#include <deque>
#include <string>
#include <iostream>
#include <algorithm>
#include <unordered_set>

#include "glog/logging.h"
#include "cxxopts.hpp"

#include "embarlet/message_ordering.h"
#include "common/config.h"
#include "cxl_manager/cxl_datastructure.h"
extern "C" void* bench_map_cxl(size_t size);

using namespace Embarcadero;

static constexpr size_t kAlign = 64;

static void* aligned_alloc_or_die(size_t align, size_t size) {
    void* p = nullptr;
    if (posix_memalign(&p, align, size) != 0 || p == nullptr) {
        LOG(FATAL) << "posix_memalign failed for size=" << size;
    }
    std::memset(p, 0, size);
    return p;
}

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    std::ios::sync_with_stdio(false);
    std::cout.setf(std::ios::unitbuf);
    cxxopts::Options options("order_micro_bench", "Ordering layer scalability microbenchmark");
    options.add_options()
        ("brokers", "Number of brokers", cxxopts::value<int>()->default_value("4"))
        ("batch_size", "Messages per batch (defaults to BATCH_SIZE/padded)", cxxopts::value<int>())
        ("message_size", "Payload bytes per message (before header & padding)", cxxopts::value<int>()->default_value("256"))
        ("clients_per_broker", "Clients per broker", cxxopts::value<int>()->default_value("1"))
        ("pattern", "ordered|gaps|dups", cxxopts::value<std::string>()->default_value("ordered"))
        ("gap_ratio", "0.0..0.9 fraction of out-of-order batches", cxxopts::value<double>()->default_value("0.2"))
        ("dup_ratio", "0.0..0.5 fraction of duplicate batches", cxxopts::value<double>()->default_value("0.02"))
        ("seed", "PRNG seed", cxxopts::value<uint64_t>()->default_value("1"))
        ("target_msgs_per_s", "Per-broker target message rate (0 = unlimited)", cxxopts::value<double>()->default_value("0"))
        ("use_real_cxl", "Use real CXL mapping via CXLManager (0/1)", cxxopts::value<int>()->default_value("1"))
        ("flush_metadata", "Flush metadata cachelines to emulate uncached reads (0/1)", cxxopts::value<int>()->default_value("0"))
        ("pin_seq_cpus", "Comma-separated CPU list to pin sequencer threads (e.g., 0,2,4)", cxxopts::value<std::string>()->default_value(""))
        ("headers_only", "Order without per-message memory touches (0/1)", cxxopts::value<int>()->default_value("1"))
        ("csv_out", "Write per-run CSV summary to this file", cxxopts::value<std::string>())
        ("per_thread_csv", "Write per-thread stats CSV to this file", cxxopts::value<std::string>())
        ("broker_head_ip", "Head IP for CXLManager (if needed)", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("duration_s", "Duration seconds", cxxopts::value<int>()->default_value("10"))
        ("warmup_s", "Warmup seconds", cxxopts::value<int>()->default_value("2"))
        ("verify", "Verify correctness (client batch seq and global total order)", cxxopts::value<int>()->default_value("0"))
        ("help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    const int brokers = result["brokers"].as<int>();
    const int warmup_s = result["warmup_s"].as<int>();
    const int duration_s = result["duration_s"].as<int>();
    const size_t payload_bytes = static_cast<size_t>(result["message_size"].as<int>());
    const int clients_per_broker = result["clients_per_broker"].as<int>();
    const std::string pattern = result["pattern"].as<std::string>();
    const double gap_ratio = result["gap_ratio"].as<double>();
    const double dup_ratio = result["dup_ratio"].as<double>();
    const uint64_t seed = result["seed"].as<uint64_t>();
    const double target_msgs_per_s = result["target_msgs_per_s"].as<double>();
    const bool verify = result["verify"].as<int>() != 0;
    const bool use_real_cxl = result["use_real_cxl"].as<int>() != 0;
    const bool flush_metadata = result["flush_metadata"].as<int>() != 0;
    const bool write_csv = result.count("csv_out") > 0;
    const bool write_thread_csv = result.count("per_thread_csv") > 0;
    const std::string csv_path = write_csv ? result["csv_out"].as<std::string>() : std::string();
    const std::string thread_csv_path = write_thread_csv ? result["per_thread_csv"].as<std::string>() : std::string();
    const std::string pin_seq_cpus = result["pin_seq_cpus"].as<std::string>();
    const bool headers_only = result["headers_only"].as<int>() != 0;
    const std::string head_ip = result["broker_head_ip"].as<std::string>();

    auto align_up = [](size_t x, size_t a) -> size_t {
        return (x + (a - 1)) & ~(a - 1);
    };

    // Compute padded size and default batch size if omitted
    const size_t padded_size = align_up(sizeof(MessageHeader) + payload_bytes, 64);
    size_t batch_size = 0;
    if (result.count("batch_size") == 0) {
        batch_size = std::max<size_t>(1, static_cast<size_t>(BATCH_SIZE / padded_size));
    } else {
        batch_size = static_cast<size_t>(result["batch_size"].as<int>());
    }

    std::cout << "Config: brokers=" << brokers
              << " clients_per_broker=" << clients_per_broker
              << " batch_size=" << batch_size
              << " message_size=" << payload_bytes
              << " pattern=" << pattern
              << " gap_ratio=" << gap_ratio
              << " dup_ratio=" << dup_ratio
              << " target_msgs_per_s=" << target_msgs_per_s
              << " warmup_s=" << warmup_s
              << " duration_s=" << duration_s << std::endl;

    // Allocate CXL region: real or synthetic
    const size_t region_size = 1ull << 30; // 1 GiB default for synthetic
    void* region = nullptr;
    if (use_real_cxl) {
        region = bench_map_cxl(region_size);
    }
    if (!region) {
        region = aligned_alloc_or_die(4096, region_size);
    }

    // Allocate tinode in region head for realism
    TInode* tinode = reinterpret_cast<TInode*>(aligned_alloc_or_die(kAlign, sizeof(TInode)));
    std::memset(tinode, 0, sizeof(TInode));
    tinode->seq_type = EMBARCADERO;
    tinode->order = 4;

    // Layout per-broker areas: simplistic equal partitioning
    size_t batch_headers_bytes = 256 * 1024 * 1024; // enlarge headers region for sustained raw runs
    // Split header space into input batch headers and export headers to avoid overlap corruption
    const size_t bh_input_bytes = batch_headers_bytes / 2;
    const size_t bh_export_bytes = batch_headers_bytes - bh_input_bytes;
    const size_t per_broker_log = (region_size - batch_headers_bytes) / std::max(brokers, 1);
    uint8_t* base = reinterpret_cast<uint8_t*>(region);
    const size_t per_broker_bh_input = bh_input_bytes / std::max(brokers, 1);
    const size_t per_broker_bh_export = bh_export_bytes / std::max(brokers, 1);
    uint8_t* bh_input_base = base;
    uint8_t* bh_export_base = base + bh_input_bytes;
    uint8_t* log_base = base + batch_headers_bytes;

    // Zero-initialize input/export header regions to avoid reading stale fields
    std::memset(bh_input_base, 0, bh_input_bytes);
    std::memset(bh_export_base, 0, bh_export_bytes);

    for (int b = 0; b < brokers; ++b) {
        tinode->offsets[b].batch_headers_offset = static_cast<size_t>(bh_input_base + b * per_broker_bh_input - reinterpret_cast<uint8_t*>(region));
        tinode->offsets[b].log_offset = static_cast<size_t>(log_base + b * per_broker_log - reinterpret_cast<uint8_t*>(region));
        tinode->offsets[b].written = 0;
        tinode->offsets[b].ordered = 0;
    }

    MessageOrdering ordering(region, tinode, /*broker_id=*/0);

    ordering.SetGetRegisteredBrokersCallback([&](absl::btree_set<int>& set, TInode* /*unused*/) {
        for (int b = 0; b < brokers; ++b) set.insert(b);
        return true;
    });

    std::cout << "Starting Sequencer4 with " << brokers << " brokers" << std::endl;
    ordering.StartSequencer(SequencerType::EMBARCADERO, /*order=*/4, /*topic=*/"bench");
#ifdef BUILDING_ORDER_BENCH
    ordering.SetBenchFlushMetadata(flush_metadata);
    ordering.SetBenchHeadersOnly(headers_only);
    if (!pin_seq_cpus.empty()) {
        std::vector<int> cpus;
        size_t start = 0;
        while (start < pin_seq_cpus.size()) {
            size_t comma = pin_seq_cpus.find(',', start);
            std::string token = pin_seq_cpus.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!token.empty()) {
                try {
                    cpus.push_back(std::stoi(token));
                } catch (...) {}
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!cpus.empty()) {
            ordering.SetBenchPinSequencerCpus(cpus);
        }
    }
#endif

    // Writer threads creating batches and messages
    std::atomic<bool> stop_writers{false};
    std::vector<std::thread> writers;
    std::atomic<bool> full_speed{target_msgs_per_s == 0.0 ? false : true};
    writers.reserve(brokers);
    for (int b = 0; b < brokers; ++b) {
        writers.emplace_back([&, b]() {
            uint8_t* broker_log_ptr = reinterpret_cast<uint8_t*>(region) + tinode->offsets[b].log_offset;
            uint8_t* broker_bh_ptr = reinterpret_cast<uint8_t*>(region) + tinode->offsets[b].batch_headers_offset;
            uint8_t* broker_export_bh_ptr = bh_export_base + b * per_broker_bh_export;
            uint8_t* broker_log_begin = reinterpret_cast<uint8_t*>(region) + tinode->offsets[b].log_offset;
            uint8_t* broker_log_end = broker_log_begin + per_broker_log;
            size_t logical_offset_next = 0;

            const size_t msg_stride = padded_size;
            size_t max_batches = (per_broker_bh_input / sizeof(BatchHeader));
            if (max_batches > 2) max_batches -= 2; // keep two spares to avoid pointer overlap
            const size_t batches_fit_in_log = (batch_size > 0) ? (per_broker_log / (batch_size * msg_stride)) : 0;
            if (batches_fit_in_log > 0) {
                max_batches = std::min(max_batches, (batches_fit_in_log > 2 ? batches_fit_in_log - 2 : batches_fit_in_log));
            }
            if (max_batches < 2) max_batches = 2; // ensure ring has at least 2 entries
            const size_t capacity_msgs = (batches_fit_in_log > 0 ? (batches_fit_in_log - 2 > 0 ? batches_fit_in_log - 2 : 0) : 0) * batch_size;
            // Inform orderer of our header ring to enable wrap-around scanning
#ifdef BUILDING_ORDER_BENCH
            ordering.SetBenchBatchHeaderRing(b, reinterpret_cast<BatchHeader*>(broker_bh_ptr), max_batches);
            size_t max_export_batches = (per_broker_bh_export / sizeof(BatchHeader));
            if (max_export_batches > 2) max_export_batches -= 2;
            if (max_export_batches < 2) max_export_batches = 2;
            ordering.SetBenchExportHeaderRing(b, reinterpret_cast<BatchHeader*>(broker_export_bh_ptr), max_export_batches);
#endif
            const double batch_interval_sec = (target_msgs_per_s > 0.0)
                ? static_cast<double>(batch_size) / target_msgs_per_s
                : 0.0;
            const double warmup_batch_interval_sec = (target_msgs_per_s == 0.0) ? 0.001 : batch_interval_sec;
            auto next_release = std::chrono::steady_clock::now();
            size_t batch_index = 0;

            std::mt19937_64 rng(seed + static_cast<uint64_t>(b));
            std::uniform_real_distribution<double> uni(0.0, 1.0);
            std::vector<size_t> next_seq(static_cast<size_t>(clients_per_broker), 0);
            std::vector<std::deque<size_t>> delayed(static_cast<size_t>(clients_per_broker));
            int rr_client = 0;

            while (!stop_writers.load(std::memory_order_relaxed)) {
                if (target_msgs_per_s == 0.0) {
                    if (batch_index >= max_batches) break; // do not wrap header ring in raw mode
                } else {
                    if (batch_index >= max_batches) batch_index = 0; // ring when paced
                }

                // Flow control: avoid overwriting un-ordered log space
                if (capacity_msgs > 0 && target_msgs_per_s == 0.0) {
                    size_t produced_msgs = logical_offset_next;
                    size_t ordered_msgs = tinode->offsets[b].ordered;
                    while (produced_msgs - ordered_msgs + batch_size > capacity_msgs && !stop_writers.load(std::memory_order_relaxed)) {
                        std::this_thread::yield();
                        ordered_msgs = tinode->offsets[b].ordered;
                    }
                }

                auto* batch_header = reinterpret_cast<BatchHeader*>(broker_bh_ptr + (batch_index % max_batches) * sizeof(BatchHeader));
                // Choose client
                size_t client_id = static_cast<size_t>(rr_client);
                rr_client = (rr_client + 1) % clients_per_broker;

                // Decide batch_seq based on pattern
                size_t batch_seq = 0;
                if (pattern == "ordered") {
                    batch_seq = next_seq[client_id]++;
                } else if (pattern == "gaps") {
                    bool emit_delayed = !delayed[client_id].empty() && (delayed[client_id].size() > 8 || uni(rng) < 0.3);
                    if (emit_delayed) {
                        batch_seq = delayed[client_id].front();
                        delayed[client_id].pop_front();
                    } else if (uni(rng) < gap_ratio) {
                        // Skip one seq now, emit it later
                        delayed[client_id].push_back(next_seq[client_id]);
                        batch_seq = next_seq[client_id] + 1;
                        next_seq[client_id] += 2;
                    } else {
                        batch_seq = next_seq[client_id]++;
                    }
                } else if (pattern == "dups") {
                    if (uni(rng) < dup_ratio && next_seq[client_id] > 0) {
                        batch_seq = next_seq[client_id] - 1; // duplicate last
                    } else {
                        batch_seq = next_seq[client_id]++;
                    }
                } else {
                    batch_seq = next_seq[client_id]++;
                }

                // Reserve space for messages
                uint8_t* first_msg_ptr = broker_log_ptr;
                // Wrap log region if needed
                if (first_msg_ptr + batch_size * msg_stride > broker_log_end) {
                    first_msg_ptr = broker_log_begin;
                    broker_log_ptr = broker_log_begin;
                }

                // Write messages
                auto* msg = reinterpret_cast<MessageHeader*>(first_msg_ptr);
                for (size_t i = 0; i < batch_size; ++i) {
                    msg->paddedSize = msg_stride;
                    msg->complete = 1; // publish message ready
                    msg = reinterpret_cast<MessageHeader*>(reinterpret_cast<uint8_t*>(msg) + msg_stride);
                }

                // Fill batch header (publish num_msg last)
                batch_header->client_id = static_cast<uint32_t>(client_id);
                batch_header->broker_id = static_cast<uint32_t>(b);
                batch_header->batch_seq = batch_seq;
                batch_header->start_logical_offset = logical_offset_next;
                batch_header->log_idx = static_cast<size_t>(first_msg_ptr - reinterpret_cast<uint8_t*>(region));
                batch_header->total_size = batch_size * msg_stride;
                batch_header->num_msg = static_cast<uint32_t>(batch_size);
#ifdef BUILDING_ORDER_BENCH
                batch_header->publish_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif

                // Advance pointers
                broker_log_ptr += batch_size * msg_stride;
                logical_offset_next += batch_size;
                batch_index++;

                double interval = full_speed.load(std::memory_order_relaxed) ? batch_interval_sec : warmup_batch_interval_sec;
                if (interval > 0.0) {
                    next_release += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(interval));
                    std::this_thread::sleep_until(next_release);
                }
            }
        });
    }

    // Warmup and measurement loop with simple throughput reporting
    auto now = []{ return std::chrono::steady_clock::now(); };
    // Prepare throughput baseline buffer before warmup
    std::vector<size_t> prev_ordered(brokers, 0);
    std::cout << "Warmup for " << warmup_s << "s..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(warmup_s));
    // Reset baselines at measurement start. Switch to full speed only if unpaced.
    if (target_msgs_per_s == 0.0) {
        full_speed.store(true, std::memory_order_relaxed);
    }
    for (int b = 0; b < brokers; ++b) prev_ordered[b] = tinode->offsets[b].ordered;

    // prev_ordered already initialized above

    std::cout << "Entering measurement loop for " << duration_s << " seconds" << std::endl;
    auto start = now();
    size_t start_sum = 0;
    for (int b = 0; b < brokers; ++b) start_sum += tinode->offsets[b].ordered;
    for (int sec = 0; sec < duration_s; ++sec) {
        std::cout << "tick " << (sec+1) << "/" << duration_s << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        size_t sum = 0;
        for (int b = 0; b < brokers; ++b) {
            size_t cur = tinode->offsets[b].ordered;
            size_t delta = 0;
            if (cur >= prev_ordered[b]) delta = cur - prev_ordered[b];
            sum += delta;
            prev_ordered[b] = cur;
        }
        std::cout << "Throughput msgs/s: " << sum << std::endl;
    }
    auto end = now();
    size_t end_sum = 0;
    for (int b = 0; b < brokers; ++b) end_sum += tinode->offsets[b].ordered;
    double avg = 0.0;
    if (duration_s > 0) {
        avg = static_cast<double>(end_sum - start_sum) / static_cast<double>(duration_s);
    }
    std::cout << "Average throughput msgs/s over " << duration_s << "s: " << static_cast<long long>(avg) << std::endl;
    ordering.StopSequencer();
    stop_writers.store(true, std::memory_order_relaxed);
    for (auto& t : writers) t.join();

    if (write_csv || write_thread_csv) {
#ifdef BUILDING_ORDER_BENCH
        std::vector<std::pair<int, MessageOrdering::SequencerThreadStats>> stats;
        ordering.BenchGetStatsSnapshot(stats);
        // Aggregate batch ordering latencies (ns)
        std::vector<uint64_t> all_lat;
        all_lat.reserve(1024);
        for (auto& kv : stats) {
            const auto& s = kv.second;
            all_lat.insert(all_lat.end(), s.batch_order_latency_ns.begin(), s.batch_order_latency_ns.end());
        }
        auto percentile = [&](double q)->uint64_t{
            if (all_lat.empty()) return 0ULL;
            std::sort(all_lat.begin(), all_lat.end());
            size_t idx = static_cast<size_t>(q * (all_lat.size()-1));
            return all_lat[idx];
        };
        uint64_t p50 = percentile(0.50);
        uint64_t p90 = percentile(0.90);
        uint64_t p99 = percentile(0.99);
        // Write per-run CSV summary
        if (write_csv) {
            FILE* f = fopen(csv_path.c_str(), "a");
            if (f) {
                uint64_t total_batches = 0, total_ordered = 0, total_skipped = 0, total_dups = 0;
                uint64_t total_fetch_add = 0, total_claimed_msgs = 0;
                uint64_t total_lock_ns = 0, total_assign_ns = 0;
                for (auto& kv : stats) {
                    const auto& s = kv.second;
                    total_batches += s.num_batches_seen;
                    total_ordered += s.num_batches_ordered;
                    total_skipped += s.num_batches_skipped;
                    total_dups += s.num_duplicates;
                    total_fetch_add += s.atomic_fetch_add_count;
                    total_claimed_msgs += s.atomic_claimed_msgs;
                    total_lock_ns += s.lock_acquire_time_total_ns;
                    total_assign_ns += s.time_in_assign_order_total_ns;
                }
                fprintf(f, "brokers,%d,clients_per_broker,%d,message_size,%zu,batch_size,%zu,pattern,%s,gap_ratio,%.3f,dup_ratio,%.3f,target_msgs_per_s,%.1f,throughput_avg,%.0f,total_batches,%lu,total_ordered,%lu,total_skipped,%lu,total_dups,%lu,atomic_fetch_add,%lu,claimed_msgs,%lu,total_lock_ns,%lu,total_assign_ns,%lu,p50_ns,%llu,p90_ns,%llu,p99_ns,%llu\n",
                        brokers, clients_per_broker, payload_bytes, batch_size, pattern.c_str(), gap_ratio, dup_ratio, target_msgs_per_s, avg,
                        total_batches, total_ordered, total_skipped, total_dups, total_fetch_add, total_claimed_msgs, total_lock_ns, total_assign_ns,
                        (unsigned long long)p50, (unsigned long long)p90, (unsigned long long)p99);
                fclose(f);
            }
        }
        // Write per-thread CSV
        if (write_thread_csv) {
            FILE* f = fopen(thread_csv_path.c_str(), "a");
            if (f) {
                for (auto& kv : stats) {
                    int broker = kv.first;
                    const auto& s = kv.second;
                    fprintf(f, "broker,%d,num_seen,%lu,num_ordered,%lu,num_skipped,%lu,num_dups,%lu,fetch_add,%lu,claimed_msgs,%lu,lock_ns,%lu,assign_ns,%lu\n",
                            broker, s.num_batches_seen, s.num_batches_ordered, s.num_batches_skipped, s.num_duplicates, s.atomic_fetch_add_count, s.atomic_claimed_msgs, s.lock_acquire_time_total_ns, s.time_in_assign_order_total_ns);
                }
                fclose(f);
            }
        }
#endif
    }

    if (verify) {
        size_t total_msgs = 0;
        for (int b = 0; b < brokers; ++b) total_msgs += tinode->offsets[b].ordered;
        std::vector<uint8_t> seen(total_msgs, 0);
        bool ok = true;
        size_t seen_count = 0;
        for (int b = 0; b < brokers; ++b) {
            size_t remaining = tinode->offsets[b].ordered;
            uint8_t* export_ptr = reinterpret_cast<uint8_t*>(region) + tinode->offsets[b].batch_headers_offset;
            // Walk export headers; each export header points to the actual batch via batch_off_to_export
            for (size_t h = 0; remaining > 0 && h < (1ull<<22); ++h) {
                auto* export_bh = reinterpret_cast<BatchHeader*>(export_ptr + h * sizeof(BatchHeader));
                if (export_bh->ordered != 1) continue;
                auto* real_bh = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(export_bh) + export_bh->batch_off_to_export);
                if (real_bh->num_msg == 0) continue;
                uint8_t* msg_ptr = reinterpret_cast<uint8_t*>(region) + real_bh->log_idx;
                for (uint32_t i = 0; i < real_bh->num_msg && remaining > 0; ++i) {
                    auto* mh = reinterpret_cast<MessageHeader*>(msg_ptr);
                    size_t to = mh->total_order;
                    if (to >= total_msgs) { std::cout << "VERIFY FAIL: total_order out of range: " << to << std::endl; ok = false; break; }
                    if (seen[to]) { std::cout << "VERIFY FAIL: duplicate total_order: " << to << std::endl; ok = false; break; }
                    seen[to] = 1;
                    ++seen_count;
                    --remaining;
                    msg_ptr += mh->paddedSize;
                }
                if (!ok) break;
            }
            if (!ok) break;
            if (remaining != 0) { std::cout << "VERIFY FAIL: not enough ordered export headers to cover broker " << b << std::endl; ok = false; break; }
        }
        if (ok && seen_count != total_msgs) { std::cout << "VERIFY FAIL: seen_count=" << seen_count << " total_msgs=" << total_msgs << std::endl; ok = false; }
        if (ok) { for (size_t i = 0; i < total_msgs; ++i) if (!seen[i]) { std::cout << "VERIFY FAIL: missing total_order=" << i << std::endl; ok = false; break; } }
        std::cout << (ok ? "Verification PASSED" : "Verification FAILED") << ". total ordered msgs=" << total_msgs << std::endl;
    }

    std::free(tinode);
    if (!use_real_cxl || region == nullptr) {
        std::free(region);
    }

    return 0;
}


