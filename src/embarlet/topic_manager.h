#ifndef INCLUDE_TOPIC_MANAGER_H_
#define INCLUDE_TOPIC_MANAGER_H_

// Standard library includes
#include <bits/stdc++.h>

// External library includes
#include "absl/container/flat_hash_map.h"
#include "absl/container/btree_set.h"
#include "absl/synchronization/mutex.h"
#include <glog/logging.h>

// Project includes
#include "../cxl_manager/cxl_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../disk_manager/corfu_replication_client.h"
#include "../disk_manager/scalog_replication_client.h"

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

namespace Embarcadero {

// Forward declarations
class CXLManager;
class DiskManager;

/**
 * Callback type for obtaining a new segment
 */
using GetNewSegmentCallback = std::function<void*()>;

/**
 * Class representing a message topic with storage and sequencing capabilities
 */
class Topic {
public:
    /**
     * Constructor for a new Topic
     *
     * @param get_new_segment_callback Callback function to get new storage segments
     * @param TInode_addr Address of the topic inode
     * @param replica_tinode Address of the replica inode (can be nullptr)
     * @param topic_name Name of the topic
     * @param broker_id ID of the broker handling this topic
     * @param order Ordering level for the topic
     * @param seq_type Type of sequencer to use
     * @param cxl_addr Base address of CXL memory
     * @param segment_metadata Pointer to segment metadata
     */
    Topic(GetNewSegmentCallback get_new_segment_callback,
          void* TInode_addr, TInode* replica_tinode,
          const char* topic_name, int broker_id, int order,
          heartbeat_system::SequencerType seq_type,
          void* cxl_addr, void* segment_metadata);

    /**
     * Destructor - ensures all threads are stopped and joined
     */
    ~Topic() {
        stop_threads_ = true;
        for (std::thread& thread : combiningThreads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        VLOG(3) << "[Topic]: \tDestructed";
    }

    // Delete copy constructor and copy assignment operator
    Topic(const Topic&) = delete;
    Topic& operator=(const Topic&) = delete;

    /**
     * Get the address and size of messages for a subscriber
     *
     * @param last_offset Reference to the last message offset seen by subscriber
     * @param last_addr Reference to the last message address seen by subscriber
     * @param messages Reference to store the messages pointer
     * @param messages_size Reference to store the size of messages
     * @return true if new messages were found, false otherwise
     */
    bool GetMessageAddr(size_t& last_offset,
                        void*& last_addr,
                        void*& messages,
                        size_t& messages_size);

    /**
     * Start the message combiner thread
     */
    void Combiner();

    /**
     * Get a buffer in CXL memory for a new batch of messages
     *
     * @param batch_header Reference to the batch header
     * @param topic Topic name
     * @param log Reference to store log pointer
     * @param segment_header Reference to store segment header pointer
     * @param logical_offset Reference to store logical offset
     * @return Callback function to execute after writing to the buffer
     */
    std::function<void(void*, size_t)> GetCXLBuffer(
        struct BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset) {
        return (this->*GetCXLBufferFunc)(batch_header, topic, log, segment_header, logical_offset);
    }

    /**
     * Get the sequencer type for this topic
     *
     * @return The sequencer type
     */
    heartbeat_system::SequencerType GetSeqtype() const {
        return seq_type_;
    }

private:
    /**
     * Update the TInode's written offset and address
     */
    inline void UpdateTInodeWritten(size_t written, size_t written_addr);

    /**
     * Thread function for the message combiner
     */
    void CombinerThread();

    /**
     * Check and handle segment boundary crossing
     */
    void CheckSegmentBoundary(void* log, size_t msgSize, unsigned long long int segment_metadata);

    // Function pointer type for GetCXLBuffer implementations
    using GetCXLBufferFuncPtr = std::function<void(void*, size_t)> (Topic::*)(
        BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset);

    // Pointer to the appropriate GetCXLBuffer implementation
    GetCXLBufferFuncPtr GetCXLBufferFunc;

    // Different implementations of GetCXLBuffer for different sequencer types
    std::function<void(void*, size_t)> KafkaGetCXLBuffer(
        BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset);

    std::function<void(void*, size_t)> CorfuGetCXLBuffer(
        BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset);

    std::function<void(void*, size_t)> ScalogGetCXLBuffer(
        BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset);

    std::function<void(void*, size_t)> Order3GetCXLBuffer(
        BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset);

    std::function<void(void*, size_t)> Order4GetCXLBuffer(
        BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset);

    std::function<void(void*, size_t)> EmbarcaderoGetCXLBuffer(
        BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset);

    // Core members
    const GetNewSegmentCallback get_new_segment_callback_;
    struct TInode* tinode_;
    struct TInode* replica_tinode_;
    std::string topic_name_;
    int broker_id_;
    struct MessageHeader* last_message_header_;
    int order_;
    heartbeat_system::SequencerType seq_type_;
    void* cxl_addr_;

    // Replication
    std::unique_ptr<Corfu::CorfuReplicationClient> corfu_replication_client_;
    std::unique_ptr<Scalog::ScalogReplicationClient> scalog_replication_client_;

    // Offset tracking
    size_t logical_offset_;
    size_t written_logical_offset_;
    void* written_physical_addr_;
    std::atomic<unsigned long long int> log_addr_;
    unsigned long long int batch_headers_;

    // First message pointers (nullptr if segment is GC'd)
    void* first_message_addr_;
    void* first_batch_headers_addr_;

    // Order 3 specific data structures
    absl::flat_hash_map<size_t, absl::flat_hash_map<size_t, void*>> skipped_batch_ ABSL_GUARDED_BY(mutex_);
    absl::flat_hash_map<size_t, size_t> order3_client_batch_ ABSL_GUARDED_BY(mutex_);

    // Synchronization
    absl::Mutex mutex_;
    absl::Mutex written_mutex_;

    // Kafka specific
    std::atomic<size_t> kafka_logical_offset_{0};
    absl::flat_hash_map<size_t, size_t> written_messages_range_;

    // TInode cache
    int replication_factor_;
    void* ordered_offset_addr_;
    void* current_segment_;
    size_t ordered_offset_;

    // Thread control
    bool stop_threads_ = false;
    std::vector<std::thread> combiningThreads_;
};

/**
 * Class for managing multiple topics
 */
class TopicManager {
public:
    /**
     * Constructor
     *
     * @param cxl_manager Reference to CXL memory manager
     * @param disk_manager Reference to disk storage manager
     * @param broker_id ID of the broker
     */
    TopicManager(CXLManager& cxl_manager, DiskManager& disk_manager, int broker_id) :
        cxl_manager_(cxl_manager),
        disk_manager_(disk_manager),
        broker_id_(broker_id),
        num_topics_(0) {
            VLOG(3) << "\t[TopicManager]\t\tConstructed";
        }

    /**
     * Destructor
     */
    ~TopicManager() {
        VLOG(3) << "\t[TopicManager]\tDestructed";
    }

    /**
     * Create a new topic with specified parameters
     *
     * @param topic Topic name
     * @param order Ordering level
     * @param replication_factor Number of replicas
     * @param replicate_tinode Whether to replicate the TInode
     * @param seq_type Type of sequencer to use
     * @return true if topic creation succeeded, false otherwise
     */
    bool CreateNewTopic(
        const char topic[TOPIC_NAME_SIZE],
        int order,
        int replication_factor,
        bool replicate_tinode,
        heartbeat_system::SequencerType seq_type);

    /**
     * Delete a topic
     *
     * @param topic Topic name to delete
     */
    void DeleteTopic(const char topic[TOPIC_NAME_SIZE]);

    /**
     * Get a buffer in CXL memory for a new batch of messages
     *
     * @param batch_header Reference to batch header
     * @param topic Topic name
     * @param log Reference to store log pointer
     * @param segment_header Reference to store segment header
     * @param logical_offset Reference to store logical offset
     * @param seq_type Reference to store sequencer type
     * @return Callback function to execute after writing to the buffer
     */
    std::function<void(void*, size_t)> GetCXLBuffer(
        BatchHeader& batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void*& log,
        void*& segment_header,
        size_t& logical_offset,
        heartbeat_system::SequencerType& seq_type);

    /**
     * Get message address and size for a topic
     *
     * @param topic Topic name
     * @param last_offset Reference to the last message offset seen
     * @param last_addr Reference to the last message address seen
     * @param messages Reference to store messages pointer
     * @param messages_size Reference to store messages size
     * @return true if new messages were found, false otherwise
     */
    bool GetMessageAddr(
        const char* topic,
        size_t& last_offset,
        void*& last_addr,
        void*& messages,
        size_t& messages_size);

private:
    /**
     * Internal implementation of topic creation
     *
     * @param topic Topic name
     * @return Pointer to the created TInode or nullptr on failure
     */
    struct TInode* CreateNewTopicInternal(const char topic[TOPIC_NAME_SIZE]);

    /**
     * Internal implementation of topic creation with parameters
     *
     * @param topic Topic name
     * @param order Ordering level
     * @param replication_factor Number of replicas
     * @param replicate_tinode Whether to replicate the TInode
     * @param seq_type Type of sequencer to use
     * @return Pointer to the created TInode or nullptr on failure
     */
    struct TInode* CreateNewTopicInternal(
        const char topic[TOPIC_NAME_SIZE],
        int order,
        int replication_factor,
        bool replicate_tinode,
        heartbeat_system::SequencerType seq_type);

    /**
     * Helper to initialize TInode offsets
     */
    void InitializeTInodeOffsets(
        TInode* tinode,
        void* segment_metadata,
        void* batch_headers_region,
        void* cxl_addr);

    /**
     * Get topic index from name
     *
     * @param topic Topic name
     * @return Topic index
     */
    int GetTopicIdx(const char topic[TOPIC_NAME_SIZE]) {
        return topic_to_idx_(topic) % MAX_TOPIC_SIZE;
    }

    /**
     * Check if this is the head node
     *
     * @return true if this is the head node (broker_id == 0)
     */
    inline bool IsHeadNode() const {
        return broker_id_ == 0;
    }

    // Core members
    CXLManager& cxl_manager_;
    DiskManager& disk_manager_;
    static const std::hash<std::string> topic_to_idx_;
    absl::flat_hash_map<std::string, std::unique_ptr<Topic>> topics_;
    absl::Mutex mutex_;
    int broker_id_;
    size_t num_topics_;
};

} // End of namespace Embarcadero

#endif // INCLUDE_TOPIC_MANAGER_H_
