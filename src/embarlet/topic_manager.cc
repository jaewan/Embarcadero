#include "topic_manager.h"

#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <immintrin.h>
#include "folly/ConcurrentSkipList.h"

namespace Embarcadero {

constexpr size_t NT_THRESHOLD = 128;

/**
 * Non-temporal memory copy function optimized for large data transfers
 * Uses streaming stores to bypass cache for large copies
 */
void nt_memcpy(void* __restrict dst, const void* __restrict src, size_t size) {
    static const size_t CACHE_LINE_SIZE = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

    // For small copies, use standard memcpy
    if (size < NT_THRESHOLD) {
        memcpy(dst, src, size);
        return;
    }

    // Handle unaligned portion at the beginning
    const uintptr_t dst_addr = reinterpret_cast<uintptr_t>(dst);
    const size_t unaligned_bytes = (CACHE_LINE_SIZE - dst_addr % CACHE_LINE_SIZE) % CACHE_LINE_SIZE;
    const size_t initial_bytes = std::min(unaligned_bytes, size);
    
    if (initial_bytes > 0) {
        memcpy(dst, src, initial_bytes);
    }
    
    uint8_t* aligned_dst = static_cast<uint8_t*>(dst) + initial_bytes;
    const uint8_t* aligned_src = static_cast<const uint8_t*>(src) + initial_bytes;
    size_t remaining = size - initial_bytes;

    // Process cache-line-aligned data with non-temporal stores
    const size_t num_lines = remaining / CACHE_LINE_SIZE;
    const size_t vectors_per_line = CACHE_LINE_SIZE / sizeof(__m128i);
    
    for (size_t i = 0; i < num_lines; i++) {
        for (size_t j = 0; j < vectors_per_line; j++) {
            const __m128i data = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(aligned_src + j * sizeof(__m128i)));
            _mm_stream_si128(
                reinterpret_cast<__m128i*>(aligned_dst + j * sizeof(__m128i)), data);
        }
        aligned_src += CACHE_LINE_SIZE;
        aligned_dst += CACHE_LINE_SIZE;
        remaining -= CACHE_LINE_SIZE;
    }

    // Copy any remaining bytes
    if (remaining > 0) {
        memcpy(aligned_dst, aligned_src, remaining);
    }
}

/**
 * Helper function to initialize TInode offsets
 */
void TopicManager::InitializeTInodeOffsets(TInode* tinode, 
                                          void* segment_metadata,
                                          void* batch_headers_region, 
                                          void* cxl_addr) {
    if (!tinode) return;
    
    // Initialize offset values
    tinode->offsets[broker_id_].ordered = -1;
    tinode->offsets[broker_id_].written = -1;

    // Calculate log offset using pointer difference plus CACHELINE_SIZE
    const uintptr_t segment_addr = reinterpret_cast<uintptr_t>(segment_metadata);
    const uintptr_t cxl_base_addr = reinterpret_cast<uintptr_t>(cxl_addr);
    tinode->offsets[broker_id_].log_offset = 
        static_cast<size_t>(segment_addr + CACHELINE_SIZE - cxl_base_addr);

    // Calculate batch headers offset using pointer difference
    const uintptr_t batch_headers_addr = reinterpret_cast<uintptr_t>(batch_headers_region);
    tinode->offsets[broker_id_].batch_headers_offset = 
        static_cast<size_t>(batch_headers_addr - cxl_base_addr);
}

struct TInode* TopicManager::CreateNewTopicInternal(const char topic[TOPIC_NAME_SIZE]) {
    struct TInode* tinode = cxl_manager_.GetTInode(topic);
    TInode* replica_tinode = nullptr;

    {
        absl::WriterMutexLock lock(&mutex_);

        CHECK_LT(num_topics_, MAX_TOPIC_SIZE) 
            << "Creating too many topics, increase MAX_TOPIC_SIZE";
            
        if (topics_.find(topic) != topics_.end()) {
            return nullptr;
        }

        void* cxl_addr = cxl_manager_.GetCXLAddr();
        void* segment_metadata = cxl_manager_.GetNewSegment();
        void* batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();

        // Handle replica if needed
        if (tinode->replicate_tinode) {
            replica_tinode = cxl_manager_.GetReplicaTInode(topic);
            InitializeTInodeOffsets(replica_tinode, segment_metadata, 
                                   batch_headers_region, cxl_addr);
        }

        InitializeTInodeOffsets(tinode, segment_metadata, batch_headers_region, cxl_addr);

        // Create the topic
        topics_[topic] = std::make_unique<Topic>(
            [this]() { return cxl_manager_.GetNewSegment(); },
            tinode,
            replica_tinode,
            topic,
            broker_id_,
            tinode->order,
            tinode->seq_type,
            cxl_addr,
            segment_metadata
        );
    }

    // Handle replication if needed
    int replication_factor = tinode->replication_factor;
    if (tinode->seq_type == EMBARCADERO && replication_factor > 0) {
        disk_manager_.Replicate(tinode, replica_tinode, replication_factor);
    }

    // Start combiner if needed
    if (tinode->seq_type != KAFKA && tinode->order != 4) {
        topics_[topic]->Combiner();
    }

    // Run sequencer if needed
    if (broker_id_ != 0 && tinode->seq_type == SCALOG) {
        cxl_manager_.RunSequencer(topic, tinode->order, tinode->seq_type);
    }

    return tinode;
}

struct TInode* TopicManager::CreateNewTopicInternal(
        const char topic[TOPIC_NAME_SIZE],
        int order,
        int replication_factor,
        bool replicate_tinode,
        SequencerType seq_type) {

    struct TInode* tinode = cxl_manager_.GetTInode(topic);
    struct TInode* replica_tinode = nullptr;

    // Check for name collision in tinode
    const bool no_collision = std::all_of(
        reinterpret_cast<const unsigned char*>(tinode->topic),
        reinterpret_cast<const unsigned char*>(tinode->topic) + TOPIC_NAME_SIZE,
        [](unsigned char c) { return c == 0; }
    );

    if (!no_collision) {
        LOG(ERROR) << "Topic name collides: " << tinode->topic << " handle collision";
        exit(1);
    }

    {
        absl::WriterMutexLock lock(&mutex_);

        CHECK_LT(num_topics_, MAX_TOPIC_SIZE) 
            << "Creating too many topics, increase MAX_TOPIC_SIZE";
            
        if (topics_.find(topic) != topics_.end()) {
            return nullptr;
        }

        void* cxl_addr = cxl_manager_.GetCXLAddr();
        void* segment_metadata = cxl_manager_.GetNewSegment();
        void* batch_headers_region = cxl_manager_.GetNewBatchHeaderLog();

        // Initialize tinode
        InitializeTInodeOffsets(tinode, segment_metadata, batch_headers_region, cxl_addr);
        tinode->order = order;
        tinode->replication_factor = replication_factor;
        tinode->replicate_tinode = replicate_tinode;
        tinode->seq_type = seq_type;
        memcpy(tinode->topic, topic, TOPIC_NAME_SIZE);

        // Handle replica if needed
        if (replicate_tinode) {
            char replica_topic[TOPIC_NAME_SIZE] = {0};
            memcpy(replica_topic, topic, TOPIC_NAME_SIZE);
            memcpy(replica_topic + (TOPIC_NAME_SIZE - 7), "replica", 7);

            replica_tinode = cxl_manager_.GetReplicaTInode(topic);

            const bool replica_no_collision = std::all_of(
                reinterpret_cast<const unsigned char*>(replica_tinode->topic),
                reinterpret_cast<const unsigned char*>(replica_tinode->topic) + TOPIC_NAME_SIZE,
                [](unsigned char c) { return c == 0; }
            );

            if (!replica_no_collision) {
                LOG(ERROR) << "Replica topic name collides: " << replica_tinode->topic 
                           << " handle collision";
                exit(1);
            }

            InitializeTInodeOffsets(replica_tinode, segment_metadata, 
                                   batch_headers_region, cxl_addr);
            replica_tinode->order = order;
            replica_tinode->replication_factor = replication_factor;
            replica_tinode->replicate_tinode = replicate_tinode;
            replica_tinode->seq_type = seq_type;
            memcpy(replica_tinode->topic, replica_topic, TOPIC_NAME_SIZE);
        }

        // Create the topic
        topics_[topic] = std::make_unique<Topic>(
            [this]() { return cxl_manager_.GetNewSegment(); },
            tinode,
            replica_tinode,
            topic,
            broker_id_,
            order,
            seq_type,
            cxl_addr,
            segment_metadata
        );
    }

    // Handle replication if needed
    if (replication_factor > 0) {
        disk_manager_.Replicate(tinode, replica_tinode, replication_factor);
    }

    // Start combiner if needed
    if (tinode->seq_type != KAFKA && tinode->order != 4) {
        topics_[topic]->Combiner();
    }

    return tinode;
}

bool TopicManager::CreateNewTopic(
        const char topic[TOPIC_NAME_SIZE], 
        int order, 
        int replication_factor, 
        bool replicate_tinode, 
        SequencerType seq_type) {
    
    TInode* tinode = CreateNewTopicInternal(
        topic, order, replication_factor, replicate_tinode, seq_type);
        
    if (tinode) {
        cxl_manager_.RunSequencer(topic, order, seq_type);
        return true;
    } else {
        LOG(ERROR) << "Topic already exists!";
        return false;
    }
}

void TopicManager::DeleteTopic(const char topic[TOPIC_NAME_SIZE]) {
    // Implementation placeholder
}

std::function<void(void*, size_t)> TopicManager::GetCXLBuffer(
        BatchHeader &batch_header,
        const char topic[TOPIC_NAME_SIZE], 
        void* &log, 
        void* &segment_header, 
        size_t &logical_offset, 
        SequencerType &seq_type) {
        
    auto topic_itr = topics_.find(topic);
    
    if (topic_itr == topics_.end()) {
        // Check if topic exists in CXL but not in local map
        const TInode* tinode = cxl_manager_.GetTInode(topic);
        if (tinode && memcmp(topic, tinode->topic, TOPIC_NAME_SIZE) == 0) {
            // Topic was created from another broker, create it locally
            CreateNewTopicInternal(topic);
            topic_itr = topics_.find(topic);
            
            if (topic_itr == topics_.end()) {
                LOG(ERROR) << "Topic Entry was not created, something is wrong";
                return nullptr;
            }
        } else {
            LOG(ERROR) << "[GetCXLBuffer] Topic: " << topic 
                       << " was not created before: " << tinode->topic
                       << " memcmp: " << memcmp(topic, tinode->topic, TOPIC_NAME_SIZE);
            return nullptr;
        }
    }
    
    seq_type = topic_itr->second->GetSeqtype();
    return topic_itr->second->GetCXLBuffer(
        batch_header, topic, log, segment_header, logical_offset);
}

bool TopicManager::GetMessageAddr(
        const char* topic, 
        size_t &last_offset,
        void* &last_addr, 
        void* &messages, 
        size_t &messages_size) {
        
    absl::ReaderMutexLock lock(&mutex_);
    
    auto topic_itr = topics_.find(topic);
    if (topic_itr == topics_.end()) {
        // Not throwing error as subscribe can be called before topic creation
        return false;
    }
    
    return topic_itr->second->GetMessageAddr(last_offset, last_addr, messages, messages_size);
}

Topic::Topic(
        GetNewSegmentCallback get_new_segment, 
        void* TInode_addr, 
        TInode* replica_tinode, 
        const char* topic_name,
        int broker_id, 
        int order, 
        SequencerType seq_type, 
        void* cxl_addr, 
        void* segment_metadata):
    get_new_segment_callback_(get_new_segment),
    tinode_(static_cast<struct TInode*>(TInode_addr)),
    replica_tinode_(replica_tinode),
    topic_name_(topic_name),
    broker_id_(broker_id),
    order_(order),
    seq_type_(seq_type),
    cxl_addr_(cxl_addr),
    logical_offset_(0),
    written_logical_offset_((size_t)-1),
    current_segment_(segment_metadata) {
    
    // Initialize addresses based on offsets
    log_addr_.store(static_cast<unsigned long long int>(
        reinterpret_cast<uintptr_t>(cxl_addr_) + tinode_->offsets[broker_id_].log_offset));
    
    batch_headers_ = static_cast<unsigned long long int>(
        reinterpret_cast<uintptr_t>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
    
    first_message_addr_ = reinterpret_cast<uint8_t*>(cxl_addr_) + 
                          tinode_->offsets[broker_id_].log_offset;
                          
    first_batch_headers_addr_ = reinterpret_cast<uint8_t*>(cxl_addr_) + 
                                tinode_->offsets[broker_id_].batch_headers_offset;
                                
    replication_factor_ = tinode_->replication_factor;
    ordered_offset_addr_ = nullptr;
    ordered_offset_ = 0;
    
    // Set appropriate get buffer function based on sequencer type
    if (seq_type == KAFKA) {
        GetCXLBufferFunc = &Topic::KafkaGetCXLBuffer;
    } else if (seq_type == CORFU) {
        // Initialize Corfu replication client
		// TODO(Jae) change this to actual replica address
        corfu_replication_client_ = std::make_unique<Corfu::CorfuReplicationClient>(
            topic_name, 
            replication_factor_, 
            "127.0.0.1:" + std::to_string(CORFU_REP_PORT)
        );
        
        if (!corfu_replication_client_->Connect()) {
            LOG(ERROR) << "Corfu replication client failed to connect to replica";
        }
        
        GetCXLBufferFunc = &Topic::CorfuGetCXLBuffer;
    } else if (seq_type == SCALOG) {
        if (replication_factor_ > 0) {
            scalog_replication_client_ = std::make_unique<Scalog::ScalogReplicationClient>(
                topic_name, 
                replication_factor_, 
                "localhost",
                broker_id_ // broker_id used to determine the port
            );
            
            if (!scalog_replication_client_->Connect()) {
                LOG(ERROR) << "Scalog replication client failed to connect to replica";
            }
        }
        GetCXLBufferFunc = &Topic::ScalogGetCXLBuffer;
    } else {
        // Set buffer function based on order
        if (order_ == 3) {
            GetCXLBufferFunc = &Topic::Order3GetCXLBuffer;
        } else if (order_ == 4) {
            GetCXLBufferFunc = &Topic::Order4GetCXLBuffer;
        } else {
            GetCXLBufferFunc = &Topic::EmbarcaderoGetCXLBuffer;
        }
    }
}

inline void Topic::UpdateTInodeWritten(size_t written, size_t written_addr) {
    // Update replica tinode if it exists
    if (tinode_->replicate_tinode && replica_tinode_) {
        replica_tinode_->offsets[broker_id_].written = written;
        replica_tinode_->offsets[broker_id_].written_addr = written_addr;
    }
    
    // Update primary tinode
    tinode_->offsets[broker_id_].written = written;
    tinode_->offsets[broker_id_].written_addr = written_addr;
}

void Topic::CombinerThread() {
    // Initialize header pointers
    void* segment_header = reinterpret_cast<uint8_t*>(first_message_addr_) - CACHELINE_SIZE;
    MessageHeader* header = reinterpret_cast<MessageHeader*>(first_message_addr_);
    
    while (!stop_threads_) {
        // Wait for message to be completed
        while (header->complete == 0) {
            if (stop_threads_) {
                return;
            }
            std::this_thread::yield();
        }

#ifdef MULTISEGMENT
        // Handle segment transition
        if (header->next_msg_diff != 0) { // Moved to new segment
            header = reinterpret_cast<MessageHeader*>(
                reinterpret_cast<uint8_t*>(header) + header->next_msg_diff);
            segment_header = reinterpret_cast<uint8_t*>(header) - CACHELINE_SIZE;
            continue;
        }
#endif

        // Update message metadata
        header->segment_header = segment_header;
        header->logical_offset = logical_offset_;
        header->next_msg_diff = header->paddedSize;
        
        // Ensure write ordering with a memory fence
        //std::atomic_thread_fence(std::memory_order_release);
        
        // Update tinode with write information
        UpdateTInodeWritten(
            logical_offset_, 
            static_cast<unsigned long long int>(
                reinterpret_cast<uint8_t*>(header) - reinterpret_cast<uint8_t*>(cxl_addr_))
        );
        
        // Update segment header
        *reinterpret_cast<unsigned long long int*>(segment_header) =
            static_cast<unsigned long long int>(
                reinterpret_cast<uint8_t*>(header) - reinterpret_cast<uint8_t*>(segment_header)
            );
            
        // Update tracking variables
        written_logical_offset_ = logical_offset_;
        written_physical_addr_ = reinterpret_cast<void*>(header);
        
        // Move to next message
        header = reinterpret_cast<MessageHeader*>(
            reinterpret_cast<uint8_t*>(header) + header->next_msg_diff);
        logical_offset_++;
    }
}

void Topic::Combiner() {
    combiningThreads_.emplace_back(&Topic::CombinerThread, this);
}

/**
 * Check and handle segment boundary crossing
 */
void Topic::CheckSegmentBoundary(
        void* log, 
        size_t msgSize, 
        unsigned long long int segment_metadata) {
    
    const uintptr_t log_addr = reinterpret_cast<uintptr_t>(log);
    const uintptr_t segment_end = segment_metadata + SEGMENT_SIZE;
    
    // Check if message would cross segment boundary
    if (segment_end <= log_addr + msgSize) {
        LOG(ERROR) << "Segment size limit reached (" << SEGMENT_SIZE 
                   << "). Increase SEGMENT_SIZE";

        // TODO(Jae) Implement segment boundary crossing
        if (segment_end <= log_addr) {
            // Allocate a new segment when log is entirely in next segment
        } else {
            // Wait for first thread that crossed segment to allocate new segment
        }
    }
}

std::function<void(void*, size_t)> Topic::KafkaGetCXLBuffer(
        BatchHeader &batch_header, 
        const char topic[TOPIC_NAME_SIZE], 
        void* &log, 
        void* &segment_header, 
        size_t &logical_offset) {
        
    size_t start_logical_offset;
    
    {
        absl::MutexLock lock(&mutex_);
        
        // Allocate space in the log
        log = reinterpret_cast<void*>(log_addr_.fetch_add(batch_header.total_size));
        logical_offset = logical_offset_;
        segment_header = current_segment_;
        start_logical_offset = logical_offset_;
        logical_offset_ += batch_header.num_msg;
        
        // Check for segment boundary issues
        if (reinterpret_cast<unsigned long long int>(current_segment_) + SEGMENT_SIZE <= log_addr_) {
            LOG(ERROR) << "!!!!!!!!! Increase the Segment Size: " << SEGMENT_SIZE;
            // TODO(Jae) Finish below segment boundary crossing code
        }
    }
    
    // Return completion callback function
    return [this, start_logical_offset](void* log_ptr, size_t logical_offset) {
        absl::MutexLock lock(&written_mutex_);
        
        if (kafka_logical_offset_.load() != start_logical_offset) {
            // Save for later processing
            written_messages_range_[start_logical_offset] = logical_offset;
        } else {
            // Process now and check for consecutive messages
            size_t start = start_logical_offset;
            bool has_next_messages_written = false;
            
            do {
                has_next_messages_written = false;
                
                // Update tracking state
                written_logical_offset_ = logical_offset;
                written_physical_addr_ = log_ptr;
                
                // Mark message as processed
                reinterpret_cast<MessageHeader*>(log_ptr)->logical_offset = static_cast<size_t>(-1);
                
                // Update TInode
                UpdateTInodeWritten(
                    logical_offset, 
                    static_cast<unsigned long long int>(
                        reinterpret_cast<uint8_t*>(log_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_))
                );
                
                // Update segment header
                *reinterpret_cast<unsigned long long int*>(current_segment_) =
                    static_cast<unsigned long long int>(
                        reinterpret_cast<uint8_t*>(log_ptr) - 
                        reinterpret_cast<uint8_t*>(current_segment_)
                    );
                
                // Move to next logical offset
                kafka_logical_offset_.store(logical_offset + 1);
                
                // Check if next message is already written
                if (written_messages_range_.contains(logical_offset + 1)) {
                    start = logical_offset + 1;
                    logical_offset = written_messages_range_[start];
                    written_messages_range_.erase(start);
                    has_next_messages_written = true;
                }
            } while (has_next_messages_written);
        }
    };
}

std::function<void(void*, size_t)> Topic::CorfuGetCXLBuffer(
        BatchHeader &batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void* &log,
        void* &segment_header,
        size_t &logical_offset) {

    // Calculate addresses
    const unsigned long long int segment_metadata = 
        reinterpret_cast<unsigned long long int>(current_segment_);
    const size_t msg_size = batch_header.total_size;

    // Get log address with batch offset
    log = reinterpret_cast<void*>(
        reinterpret_cast<uint8_t*>(
            reinterpret_cast<void*>(
                reinterpret_cast<uint8_t*>(cxl_addr_) + log_addr_.load()
            )
        ) + batch_header.log_idx
    );

    // Check for segment boundary issues
    CheckSegmentBoundary(log, msg_size, segment_metadata);

    // Return replication callback
    return [this, batch_header](void* log_ptr, size_t /*placeholder*/) {
        // Handle replication if needed
        if (replication_factor_ > 0 && corfu_replication_client_) {
            corfu_replication_client_->ReplicateData(
                batch_header.log_idx,
                batch_header.total_size,
                log_ptr
            );
        }
    };
}

std::function<void(void*, size_t)> Topic::Order3GetCXLBuffer(
        BatchHeader &batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void* &log,
        void* &segment_header,
        size_t &logical_offset) {

    absl::MutexLock lock(&mutex_);

    // Check if this batch was previously skipped
    if (skipped_batch_.contains(batch_header.client_id)) {
        auto& client_batches = skipped_batch_[batch_header.client_id];
        auto it = client_batches.find(batch_header.batch_seq);
        
        if (it != client_batches.end()) {
            log = it->second;
            client_batches.erase(it);
            return nullptr;
        }
    }

    // Initialize client tracking if needed
    if (!order3_client_batch_.contains(batch_header.client_id)) {
        order3_client_batch_.emplace(batch_header.client_id, broker_id_);
    }

    // Handle all skipped batches
    auto& client_seq = order3_client_batch_[batch_header.client_id];
    while (client_seq < batch_header.batch_seq) {
        // Allocate space for skipped batch
        void* skipped_addr = reinterpret_cast<void*>(log_addr_.load());
        
        // Store for later retrieval
        skipped_batch_[batch_header.client_id].emplace(client_seq, skipped_addr);

        // Move log address forward (assuming same batch size)
        log_addr_ += batch_header.total_size;
        
        // Update client sequence
        client_seq += batch_header.num_brokers;
    }

    // Allocate space for this batch
    log = reinterpret_cast<void*>(log_addr_.load());
    log_addr_ += batch_header.total_size;
    client_seq += batch_header.num_brokers;

    return nullptr;
}

std::function<void(void*, size_t)> Topic::Order4GetCXLBuffer(
        BatchHeader &batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void* &log,
        void* &segment_header,
        size_t &logical_offset) {

    // Calculate base addresses
    const unsigned long long int segment_metadata = 
        reinterpret_cast<unsigned long long int>(current_segment_);
    const size_t msg_size = batch_header.total_size;
    void* batch_headers_log;

    {
        absl::MutexLock lock(&mutex_);
        
        // Allocate space in log
        log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
        
        // Allocate space for batch header
        batch_headers_log = reinterpret_cast<void*>(batch_headers_);
        batch_headers_ += sizeof(BatchHeader);
    }

    // Check for segment boundary
    CheckSegmentBoundary(log, msg_size, segment_metadata);

    // Update batch header fields
    batch_header.next_reader_head = 0;
    batch_header.broker_id = 0;
    batch_header.num_brokers = 0;
    batch_header.total_order = 0;
    batch_header.log_idx = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_)
    );

    // Store batch header
    memcpy(batch_headers_log, &batch_header, sizeof(BatchHeader));

    return nullptr;
}

std::function<void(void*, size_t)> Topic::ScalogGetCXLBuffer(
        BatchHeader &batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void* &log,
        void* &segment_header,
        size_t &logical_offset) {
    static std::atomic<size_t> batch_offset = 0;
    batch_header.log_idx = batch_offset.fetch_add(batch_header.total_size); 

    // Calculate addresses
    const unsigned long long int segment_metadata = 
        reinterpret_cast<unsigned long long int>(current_segment_);
    const size_t msg_size = batch_header.total_size;

    // Allocate space in log
    log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
    
    // Check for segment boundary
    CheckSegmentBoundary(log, msg_size, segment_metadata);

    // Return replication callback
    return [this, batch_header, log](void* log_ptr, size_t /*placeholder*/) {
        MessageHeader *header = (MessageHeader*)log;
			// TODO(Jae) Change this to check from processed_ptr 
			// Wait until the message is combined
			while(header->next_msg_diff == 0){
				std::this_thread::yield();
			}

			// Handle replication if needed
			if (replication_factor_ > 0 && scalog_replication_client_) {
					scalog_replication_client_->ReplicateData(
							batch_header.log_idx,
							batch_header.total_size,
							batch_header.num_msg,
							log
					);
			}
    };
}

std::function<void(void*, size_t)> Topic::EmbarcaderoGetCXLBuffer(
        BatchHeader &batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void* &log,
        void* &segment_header,
        size_t &logical_offset) {

    // Calculate addresses
    const unsigned long long int segment_metadata = 
        reinterpret_cast<unsigned long long int>(current_segment_);
    const size_t msg_size = batch_header.total_size;

    // Allocate space in log
    log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
    
    // Check for segment boundary
    CheckSegmentBoundary(log, msg_size, segment_metadata);

    return nullptr;
}

/**
 * Get message address and size for topic subscribers
 *
 * Note: Current implementation depends on the subscriber knowing the physical
 * address of last fetched message. This is only true if messages were exported
 * from CXL. For disk cache optimization, we'd need to implement indexing.
 *
 * @return true if more messages are available
 */
bool Topic::GetMessageAddr(
        size_t &last_offset,
        void* &last_addr,
        void* &messages,
        size_t &messages_size) {

    // Determine current read position based on order
    size_t combined_offset;
    void* combined_addr;
    
    if (order_ > 0) {
        combined_offset = tinode_->offsets[broker_id_].ordered;
        combined_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + 
                        tinode_->offsets[broker_id_].ordered_offset;
    } else {
        combined_offset = written_logical_offset_;
        combined_addr = written_physical_addr_;
    }

    // Check if we have new messages
    if (combined_offset == static_cast<size_t>(-1) ||
        (last_addr != nullptr && combined_offset <= last_offset)) {
        return false;
    }

    // Find start message location
    MessageHeader* start_msg_header;
    
    if (last_addr != nullptr) {
        start_msg_header = static_cast<MessageHeader*>(last_addr);
        
        // Wait for message to be combined if necessary
        while (start_msg_header->next_msg_diff == 0) {
            std::this_thread::yield();
        }

        // Move to next message
        start_msg_header = reinterpret_cast<MessageHeader*>(
            reinterpret_cast<uint8_t*>(start_msg_header) + start_msg_header->next_msg_diff
        );
    } else {
        // Start from first message
        if (combined_addr <= last_addr) {
            LOG(ERROR) << "GetMessageAddr: Invalid address relationship";
            return false;
        }
        start_msg_header = static_cast<MessageHeader*>(first_message_addr_);
    }

    // Verify message is valid
    if (start_msg_header->paddedSize == 0) {
        return false;
    }

    // Set output message pointer
    messages = static_cast<void*>(start_msg_header);

#ifdef MULTISEGMENT
    // Multi-segment logic for determining message size and last offset
    unsigned long long int* segment_offset_ptr = 
        static_cast<unsigned long long int*>(start_msg_header->segment_header);
    
    MessageHeader* last_msg_of_segment = reinterpret_cast<MessageHeader*>(
        reinterpret_cast<uint8_t*>(segment_offset_ptr) + *segment_offset_ptr
    );

    if (combined_addr < last_msg_of_segment) {
        // Last message is not fully ordered yet
        messages_size = reinterpret_cast<uint8_t*>(combined_addr) -
                       reinterpret_cast<uint8_t*>(start_msg_header) +
                       reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize;
        last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
        last_addr = combined_addr;
    } else {
        // Return entire segment of messages
        messages_size = reinterpret_cast<uint8_t*>(last_msg_of_segment) -
                       reinterpret_cast<uint8_t*>(start_msg_header) +
                       last_msg_of_segment->paddedSize;
        last_offset = last_msg_of_segment->logical_offset;
        last_addr = static_cast<void*>(last_msg_of_segment);
    }
#else
    // Single-segment logic for determining message size and last offset
    messages_size = reinterpret_cast<uint8_t*>(combined_addr) -
                   reinterpret_cast<uint8_t*>(start_msg_header) +
                   reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize;

    last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
    last_addr = combined_addr;
#endif

    return true;
}

} // End of namespace Embarcadero
