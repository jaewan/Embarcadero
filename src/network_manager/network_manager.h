#ifndef EMBARCADERO_NETWORK_MANAGER_H_
#define EMBARCADERO_NETWORK_MANAGER_H_

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <optional>
#include <functional>
#include <utility>
#include "folly/MPMCQueue.h"
#include "absl/synchronization/mutex.h"
#include "absl/container/flat_hash_map.h"

#include "common/config.h"
#include "cxl_manager/cxl_datastructure.h"

namespace Embarcadero {

class CXLManager;
class DiskManager;
class TopicManager;

enum ClientRequestType {Publish, Subscribe};

struct alignas(64) NetworkRequest {
    int client_socket;
};

struct alignas(64) EmbarcaderoReq {
    uint32_t client_id;
    uint32_t ack;
    size_t num_msg;  // At Subscribe: used as last offset (set to -2 as sentinel value)
                     // At Publish: used as num brokers
    void* last_addr; // Subscribe: address of last fetched message
    uint32_t port;
    ClientRequestType client_req;
    char topic[32];  // Sized to maintain overall 64B alignment
};

struct alignas(64) LargeMsgRequest {
    void* msg;
    size_t len;
};

struct SubscriberState {
    absl::Mutex mu;
    size_t last_offset;
    void* last_addr;
    bool initialized = false;
};

/**
 * Frontier state for ORDER=0 inline processing.
 * Tracks next PBR slot to process for gapless written advancement.
 * Must be cache-aligned to prevent false sharing between topics.
 */
struct alignas(128) Order0FrontierState {
    std::atomic<size_t> next_complete_slot{0};  // Next PBR slot expected to complete (gapless advancement)
    char _padding[128 - sizeof(std::atomic<size_t>)];  // Prevent false sharing
};


class NetworkManager {
public:
    /**
     * Creates a network manager for the specified broker
     * @param broker_id The ID of this broker
     * @param num_reqReceive_threads Number of request receiving threads to create
     */
    NetworkManager(int broker_id, int num_reqReceive_threads = NUM_NETWORK_IO_THREADS);
    
    /**
     * Destructor ensures clean shutdown of all threads
     */
    ~NetworkManager();
    /** Idempotent explicit shutdown hook used by broker main to control stop order. */
    void Shutdown();

    /**
     * Enqueues a network request for processing by worker threads
     */
    void EnqueueRequest(struct NetworkRequest request);
    bool IsListening() const { return listening_.load(std::memory_order_acquire); }
    
    void SetDiskManager(DiskManager* disk_manager) { disk_manager_ = disk_manager; }
    void SetCXLManager(CXLManager* cxl_manager);
    void SetTopicManager(TopicManager* topic_manager) { topic_manager_ = topic_manager; }
		void RegisterGetNumBrokersCallback(GetNumBrokersCallback callback){
			get_num_brokers_callback_ = callback;
		}

private:
    // Network socket utility functions
    bool ConfigureNonBlockingSocket(int fd);
    bool SetupAcknowledgmentSocket(int& ack_fd, const struct sockaddr_in& client_address, uint32_t port);
    
    // Thread handlers
    void MainThread();
    void ReqReceiveThread();
    /** @param topic_cstr Topic name (converted to std::string internally for thread safety). */
    void AckThread(const char* topic_cstr, uint32_t ack_level, int ack_fd, int ack_efd);
    size_t GetOffsetToAck(const char* topic, uint32_t ack_level);
    std::pair<size_t, bool> GetOffsetToAckFast(const char* topic, uint32_t ack_level, size_t last_known_ack);
	void SubscribeNetworkThread(int sock, int efd, const char* topic, int connection_id);

    // Request handling helpers
    void HandlePublishRequest(int client_socket, const EmbarcaderoReq& handshake,
                             const struct sockaddr_in& client_address);
    void HandleSubscribeRequest(int client_socket, const EmbarcaderoReq& handshake);
    bool SendMessageData(int sock_fd, int epoll_fd, void* buffer, size_t buffer_size,
                        size_t& send_limit, size_t zero_copy_send_limit_cached);
    bool IsConnectionAlive(int fd, char* buffer);

    // [[PERF]] Blocking path: update written for Order 0 so ACK path advances
    void UpdateWrittenForOrder0(TInode* tinode, size_t logical_offset, uint32_t num_msg);

    // Process ORDER=0 batch inline: set per-message metadata.
    // Called by ReqReceive thread immediately after recv() to batch data, while hot in cache.
    void ProcessOrder0BatchInline(void* batch_data, uint32_t num_msg, size_t base_logical_offset);

    // ORDER=0 written frontier is advanced collaboratively from network threads.
    void TryAdvanceWrittenFrontier(const char* topic, size_t my_slot, uint32_t num_msg, TInode* tinode);

    // Thread-safe queues
    folly::MPMCQueue<std::optional<struct NetworkRequest>> request_queue_;
    folly::MPMCQueue<struct LargeMsgRequest> large_msg_queue_;


    // Thread management
    int broker_id_;
    std::vector<std::thread> threads_;
    int num_reqReceive_threads_;
    std::atomic<int> thread_count_{0};
    std::atomic<bool> stop_threads_{false};  // Atomic for correctness - volatile was dangerous
    std::atomic<bool> listening_{false};
    std::atomic<bool> shutdown_started_{false};

    // Acknowledgment management
    absl::flat_hash_map<size_t, int> ack_connections_;  // <client_id, ack_sock>
    absl::Mutex ack_mu_;
    absl::Mutex sub_mu_;
    std::vector<std::unique_ptr<SubscriberState>> sub_state_;  // <connection_id, state> - dense IDs
    int ack_efd_; // Epoll file descriptor for acknowledgments
    int ack_fd_ = -1; // Socket file descriptor for acknowledgments

    // ORDER=0 inline processing: frontier state per topic for gapless written advancement
    absl::Mutex frontier_mu_;
    absl::flat_hash_map<std::string, std::unique_ptr<Order0FrontierState>> order0_frontiers_;
    
    // Manager dependencies
    CXLManager* cxl_manager_ = nullptr;
    DiskManager* disk_manager_ = nullptr;
    TopicManager* topic_manager_ = nullptr;
    Embarcadero::GetNumBrokersCallback get_num_brokers_callback_;
};

} // namespace Embarcadero
#endif // EMBARCADERO_NETWORK_MANAGER_H_
