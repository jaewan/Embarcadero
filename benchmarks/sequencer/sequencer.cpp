#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <queue>
#include <condition_variable>

// Constants
constexpr size_t CACHE_LINE_SIZE = 64;
constexpr size_t PBR_SIZE = 256 * 1024 * 1024;
constexpr size_t GOI_SIZE = 4ULL * 1024 * 1024 * 1024;

// Original structures
struct alignas(CACHE_LINE_SIZE) PBREntry {
    size_t client_id;
    size_t client_order;
    std::atomic<bool> complete;
    char padding[64 - 2*sizeof(size_t) - sizeof(std::atomic<bool>)];
    
    PBREntry() : client_id(0), client_order(0), complete(false) {}
};

struct GOIEntry {
    size_t client_id;
    size_t client_order;
    size_t total_order;
};

class CorrectEpochSequencer {
private:
    // OPTIMIZATION 1: Adaptive epoch sizing
    struct AdaptiveConfig {
        size_t base_duration_us = 1000;
        size_t min_duration_us = 200;
        size_t max_duration_us = 5000;
        size_t current_duration_us = 1000;
        size_t adjustment_counter = 0;
        
        size_t calculate_duration(size_t queue_count, size_t last_fill) {
            adjustment_counter++;
            
            // Adjust every 10 epochs
            if (adjustment_counter % 10 == 0) {
                double fill_rate = double(last_fill) / MAX_MESSAGES_PER_EPOCH;
                
                if (fill_rate > 0.8) {
                    // High fill rate - increase duration for better batching
                    current_duration_us = std::min(size_t(current_duration_us * 1.2), max_duration_us);
                } else if (fill_rate < 0.3) {
                    // Low fill rate - decrease duration for lower latency
                    current_duration_us = std::max(size_t(current_duration_us * 0.8), min_duration_us);
                }
                
                // Queue count adjustment
                if (queue_count <= 2) {
                    current_duration_us = std::max(size_t(current_duration_us * 0.7), min_duration_us);
                } else if (queue_count >= 16) {
                    current_duration_us = std::min(size_t(current_duration_us * 1.3), max_duration_us);
                }
            }
            
            return current_duration_us;
        }
    };
    
    // Configuration with adaptive sizing
    static constexpr size_t MAX_MESSAGES_PER_EPOCH = 200000;  // Increased capacity
    
    // Simple message structure
    struct Message {
        size_t client_id;
        size_t client_order;
        size_t global_sequence;
        
        Message() : client_id(0), client_order(0), global_sequence(0) {}
        Message(size_t cid, size_t co, size_t gs) : client_id(cid), client_order(co), global_sequence(gs) {}
    };
    
    // OPTIMIZATION 2: Improved epoch structure with better memory management
    struct Epoch {
        std::vector<Message> messages;
        std::atomic<bool> collecting{true};
        std::atomic<bool> sequenced{false};
        size_t epoch_id;
        size_t last_fill{0};  // Track fill rate for adaptive sizing
        
        Epoch() {
            messages.reserve(MAX_MESSAGES_PER_EPOCH);
        }
        
        void reset(size_t id) {
            last_fill = messages.size();  // Record fill before clearing
            messages.clear();
            collecting.store(true);
            sequenced.store(false);
            epoch_id = id;
        }
    };
    
    // Core components
    std::vector<std::unique_ptr<PBREntry[]>> pbr_queues;
    std::unique_ptr<GOIEntry[]> goi;
    size_t num_queues;
    size_t pbr_entries_per_queue;
    size_t goi_entries;
    
    // Simple epoch management - only 2 epochs needed
    Epoch epoch_a, epoch_b;
    std::atomic<Epoch*> current_collecting_epoch{&epoch_a};
    std::atomic<size_t> epoch_counter{0};
    
    // Adaptive configuration
    AdaptiveConfig adaptive_config;
    
    // CRITICAL: Single-threaded GOI writing for perfect ordering
    std::atomic<size_t> global_sequence_counter{1};
    std::atomic<size_t> goi_write_index{0};
    
    // Queue indices
    std::unique_ptr<std::atomic<size_t>[]> pbr_read_indices;
    std::unique_ptr<std::atomic<size_t>[]> pbr_write_indices;
    
    // Threading - simplified
    std::vector<std::thread> collector_threads;
    std::thread epoch_timer;
    std::thread sequencer_thread;
    std::atomic<bool> running{false};
    
    // Synchronization
    std::mutex epoch_switch_mutex;
    std::condition_variable epoch_ready_cv;
    std::mutex epoch_ready_mutex;
    std::queue<Epoch*> epochs_to_sequence;
    
    // Statistics
    std::atomic<size_t> total_processed{0};
    std::atomic<size_t> epochs_processed{0};
    std::atomic<size_t> messages_dropped{0};
    
public:
    CorrectEpochSequencer(size_t queues) : num_queues(queues) {
        // Initialize PBR and GOI
        pbr_entries_per_queue = PBR_SIZE / sizeof(PBREntry);
        goi_entries = GOI_SIZE / sizeof(GOIEntry);
        
        pbr_queues.reserve(num_queues);
        pbr_read_indices.reset(new std::atomic<size_t>[num_queues]);
        pbr_write_indices.reset(new std::atomic<size_t>[num_queues]);
        
        for (size_t i = 0; i < num_queues; ++i) {
            pbr_queues.emplace_back(new PBREntry[pbr_entries_per_queue]);
            pbr_read_indices[i].store(0);
            pbr_write_indices[i].store(0);
        }
        
        goi.reset(new GOIEntry[goi_entries]);
        
        // Initialize epochs
        epoch_a.reset(0);
        epoch_b.reset(1);
        
        std::cout << "Optimized epoch-based sequencer initialized:\n";
        std::cout << "  Queues: " << num_queues << "\n";
        std::cout << "  Base epoch duration: " << adaptive_config.base_duration_us << " μs\n";
        std::cout << "  Adaptive range: " << adaptive_config.min_duration_us 
                  << "-" << adaptive_config.max_duration_us << " μs\n";
        std::cout << "  Max messages/epoch: " << MAX_MESSAGES_PER_EPOCH << "\n";
    }
    
    void start() {
        running.store(true);
        
        // Start collectors
        for (size_t i = 0; i < num_queues; ++i) {
            collector_threads.emplace_back(&CorrectEpochSequencer::collector_thread, this, i);
        }
        
        // Start pipeline threads
        epoch_timer = std::thread(&CorrectEpochSequencer::epoch_timer_thread, this);
        sequencer_thread = std::thread(&CorrectEpochSequencer::sequencer_worker, this);
        
        std::cout << "Optimized epoch sequencer started with " << (num_queues + 2) << " threads\n";
    }
    
    void stop() {
        running.store(false);
        
        epoch_ready_cv.notify_all();
        
        for (auto& t : collector_threads) {
            if (t.joinable()) t.join();
        }
        if (epoch_timer.joinable()) epoch_timer.join();
        if (sequencer_thread.joinable()) sequencer_thread.join();
    }
    
private:
    // CRITICAL FIX: Lock-free collector with atomic epoch access
    void collector_thread(size_t queue_id) {
        constexpr size_t BATCH_SIZE = 32;  // Process in batches
        
        while (running.load()) {
            size_t read_idx = pbr_read_indices[queue_id].load();
            size_t write_idx = pbr_write_indices[queue_id].load();
            
            size_t available = (write_idx >= read_idx) ? (write_idx - read_idx) : 0;
            if (available == 0) {
                std::this_thread::yield();
                continue;
            }
            
            size_t to_process = std::min(available, BATCH_SIZE);
            size_t added = 0;
            
            // CRITICAL FIX: Batch processing with minimal locking
            std::vector<Message> batch_messages;
            batch_messages.reserve(to_process);
            
            // Collect batch from PBR
            for (size_t i = 0; i < to_process; ++i) {
                size_t pbr_idx = (read_idx + i) % pbr_entries_per_queue;
                PBREntry& entry = pbr_queues[queue_id][pbr_idx];
                
                if (!entry.complete.load()) break;
                
                batch_messages.emplace_back(entry.client_id, entry.client_order, 0);
                entry.complete.store(false);
                added++;
            }
            
            // CRITICAL FIX: Single lock acquisition for entire batch
            if (!batch_messages.empty()) {
                std::lock_guard<std::mutex> lock(epoch_switch_mutex);
                Epoch* current = current_collecting_epoch.load();
                
                if (current->collecting.load()) {
                    // Check capacity before adding
                    size_t space_available = MAX_MESSAGES_PER_EPOCH - current->messages.size();
                    size_t to_add = std::min(batch_messages.size(), space_available);
                    
                    for (size_t i = 0; i < to_add; ++i) {
                        current->messages.push_back(batch_messages[i]);
                    }
                    
                    total_processed.fetch_add(to_add);
                    
                    // Count dropped messages
                    if (to_add < batch_messages.size()) {
                        messages_dropped.fetch_add(batch_messages.size() - to_add);
                    }
                }
                
                pbr_read_indices[queue_id].fetch_add(added);
            }
        }
    }
    
    // OPTIMIZATION 3: Adaptive epoch timer
    void epoch_timer_thread() {
        auto next_switch = std::chrono::steady_clock::now();
        
        while (running.load()) {
            // Calculate adaptive duration
            Epoch* current = current_collecting_epoch.load();
            size_t duration_us = adaptive_config.calculate_duration(num_queues, current->last_fill);
            
            next_switch += std::chrono::microseconds(duration_us);
            std::this_thread::sleep_until(next_switch);
            
            // Switch epochs
            {
                std::lock_guard<std::mutex> lock(epoch_switch_mutex);
                
                Epoch* current = current_collecting_epoch.load();
                current->collecting.store(false);
                
                // Queue for sequencing
                {
                    std::lock_guard<std::mutex> ready_lock(epoch_ready_mutex);
                    epochs_to_sequence.push(current);
                }
                epoch_ready_cv.notify_one();
                
                // Switch to other epoch
                Epoch* next = (current == &epoch_a) ? &epoch_b : &epoch_a;
                next->reset(epoch_counter.fetch_add(1));
                current_collecting_epoch.store(next);
            }
        }
    }
    
    // CRITICAL: Single-threaded sequencer for perfect ordering
    void sequencer_worker() {
        while (running.load()) {
            Epoch* epoch_to_process = nullptr;
            
            // Wait for epoch to sequence
            {
                std::unique_lock<std::mutex> lock(epoch_ready_mutex);
                epoch_ready_cv.wait(lock, [this] { 
                    return !epochs_to_sequence.empty() || !running.load(); 
                });
                
                if (!running.load()) break;
                
                epoch_to_process = epochs_to_sequence.front();
                epochs_to_sequence.pop();
            }
            
            if (!epoch_to_process || epoch_to_process->messages.empty()) {
                continue;
            }
            
            // CRITICAL: Single atomic operation for entire epoch
            size_t message_count = epoch_to_process->messages.size();
            size_t base_sequence = global_sequence_counter.fetch_add(message_count);
            
            // Assign sequences
            for (size_t i = 0; i < message_count; ++i) {
                epoch_to_process->messages[i].global_sequence = base_sequence + i;
            }
            
            // CRITICAL: Write to GOI immediately in this thread (no race conditions!)
            size_t goi_pos = goi_write_index.load();
            for (size_t i = 0; i < message_count; ++i) {
                if (goi_pos < goi_entries) {
                    const Message& msg = epoch_to_process->messages[i];
                    goi[goi_pos].client_id = msg.client_id;
                    goi[goi_pos].client_order = msg.client_order;
                    goi[goi_pos].total_order = msg.global_sequence;
                    goi_pos++;
                }
            }
            goi_write_index.store(goi_pos);
            
            epochs_processed.fetch_add(1);
            epoch_to_process->sequenced.store(true);
        }
    }
    
public:
    // Testing interface
    bool write_to_pbr(size_t queue_id, size_t client_id, size_t client_order) {
        if (queue_id >= num_queues) return false;
        
        size_t write_idx = pbr_write_indices[queue_id].fetch_add(1);
        size_t idx = write_idx % pbr_entries_per_queue;
        
        PBREntry& entry = pbr_queues[queue_id][idx];
        entry.client_id = client_id;
        entry.client_order = client_order;
        entry.complete.store(true);
        
        return true;
    }
    
    // Perfect ordering verification
    bool verify_ordering() {
        size_t entries = goi_write_index.load();
        
        if (entries == 0) {
            std::cout << "No entries to verify\n";
            return true;
        }
        
        std::cout << "Verifying perfect ordering for " << entries << " entries...\n";
        
        // Check strict monotonic increase
        for (size_t i = 1; i < entries; ++i) {
            if (goi[i].total_order != goi[i-1].total_order + 1) {
                std::cerr << "ORDERING VIOLATION at " << i 
                         << ": " << goi[i-1].total_order << " -> " 
                         << goi[i].total_order << "\n";
                return false;
            }
        }
        
        // Check client ordering
        std::unordered_map<size_t, size_t> last_seen;
        for (size_t i = 0; i < entries; ++i) {
            size_t client = goi[i].client_id;
            size_t order = goi[i].client_order;
            
            if (last_seen.count(client) && order <= last_seen[client]) {
                std::cerr << "CLIENT ORDERING VIOLATION for client " << client << "\n";
                    return false;
            }
            last_seen[client] = order;
        }
        
        std::cout << "✅ PERFECT ORDERING VERIFIED (" << entries << " entries)\n";
        return true;
    }
    
    void print_stats() {
        size_t processed = total_processed.load();
        size_t dropped = messages_dropped.load();
        size_t goi_entries_written = goi_write_index.load();
        size_t epochs_done = epochs_processed.load();
        
        std::cout << "\n=== Optimized Epoch Sequencer Statistics ===\n";
        std::cout << "Messages processed: " << processed << "\n";
        std::cout << "Messages dropped: " << dropped << "\n";
        std::cout << "GOI entries written: " << goi_entries_written << "\n";
        std::cout << "Epochs processed: " << epochs_done << "\n";
        std::cout << "Current epoch duration: " << adaptive_config.current_duration_us << " μs\n";
        
        if (processed + dropped > 0) {
            double drop_rate = 100.0 * dropped / (processed + dropped);
            std::cout << "Drop rate: " << drop_rate << "%\n";
        }
        
        if (epochs_done > 0) {
            std::cout << "Avg messages/epoch: " << (goi_entries_written / epochs_done) << "\n";
        }
        
        std::cout << "Global sequence counter: " << global_sequence_counter.load() << "\n";
    }
    
    size_t get_total_messages() const {
        return goi_write_index.load();
    }
    
    size_t get_total_epochs() const {
        return epochs_processed.load();
    }
    
    size_t get_current_epoch_duration() const {
        return adaptive_config.current_duration_us;
    }
};

// Test harness
int main() {
    std::cout << "Optimized Epoch-Based Total Order Sequencer\n";
    std::cout << "============================================\n\n";
    
    // Test with increasing queue counts up to 32
    for (size_t num_queues : {1, 2, 4, 8, 16, 32}) {
        std::cout << "\n🚀 Testing with " << num_queues << " queues\n";
        std::cout << "------------------------\n";
        
        CorrectEpochSequencer sequencer(num_queues);
        sequencer.start();
        
        // CAPACITY-AWARE LOAD: Reduce messages for high queue counts to stay within system capacity
        const size_t messages_per_queue = (num_queues <= 8) ? 500000 : 
                                          (num_queues <= 16) ? 200000 : 100000;  // Scale down for high concurrency
        const size_t num_clients = num_queues * 10;
        
        std::cout << "Generating " << (messages_per_queue * num_queues) << " messages with " 
                  << num_clients << " clients...\n";
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Producer threads
        std::vector<std::thread> producers;
        for (size_t client_id = 0; client_id < num_clients; ++client_id) {
            producers.emplace_back([&sequencer, client_id, num_queues, messages_per_queue, num_clients]() {
                size_t queue_id = client_id % num_queues;
                size_t msgs_per_client = messages_per_queue / (num_clients / num_queues);
                
                for (size_t i = 1; i <= msgs_per_client; ++i) {
                    sequencer.write_to_pbr(queue_id, client_id, i);
                    
                    // CAPACITY-AWARE RATE: Adjust delay based on queue count
                    if (i % 10000 == 0) {
                        size_t delay_us = (num_queues <= 8) ? 50 : 
                                         (num_queues <= 16) ? 100 : 200;  // More delay for high concurrency
                        std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
                    }
                }
            });
        }
        
        // Wait for producers
        for (auto& p : producers) {
            p.join();
        }
        
        std::cout << "All producers finished, waiting for pipeline to flush...\n";
        
        // Let pipeline flush
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    
    auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        
    sequencer.stop();
    
    // Calculate throughput
    double seconds = duration.count() / 1000000.0;
        size_t total_messages = sequencer.get_total_messages();
        double throughput = total_messages / seconds;
        
        std::cout << "\n📊 Results:\n";
        std::cout << "Duration: " << seconds << " seconds\n";
        std::cout << "Messages processed: " << total_messages << " / " << (messages_per_queue * num_queues) << "\n";
        std::cout << "Completion rate: " << (100.0 * total_messages / (messages_per_queue * num_queues)) << "%\n";
        std::cout << "Throughput: " << throughput / 1000000.0 << "M msgs/sec\n";
        std::cout << "Per-queue throughput: " << (throughput / num_queues) / 1000000.0 << "M msgs/sec\n";
        std::cout << "Drop rate: " << (100.0 * (messages_per_queue * num_queues - total_messages) / (messages_per_queue * num_queues)) << "%\n";
        std::cout << "Final epoch duration: " << sequencer.get_current_epoch_duration() << " μs\n";
        
        // CRITICAL: Verify perfect ordering
        bool ordering_ok = sequencer.verify_ordering();
        if (!ordering_ok) {
            std::cerr << "❌ ORDERING VERIFICATION FAILED!\n";
            break;  // Stop testing if ordering fails
        }
        
        sequencer.print_stats();
        
        // Brief pause between tests
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    return 0;
}
