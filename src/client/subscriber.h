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
    std::thread cluster_probe_thread_;
    std::vector<std::thread> subscribe_threads_;
    
    // Broker management
    absl::flat_hash_map<int, std::string> nodes_;
    absl::Mutex mutex_;
    char topic_[TOPIC_NAME_SIZE];
    
    bool measure_latency_;
    size_t buffer_size_;
    std::atomic<size_t> DEBUG_count_ = 0;
    void* last_fetched_addr_;
    int last_fetched_offset_;
    std::vector<std::vector<std::pair<void*, msgIdx*>>> messages_;
    std::atomic<int> messages_idx_;
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
    void CreateAConnection(int broker_id, std::string address);
    
    /**
     * Subscribes to cluster status updates
     */
    void SubscribeToClusterStatus();
    
    /**
     * Polls cluster status periodically
     */
    void ClusterProbeLoop();
};
