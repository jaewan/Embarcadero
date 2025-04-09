#pragma once

#include "common.h"

/**
 * Subscriber class for receiving messages from the messaging system
 */
class Subscriber {
public:
    /**
     * Constructor for Subscriber
     * @param head_addr Head broker address
     * @param port Port
     * @param topic Topic name
     * @param measure_latency Whether to measure latency
     */
    Subscriber(std::string head_addr, std::string port, char topic[TOPIC_NAME_SIZE], bool measure_latency = false);
    
    /**
     * Destructor - cleans up resources
     */
    ~Subscriber();
    
    /**
     * Consumes a message
     * @return Pointer to the consumed message
     */
    void* Consume();
    
    /**
     * Consumes a batch of messages
     * @return Pointer to the consumed batch
     */
    void* ConsumeBatch();
    
    /**
     * Debug method to check message ordering
     * @param order Order level to check
     * @return true if order is correct, false otherwise
     */
    bool DEBUG_check_order(int order);
    
    /**
     * Stores latency measurements to a file
     */
    void StoreLatency();
    
    /**
     * Debug method to wait for a certain amount of data
     * @param total_msg_size Total size of all messages
     * @param msg_size Size of each message
     */
    void DEBUG_wait(size_t total_msg_size, size_t msg_size);

private:
    std::string head_addr_;
    std::string port_;
    bool shutdown_;
    bool connected_;
    grpc::ClientContext context_;
    std::unique_ptr<HeartBeat::Stub> stub_;
    
    // Broker management
		absl::Mutex node_mutex_;
    absl::flat_hash_map<int, std::string> nodes_ ABSL_GUARDED_BY(node_mutex_);
    std::thread cluster_probe_thread_;
		absl::Mutex worker_mutex_;
		std::vector<std::pair<std::thread, int>> worker_threads_with_fds_ ABSL_GUARDED_BY(worker_mutex_);

    char topic_[TOPIC_NAME_SIZE];
    
    bool measure_latency_;
    size_t buffer_size_;
    std::atomic<size_t> DEBUG_count_ = 0;
    void* last_fetched_addr_;
    int last_fetched_offset_;
    int client_id_;
    bool DEBUG_do_not_check_order_ = false;

    /**
     * Thread for subscribing to messages
     * @param epoll_fd Epoll file descriptor
     * @param fd_to_msg Map of file descriptors to messages
     */
    void SubscribeThread(int epoll_fd, absl::flat_hash_map<int, std::pair<void*, msgIdx*>> fd_to_msg);
    
    /**
     * Creates a connection to a broker
     * @param broker_id Broker ID
     * @param address Broker address
     */
    void BrokerHandlerThread(int broker_id, const std::string &address);
    
    /**
     * Subscribes to cluster status updates
     */
    void SubscribeToClusterStatus();
    
    /**
     * Polls cluster status periodically
     */
    void ClusterProbeLoop();
		void ManageBrokerConnections(int broker_id, const std::string& address);
    void ReceiveWorkerThread(Subscriber* subscriber_instance, int broker_id, int fd_to_handle);
};

// Helper for Buffer Management
class ManagedConnectionResources {
public:
    void* buffer_ = nullptr;
    msgIdx* msg_idx_ = nullptr;
    size_t buffer_size_ = 0;

    ManagedConnectionResources() = default;

    ManagedConnectionResources(int broker_id, size_t buf_size) : buffer_size_(buf_size) {
        buffer_ = mmap(nullptr, buffer_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buffer_ == MAP_FAILED) {
             LOG(ERROR) << "Failed to mmap buffer: " << strerror(errno);
             buffer_ = nullptr;
             throw std::runtime_error("Failed to mmap buffer");
        }

        msg_idx_ = static_cast<msgIdx*>(malloc(sizeof(msgIdx)));
        if (!msg_idx_) {
             LOG(ERROR) << "Failed to allocate memory for msgIdx";
             munmap(buffer_, buffer_size_);
             buffer_ = nullptr;
             throw std::runtime_error("Failed to allocate msgIdx");
        }
        new (msg_idx_) msgIdx(broker_id);
    }

    ~ManagedConnectionResources() {
        if (msg_idx_) {
            msg_idx_->~msgIdx();
            free(msg_idx_);
            msg_idx_ = nullptr;
        }
        if (buffer_ != nullptr && buffer_ != MAP_FAILED) {
            munmap(buffer_, buffer_size_);
            buffer_ = nullptr;
        }
    }

    ManagedConnectionResources(const ManagedConnectionResources&) = delete;
    ManagedConnectionResources& operator=(const ManagedConnectionResources&) = delete;
    ManagedConnectionResources(ManagedConnectionResources&&) = delete;
    ManagedConnectionResources& operator=(ManagedConnectionResources&&) = delete;

    void* getBuffer() const { return buffer_; }
    msgIdx* getMsgIdx() const { return msg_idx_; }
};
