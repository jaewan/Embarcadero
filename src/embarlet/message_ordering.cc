#include "message_ordering.h"
#ifndef BUILDING_ORDER_BENCH
#include "../cxl_manager/scalog_local_sequencer.h"
#endif
#include "topic.h"
#include "../common/performance_utils.h"
#include <glog/logging.h>
#include <chrono>
#include <thread>

namespace Embarcadero {

MessageOrdering::MessageOrdering(void* cxl_addr, TInode* tinode, int broker_id)
    : cxl_addr_(cxl_addr),
      tinode_(tinode),
      broker_id_(broker_id) {}

MessageOrdering::~MessageOrdering() {
    StopSequencer();
}

void MessageOrdering::StartSequencer(SequencerType seq_type, int order, const std::string& topic_name) {
    // Only head node runs sequencer
    if (broker_id_ != 0) {
        return;
    }

    switch (seq_type) {
        case KAFKA:
        case EMBARCADERO:
            if (order == 1) {
                LOG(ERROR) << "Sequencer 1 is not ported yet from cxl_manager";
            } else if (order == 2) {
                LOG(ERROR) << "Sequencer 2 is not ported yet";
            } else if (order == 3) {
                LOG(ERROR) << "Sequencer 3 is not ported yet";
            } else if (order == 4) {
                sequencer_thread_ = std::thread(&MessageOrdering::Sequencer4, this);
            } else if (order == 5) {
                sequencer_thread_ = std::thread(&MessageOrdering::Sequencer5, this);
            }
            break;
        case SCALOG:
            if (order == 1) {
                sequencer_thread_ = std::thread(&MessageOrdering::StartScalogLocalSequencer, this, topic_name);
            } else if (order == 2) {
                LOG(ERROR) << "Order is set 2 at scalog";
            }
            break;
        case CORFU:
            if (order == 0 || order == 4) {
                VLOG(3) << "Order " << order << 
                    " for Corfu is right as messages are written ordered. Combiner combining is enough";
            } else {
                LOG(ERROR) << "Wrong Order is set for corfu " << order;
            }
            break;
        default:
            LOG(ERROR) << "Unknown sequencer:" << seq_type;
            break;
    }
}

void MessageOrdering::StopSequencer() {
    stop_threads_ = true;
    if (sequencer_thread_.joinable()) {
        sequencer_thread_.join();
    }
}

void MessageOrdering::StartScalogLocalSequencer(const std::string& topic_name) {
#ifdef BUILDING_ORDER_BENCH
    (void)topic_name;
    return;
#else
    BatchHeader* batch_header = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
    Scalog::ScalogLocalSequencer scalog_local_sequencer(tinode_, broker_id_, cxl_addr_, topic_name.c_str(), batch_header);
    bool stop = stop_threads_.load(std::memory_order_relaxed);
    scalog_local_sequencer.SendLocalCut(topic_name.c_str(), stop);
#endif
}

void MessageOrdering::Sequencer4() {
    absl::btree_set<int> registered_brokers;
    if (get_registered_brokers_callback_) {
        get_registered_brokers_callback_(registered_brokers, tinode_);
    }

    global_seq_ = 0;

    std::vector<std::thread> sequencer4_threads;
    for (int broker_id : registered_brokers) {
        sequencer4_threads.emplace_back(
            [this, broker_id]() {
#ifdef BUILDING_ORDER_BENCH
                // Optional pinning for sequencer threads
                if (!bench_seq_cpus_.empty()) {
                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    for (int cpu : bench_seq_cpus_) CPU_SET(cpu, &cpuset);
                    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
                }
#endif
                BrokerScannerWorker(broker_id);
            }
        );
    }

    for (auto& t : sequencer4_threads) {
        while (!t.joinable()) {
            std::this_thread::yield();
        }
        t.join();
    }
}

void MessageOrdering::BrokerScannerWorker(int broker_id) {
    // Wait until tinode of the broker is initialized
    while (tinode_->offsets[broker_id].log_offset == 0) {
        std::this_thread::yield();
    }

    BatchHeader* ring_start_default = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
    BatchHeader* current_batch_header = ring_start_default;
    BatchHeader* ring_end = nullptr;
    // Export header ring (where we publish ordered entries)
    BatchHeader* export_ring_start = ring_start_default;
    BatchHeader* export_ring_end = nullptr;
#ifdef BUILDING_ORDER_BENCH
    {
        absl::MutexLock l(&bench_ring_mu_);
        auto it = bench_batch_header_rings_.find(broker_id);
        if (it != bench_batch_header_rings_.end() && it->second.start != nullptr && it->second.num > 0) {
            ring_start_default = it->second.start;
            current_batch_header = ring_start_default;
            ring_end = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(ring_start_default) + it->second.num * sizeof(BatchHeader));
        }
        auto it2 = bench_export_header_rings_.find(broker_id);
        if (it2 != bench_export_header_rings_.end() && it2->second.start != nullptr && it2->second.num > 0) {
            export_ring_start = it2->second.start;
            export_ring_end = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(export_ring_start) + it2->second.num * sizeof(BatchHeader));
        } else {
            // Default export ring matches batch header ring if not provided
            export_ring_start = ring_start_default;
            export_ring_end = ring_end;
        }
    }
#endif
    if (!current_batch_header) {
        LOG(ERROR) << "Scanner [Broker " << broker_id << "]: Failed to calculate batch header start address.";
        return;
    }
    BatchHeader* header_for_sub = export_ring_start;
#ifdef BUILDING_ORDER_BENCH
    {
        absl::MutexLock l(&bench_stats_mu_);
        (void)bench_stats_by_broker_[broker_id];
    }
#endif

    absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>> skipped_batches;

    while (!stop_threads_) {
        volatile size_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;

        if (num_msg_check == 0 || current_batch_header->log_idx == 0) {
            if (!ProcessSkipped(skipped_batches, header_for_sub)) {
                std::this_thread::yield();
            }
            // advance to next header in the ring even if not ready
            current_batch_header = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
#ifdef BUILDING_ORDER_BENCH
            if (ring_end && current_batch_header >= ring_end) current_batch_header = ring_start_default;
#endif
            continue;
        }

        BatchHeader* header_to_process = current_batch_header;
#ifdef BUILDING_ORDER_BENCH
        // Sanity: skip if generator shows uninitialized header (should be fine if zero)
        (void)header_to_process;
#endif
#ifdef BUILDING_ORDER_BENCH
        {
            absl::MutexLock l(&bench_stats_mu_);
            bench_stats_by_broker_[broker_id].num_batches_seen++;
        }
#endif
        size_t client_id = current_batch_header->client_id;
        size_t client_key = (static_cast<size_t>(broker_id) << 32) | client_id;
        size_t batch_seq = current_batch_header->batch_seq;
        bool ready_to_order = false;
        size_t expected_seq = 0;
        size_t start_total_order = 0;
        bool skip_batch = false;

        // Per-client sharded critical section with atomic global range reservation
        ClientState* state = GetOrCreateClientState(client_key);
        {
            auto lock_start = std::chrono::steady_clock::now();
            absl::MutexLock l(&state->mu);
            auto lock_end = std::chrono::steady_clock::now();
#ifdef BUILDING_ORDER_BENCH
            {
                absl::MutexLock l2(&bench_stats_mu_);
                bench_stats_by_broker_[broker_id].lock_acquire_time_total_ns +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(lock_end - lock_start).count();
            }
#endif
            expected_seq = state->expected_seq;
            if (batch_seq == expected_seq) {
                start_total_order = global_seq_.fetch_add(header_to_process->num_msg, std::memory_order_relaxed);
#ifdef BUILDING_ORDER_BENCH
                {
                    absl::MutexLock l2(&bench_stats_mu_);
                    auto &s = bench_stats_by_broker_[broker_id];
                    s.atomic_fetch_add_count++;
                    s.atomic_claimed_msgs += header_to_process->num_msg;
                }
#endif
                state->expected_seq = expected_seq + 1;
                ready_to_order = true;
            } else if (batch_seq > expected_seq) {
                skip_batch = true;
#ifdef BUILDING_ORDER_BENCH
                {
                    absl::MutexLock l2(&bench_stats_mu_);
                    bench_stats_by_broker_[broker_id].num_batches_skipped++;
                }
#endif
            } else {
                LOG(WARNING) << "Scanner [B" << broker_id << "]: Duplicate/old batch seq "
                             << batch_seq << " detected from client " << client_id
                             << " (expected " << expected_seq << ") - skipping";
#ifdef BUILDING_ORDER_BENCH
                {
                    absl::MutexLock l2(&bench_stats_mu_);
                    bench_stats_by_broker_[broker_id].num_duplicates++;
                }
#endif
                // Skip this duplicate/old batch to avoid infinite loop
                skip_batch = true;
            }
        }

        if (skip_batch) {
            skipped_batches[client_key][batch_seq] = header_to_process;
            VLOG(3) << "Scanner [B" << broker_id << "]: Skipping batch from client " << client_id 
                    << ", batch_seq=" << batch_seq << ", expected=" << expected_seq
                    << ", num_msg=" << header_to_process->num_msg;
        }

        if (ready_to_order) {
#ifdef BUILDING_ORDER_BENCH
            auto t0 = std::chrono::steady_clock::now();
            // Record end-to-end batch ordering latency from publish to order
            {
                absl::MutexLock l2(&bench_stats_mu_);
                uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count();
                uint64_t pub_ns = header_to_process->publish_ts_ns;
                if (pub_ns != 0 && now_ns >= pub_ns) {
                    bench_stats_by_broker_[broker_id].batch_order_latency_ns.push_back(now_ns - pub_ns);
                }
            }
#endif
            AssignOrder(header_to_process, start_total_order, header_for_sub);
#ifdef BUILDING_ORDER_BENCH
            auto t1 = std::chrono::steady_clock::now();
            {
                absl::MutexLock l2(&bench_stats_mu_);
                auto &s = bench_stats_by_broker_[broker_id];
                s.time_in_assign_order_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                s.num_batches_ordered++;
            }
#endif
            VLOG(3) << "Scanner [B" << broker_id << "]: Ordered batch from client " << client_id 
                    << ", batch_seq=" << batch_seq << ", total_order=[" << start_total_order 
                    << ", " << (start_total_order + header_to_process->num_msg) << ")";
            ProcessSkipped(skipped_batches, header_for_sub);
        }

        current_batch_header = reinterpret_cast<BatchHeader*>(
            reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader)
        );
#ifdef BUILDING_ORDER_BENCH
        if (ring_end && current_batch_header >= ring_end) current_batch_header = ring_start_default;
#endif
    }
}

bool MessageOrdering::ProcessSkipped(
    absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>>& skipped_batches,
    BatchHeader*& header_for_sub) {
    
    bool processed_any = false;
    auto client_skipped_it = skipped_batches.begin();
    while (client_skipped_it != skipped_batches.end()) {
        size_t client_key = client_skipped_it->first;
        auto& client_skipped_map = client_skipped_it->second;

        size_t start_total_order;
        bool batch_processed;
        do {
            batch_processed = false;
            size_t expected_seq;
            BatchHeader* batch_header = nullptr;
            auto batch_it = client_skipped_map.end();
            {
                ClientState* state = GetOrCreateClientState(client_key);
                auto lock_start = std::chrono::steady_clock::now();
                absl::MutexLock l(&state->mu);
                auto lock_end = std::chrono::steady_clock::now();
                expected_seq = state->expected_seq;
                batch_it = client_skipped_map.find(expected_seq);
                if (batch_it != client_skipped_map.end()) {
                    batch_header = batch_it->second;
                    start_total_order = global_seq_.fetch_add(batch_header->num_msg, std::memory_order_relaxed);
#ifdef BUILDING_ORDER_BENCH
                    {
                        absl::MutexLock l2(&bench_stats_mu_);
                        auto& s = bench_stats_by_broker_[static_cast<int>(batch_header->broker_id)];
                        s.lock_acquire_time_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(lock_end - lock_start).count();
                        s.atomic_fetch_add_count++;
                        s.atomic_claimed_msgs += batch_header->num_msg;
                    }
#endif
                    state->expected_seq = expected_seq + 1;
                    batch_processed = true;
                    processed_any = true;
                    VLOG(4) << "ProcessSkipped [B?]: ClientKey " << client_key 
                            << ", processing skipped batch " << expected_seq 
                            << ", reserving seq [" << start_total_order << ", " << (start_total_order + batch_header->num_msg) << ")";
                } else {
                    // no ready skipped batch
                }
            }
            if (batch_processed && batch_header) {
                client_skipped_map.erase(batch_it);
                auto t0 = std::chrono::steady_clock::now();
                AssignOrder(batch_header, start_total_order, header_for_sub);
#ifdef BUILDING_ORDER_BENCH
                auto t1 = std::chrono::steady_clock::now();
                {
                    absl::MutexLock l(&bench_stats_mu_);
                    auto& s = bench_stats_by_broker_[static_cast<int>(batch_header->broker_id)];
                    s.time_in_assign_order_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    s.num_batches_ordered++;
                }
#endif
            }
        } while (batch_processed && !client_skipped_map.empty());

        if (client_skipped_map.empty()) {
            skipped_batches.erase(client_skipped_it++);
        } else {
            ++client_skipped_it;
        }
    }
    return processed_any;
}

void MessageOrdering::AssignOrder(BatchHeader* batch_to_order, size_t start_total_order, BatchHeader*& header_for_sub) {
    int broker = batch_to_order->broker_id;

    size_t num_messages = batch_to_order->num_msg;
    if (num_messages == 0) {
        LOG(WARNING) << "!!!! Orderer: Dequeued batch with zero messages. Skipping !!!";
        return;
    }

    // Sequencer 4: Keep per-message completion checking (batch_complete not set by network thread)

    MessageHeader* msg_header = reinterpret_cast<MessageHeader*>(
        batch_to_order->log_idx + reinterpret_cast<uint8_t*>(cxl_addr_)
    );
    if (!msg_header) {
        LOG(ERROR) << "Orderer: Failed to calculate message address for logical offset " << batch_to_order->log_idx;
        return;
    }
    size_t seq = start_total_order;
    batch_to_order->total_order = seq;

    size_t logical_offset = batch_to_order->start_logical_offset;

    for (size_t i = 0; i < num_messages; ++i) {
        if (!bench_headers_only_) {
            // Sequencer 4: Wait for each message to be complete (network thread doesn't set batch_complete)
            while (msg_header->paddedSize == 0) {
                if (stop_threads_) return;
                std::this_thread::yield();
            }

            size_t current_padded_size = msg_header->paddedSize;

            msg_header->logical_offset = logical_offset;
            logical_offset++;
            msg_header->total_order = seq;
            seq++;
            msg_header->next_msg_diff = current_padded_size;

            msg_header = reinterpret_cast<MessageHeader*>(
                reinterpret_cast<uint8_t*>(msg_header) + current_padded_size
            );
        } else {
            // Headers-only: skip touching per-message payloads/headers aside from advancing logical/seq
            logical_offset += 1;
            seq += 1;
        }

        // Update ordered once per message
        // ordered is read without explicit synchronization by the benchmark thread.
        // Use relaxed atomic-like semantics by writing through a volatile int field; ensure monotonic increment.
        // [[CRITICAL_FIX: Invalidate cache BEFORE reading in RMW on non-coherent CXL]]
        // On non-coherent CXL, sequencer reads stale cached value, causing lost updates
        volatile uint64_t* ordered_ptr = &tinode_->offsets[broker].ordered;
        Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
            reinterpret_cast<const volatile void*>(ordered_ptr)));
        Embarcadero::CXL::load_fence();
        tinode_->offsets[broker].ordered = tinode_->offsets[broker].ordered + 1;
    }
    header_for_sub->batch_off_to_export = (reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(header_for_sub));
    header_for_sub->ordered = 1;
#ifdef BUILDING_ORDER_BENCH
    // Mark input header as consumed to avoid reprocessing after ring wrap
    batch_to_order->num_msg = 0;
    batch_to_order->log_idx = 0;
#endif
    header_for_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header_for_sub) + sizeof(BatchHeader));
#ifdef BUILDING_ORDER_BENCH
    // Wrap export header pointer if we reached the end of its ring
    {
        absl::MutexLock l(&bench_ring_mu_);
        auto it = bench_export_header_rings_.find(broker);
        if (it != bench_export_header_rings_.end() && it->second.start) {
            BatchHeader* start = it->second.start;
            BatchHeader* end = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(start) + it->second.num * sizeof(BatchHeader));
            if (end && header_for_sub >= end) header_for_sub = start;
        }
    }
#endif
}

// MessageCombiner implementation
MessageCombiner::MessageCombiner(void* cxl_addr,
                                 void* first_message_addr,
                                 TInode* tinode,
                                 TInode* replica_tinode,
                                 int broker_id)
    : cxl_addr_(cxl_addr),
      first_message_addr_(first_message_addr),
      tinode_(tinode),
      replica_tinode_(replica_tinode),
      broker_id_(broker_id) {}

MessageCombiner::~MessageCombiner() {
    Stop();
}

void MessageCombiner::Start() {
    combiner_thread_ = std::thread(&MessageCombiner::CombinerThread, this);
}

void MessageCombiner::Stop() {
    stop_thread_ = true;
    if (combiner_thread_.joinable()) {
        combiner_thread_.join();
    }
}

void MessageCombiner::CombinerThread() {
    // Use known cacheline size from topic.h
    void* segment_header = reinterpret_cast<uint8_t*>(first_message_addr_) - CACHELINE_SIZE;
    MessageHeader* header = reinterpret_cast<MessageHeader*>(first_message_addr_);

    while (!stop_thread_) {
        // NOTE: Legacy MessageCombiner - complete flag removed
        // This is dead code not used by current sequencers
        // For batch-level completion, we would need batch header access here

#ifdef MULTISEGMENT
        if (header->next_msg_diff != 0) {
            header = reinterpret_cast<MessageHeader*>(
                reinterpret_cast<uint8_t*>(header) + header->next_msg_diff);
            segment_header = reinterpret_cast<uint8_t*>(header) - CACHELINE_SIZE;
            continue;
        }
#endif

        header->segment_header = segment_header;
        header->logical_offset = logical_offset_;
        header->next_msg_diff = header->paddedSize;

        UpdateTInodeWritten(
            logical_offset_,
            static_cast<unsigned long long int>(
                reinterpret_cast<uint8_t*>(header) - reinterpret_cast<uint8_t*>(cxl_addr_))
        );

        *reinterpret_cast<unsigned long long int*>(segment_header) =
            static_cast<unsigned long long int>(
                reinterpret_cast<uint8_t*>(header) - reinterpret_cast<uint8_t*>(segment_header)
            );

        written_logical_offset_.store(logical_offset_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        written_physical_addr_ = reinterpret_cast<void*>(header);

        header = reinterpret_cast<MessageHeader*>(
            reinterpret_cast<uint8_t*>(header) + header->next_msg_diff);
        logical_offset_++;
    }
}

void MessageCombiner::UpdateTInodeWritten(size_t written, size_t written_addr) {
    if (tinode_->replicate_tinode && replica_tinode_) {
        replica_tinode_->offsets[broker_id_].written = written;
        replica_tinode_->offsets[broker_id_].written_addr = written_addr;
    }

    tinode_->offsets[broker_id_].written = written;
    tinode_->offsets[broker_id_].written_addr = written_addr;
}

// Sequencer 5: Batch-level sequencer (copy of Sequencer 4 without message-level operations)
void MessageOrdering::Sequencer5() {
    absl::btree_set<int> registered_brokers;
    if (get_registered_brokers_callback_) {
        get_registered_brokers_callback_(registered_brokers, tinode_);
    }

    global_seq_ = 0;

    std::vector<std::thread> sequencer5_threads;
    for (int broker_id : registered_brokers) {
        sequencer5_threads.emplace_back(
            [this, broker_id]() {
#ifdef BUILDING_ORDER_BENCH
                // Optional pinning for sequencer threads
                if (!bench_seq_cpus_.empty()) {
                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    for (int cpu : bench_seq_cpus_) CPU_SET(cpu, &cpuset);
                    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
                }
#endif
                BrokerScannerWorker5(broker_id);
            }
        );
    }

    for (auto& t : sequencer5_threads) {
        while (!t.joinable()) {
            std::this_thread::yield();
        }
        t.join();
    }
}

void MessageOrdering::BrokerScannerWorker5(int broker_id) {
    // Wait until tinode of the broker is initialized
    while (tinode_->offsets[broker_id].log_offset == 0) {
        std::this_thread::yield();
    }

    BatchHeader* ring_start_default = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
    BatchHeader* current_batch_header = ring_start_default;
    BatchHeader* ring_end = nullptr;
    // Export header ring (where we publish ordered entries)
    BatchHeader* export_ring_start = ring_start_default;
    BatchHeader* export_ring_end = nullptr;
#ifdef BUILDING_ORDER_BENCH
    {
        absl::MutexLock l(&bench_ring_mu_);
        auto it = bench_batch_header_rings_.find(broker_id);
        if (it != bench_batch_header_rings_.end() && it->second.start != nullptr && it->second.num > 0) {
            ring_start_default = it->second.start;
            current_batch_header = ring_start_default;
            ring_end = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(ring_start_default) + it->second.num * sizeof(BatchHeader));
        }
        auto it2 = bench_export_header_rings_.find(broker_id);
        if (it2 != bench_export_header_rings_.end() && it2->second.start != nullptr && it2->second.num > 0) {
            export_ring_start = it2->second.start;
            export_ring_end = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(export_ring_start) + it2->second.num * sizeof(BatchHeader));
        } else {
            // Default export ring matches batch header ring if not provided
            export_ring_start = ring_start_default;
            export_ring_end = ring_end;
        }
    }
#endif
    if (!current_batch_header) {
        LOG(ERROR) << "Scanner5 [Broker " << broker_id << "]: Failed to calculate batch header start address.";
        return;
    }
    BatchHeader* header_for_sub = export_ring_start;
#ifdef BUILDING_ORDER_BENCH
    {
        absl::MutexLock l(&bench_stats_mu_);
        (void)bench_stats_by_broker_[broker_id];
    }
#endif

    absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>> skipped_batches;

    while (!stop_threads_) {
        volatile size_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;

        if (num_msg_check == 0 || current_batch_header->log_idx == 0) {
            if (!ProcessSkipped5(skipped_batches, header_for_sub)) {
                // OPTIMIZATION: Reduce yield frequency for better sequencer performance
                static size_t yield_counter = 0;
                if (++yield_counter % 10 == 0) {
                    std::this_thread::yield();
                }
            }
            // advance to next header in the ring even if not ready
            current_batch_header = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
#ifdef BUILDING_ORDER_BENCH
            if (ring_end && current_batch_header >= ring_end) current_batch_header = ring_start_default;
#endif
            continue;
        }

        BatchHeader* header_to_process = current_batch_header;
#ifdef BUILDING_ORDER_BENCH
        {
            absl::MutexLock l(&bench_stats_mu_);
            bench_stats_by_broker_[broker_id].num_batches_seen++;
        }
#endif
        size_t client_id = current_batch_header->client_id;
        size_t client_key = (static_cast<size_t>(broker_id) << 32) | client_id;
        size_t batch_seq = current_batch_header->batch_seq;
        bool ready_to_order = false;
        size_t expected_seq = 0;
        size_t start_total_order = 0;
        bool skip_batch = false;

        // Per-client sharded critical section with atomic global range reservation
        ClientState* state = GetOrCreateClientState(client_key);
        {
            auto lock_start = std::chrono::steady_clock::now();
            absl::MutexLock l(&state->mu);
            auto lock_end = std::chrono::steady_clock::now();
#ifdef BUILDING_ORDER_BENCH
            {
                absl::MutexLock l2(&bench_stats_mu_);
                bench_stats_by_broker_[broker_id].lock_acquire_time_total_ns +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(lock_end - lock_start).count();
            }
#endif
            expected_seq = state->expected_seq;
            if (batch_seq == expected_seq) {
                start_total_order = global_seq_.fetch_add(header_to_process->num_msg, std::memory_order_relaxed);
#ifdef BUILDING_ORDER_BENCH
                {
                    absl::MutexLock l2(&bench_stats_mu_);
                    auto &s = bench_stats_by_broker_[broker_id];
                    s.atomic_fetch_add_count++;
                    s.atomic_claimed_msgs += header_to_process->num_msg;
                }
#endif
                state->expected_seq = expected_seq + 1;
                ready_to_order = true;
            } else if (batch_seq > expected_seq) {
                skip_batch = true;
#ifdef BUILDING_ORDER_BENCH
                {
                    absl::MutexLock l2(&bench_stats_mu_);
                    bench_stats_by_broker_[broker_id].num_batches_skipped++;
                }
#endif
            } else {
                LOG(WARNING) << "Scanner5 [B" << broker_id << "]: Duplicate/old batch seq "
                             << batch_seq << " detected from client " << client_id
                             << " (expected " << expected_seq << ") - skipping";
#ifdef BUILDING_ORDER_BENCH
                {
                    absl::MutexLock l2(&bench_stats_mu_);
                    bench_stats_by_broker_[broker_id].num_duplicates++;
                }
#endif
                skip_batch = true;
            }
        }

        if (skip_batch) {
            skipped_batches[client_key][batch_seq] = header_to_process;
            VLOG(3) << "Scanner5 [B" << broker_id << "]: Skipping batch from client " << client_id 
                    << ", batch_seq=" << batch_seq << ", expected=" << expected_seq
                    << ", num_msg=" << header_to_process->num_msg;
        }

        if (ready_to_order) {
#ifdef BUILDING_ORDER_BENCH
            auto t0 = std::chrono::steady_clock::now();
            // Record end-to-end batch ordering latency from publish to order
            {
                absl::MutexLock l2(&bench_stats_mu_);
                uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count();
                uint64_t pub_ns = header_to_process->publish_ts_ns;
                if (pub_ns != 0 && now_ns >= pub_ns) {
                    bench_stats_by_broker_[broker_id].batch_order_latency_ns.push_back(now_ns - pub_ns);
                }
            }
#endif
            AssignOrder5(header_to_process, start_total_order, header_for_sub);
#ifdef BUILDING_ORDER_BENCH
            auto t1 = std::chrono::steady_clock::now();
            {
                absl::MutexLock l2(&bench_stats_mu_);
                auto &s = bench_stats_by_broker_[broker_id];
                s.time_in_assign_order_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                s.num_batches_ordered++;
            }
#endif
            VLOG(3) << "Scanner5 [B" << broker_id << "]: Ordered batch from client " << client_id 
                    << ", batch_seq=" << batch_seq << ", total_order=[" << start_total_order 
                    << ", " << (start_total_order + header_to_process->num_msg) << ")";
            ProcessSkipped5(skipped_batches, header_for_sub);
        }

        current_batch_header = reinterpret_cast<BatchHeader*>(
            reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader)
        );
#ifdef BUILDING_ORDER_BENCH
        if (ring_end && current_batch_header >= ring_end) current_batch_header = ring_start_default;
#endif
    }
}

bool MessageOrdering::ProcessSkipped5(
    absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>>& skipped_batches,
    BatchHeader*& header_for_sub) {
    
    bool processed_any = false;
    auto client_skipped_it = skipped_batches.begin();
    while (client_skipped_it != skipped_batches.end()) {
        size_t client_key = client_skipped_it->first;
        auto& client_skipped_map = client_skipped_it->second;

        size_t start_total_order;
        bool batch_processed;
        do {
            batch_processed = false;
            size_t expected_seq;
            BatchHeader* batch_header = nullptr;
            auto batch_it = client_skipped_map.end();
            {
                ClientState* state = GetOrCreateClientState(client_key);
                auto lock_start = std::chrono::steady_clock::now();
                absl::MutexLock l(&state->mu);
                auto lock_end = std::chrono::steady_clock::now();
                expected_seq = state->expected_seq;
                batch_it = client_skipped_map.find(expected_seq);
                if (batch_it != client_skipped_map.end()) {
                    batch_header = batch_it->second;
                    start_total_order = global_seq_.fetch_add(batch_header->num_msg, std::memory_order_relaxed);
#ifdef BUILDING_ORDER_BENCH
                    {
                        absl::MutexLock l2(&bench_stats_mu_);
                        auto& s = bench_stats_by_broker_[static_cast<int>(batch_header->broker_id)];
                        s.lock_acquire_time_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(lock_end - lock_start).count();
                        s.atomic_fetch_add_count++;
                        s.atomic_claimed_msgs += batch_header->num_msg;
                    }
#endif
                    state->expected_seq = expected_seq + 1;
                    batch_processed = true;
                    processed_any = true;
                    VLOG(4) << "ProcessSkipped5 [B?]: ClientKey " << client_key 
                            << ", processing skipped batch " << expected_seq 
                            << ", reserving seq [" << start_total_order << ", " << (start_total_order + batch_header->num_msg) << ")";
                }
            }
            if (batch_processed && batch_header) {
                client_skipped_map.erase(batch_it);
                auto t0 = std::chrono::steady_clock::now();
                AssignOrder5(batch_header, start_total_order, header_for_sub);
#ifdef BUILDING_ORDER_BENCH
                auto t1 = std::chrono::steady_clock::now();
                {
                    absl::MutexLock l(&bench_stats_mu_);
                    auto& s = bench_stats_by_broker_[static_cast<int>(batch_header->broker_id)];
                    s.time_in_assign_order_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    s.num_batches_ordered++;
                }
#endif
            }
        } while (batch_processed && !client_skipped_map.empty());

        if (client_skipped_map.empty()) {
            skipped_batches.erase(client_skipped_it++);
        } else {
            ++client_skipped_it;
        }
    }
    return processed_any;
}

void MessageOrdering::AssignOrder5(BatchHeader* batch_to_order, size_t start_total_order, BatchHeader*& header_for_sub) {
    int broker = batch_to_order->broker_id;

    size_t num_messages = batch_to_order->num_msg;
    if (num_messages == 0) {
        LOG(WARNING) << "!!!! Orderer5: Dequeued batch with zero messages. Skipping !!!";
        return;
    }

    // Batch-level ordering only - no message-level operations
    batch_to_order->total_order = start_total_order;

    // Update ordered count by the number of messages in the batch
    // This maintains compatibility with existing read path
    // [[CRITICAL_FIX: Invalidate cache BEFORE reading in RMW on non-coherent CXL]]
    // On non-coherent CXL, sequencer reads stale cached value, causing lost updates
    volatile uint64_t* ordered_ptr = &tinode_->offsets[broker].ordered;
    Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
        reinterpret_cast<const volatile void*>(ordered_ptr)));
    Embarcadero::CXL::load_fence();
    tinode_->offsets[broker].ordered = tinode_->offsets[broker].ordered + num_messages;

    // Set up export chain (GOI equivalent)
    header_for_sub->batch_off_to_export = (reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(header_for_sub));
    header_for_sub->ordered = 1;

#ifdef BUILDING_ORDER_BENCH
    // Mark input header as consumed to avoid reprocessing after ring wrap
    batch_to_order->num_msg = 0;
    batch_to_order->log_idx = 0;
#endif

    header_for_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header_for_sub) + sizeof(BatchHeader));

#ifdef BUILDING_ORDER_BENCH
    // Wrap export header pointer if we reached the end of its ring
    {
        absl::MutexLock l(&bench_ring_mu_);
        auto it = bench_export_header_rings_.find(broker);
        if (it != bench_export_header_rings_.end() && it->second.start) {
            BatchHeader* start = it->second.start;
            BatchHeader* end = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(start) + it->second.num * sizeof(BatchHeader));
            if (end && header_for_sub >= end) header_for_sub = start;
        }
    }
#endif

    VLOG(3) << "Orderer5: Assigned batch-level order " << start_total_order 
            << " to batch with " << num_messages << " messages from broker " << broker;
}

} // namespace Embarcadero
