#ifndef EMBARCADERO_NETWORK_MANAGER_H_
#define EMBARCADERO_NETWORK_MANAGER_H_

#include <thread>
#include <vector>
#include <optional>
#include <functional>
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

struct NetworkRequest {
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

struct LargeMsgRequest {
    void* msg;
    size_t len;
};

struct SubscriberState {
    absl::Mutex mu;
    size_t last_offset;
    void* last_addr;
    bool initialized = false;
};

// Forward declarations for non-blocking architecture
class StagingPool;

/**
 * Connection phase for non-blocking publish handling
 */
enum ConnectionPhase {
    INIT,              // Initial state after connection accept
    WAIT_HEADER,       // Waiting for complete batch header
    WAIT_PAYLOAD,      // Waiting for complete payload data
    WAIT_CXL,          // Waiting for CXL allocation (queued)
    COMPLETE           // Batch processed, ready for next batch
};

/**
 * Per-connection state for non-blocking publish handling
 */
struct ConnectionState {
    int fd;                           // Socket file descriptor
    ConnectionPhase phase;            // Current state machine phase

    // Header tracking
    BatchHeader batch_header;         // Partially or fully received header
    size_t header_offset;             // Bytes received so far (0..64)

    // Payload tracking
    void* staging_buf;                // Staging buffer pointer (nullptr if not allocated)
    size_t payload_offset;            // Bytes received so far

    // Retry tracking for CXL allocation
    int cxl_allocation_attempts;      // Exponential backoff counter

    // Connection metadata
    uint32_t client_id;
    std::string topic;
    bool epoll_registered;            // Whether socket is in epoll
    EmbarcaderoReq handshake;         // Original handshake for topic/ack info

    ConnectionState()
        : fd(-1),
          phase(INIT),
          header_offset(0),
          staging_buf(nullptr),
          payload_offset(0),
          cxl_allocation_attempts(0),
          client_id(0),
          epoll_registered(false) {
        memset(&handshake, 0, sizeof(handshake));
    }
};

/**
 * Pending batch waiting for CXL allocation
 */
struct PendingBatch {
    ConnectionState* conn_state;       // Connection that received this batch
    void* staging_buf;                 // Staging buffer with payload
    BatchHeader batch_header;          // Complete batch header
    EmbarcaderoReq handshake;          // Original handshake for topic/ack info

    PendingBatch()
        : conn_state(nullptr),
          staging_buf(nullptr) {}

    PendingBatch(ConnectionState* cs, void* buf, const BatchHeader& bh, const EmbarcaderoReq& hs)
        : conn_state(cs),
          staging_buf(buf),
          batch_header(bh),
          handshake(hs) {}
};

/**
 * New publish connection for non-blocking handling
 */
struct NewPublishConnection {
    int fd;                           // Socket file descriptor
    EmbarcaderoReq handshake;         // Handshake metadata
    struct sockaddr_in client_address; // Client address for ACK setup

    NewPublishConnection()
        : fd(-1) {
        memset(&handshake, 0, sizeof(handshake));
        memset(&client_address, 0, sizeof(client_address));
    }

    NewPublishConnection(int socket_fd, const EmbarcaderoReq& hs, const struct sockaddr_in& addr)
        : fd(socket_fd),
          handshake(hs),
          client_address(addr) {}
};

class NetworkManager {
public:
    /**
     * Creates a network manager for the specified broker
     * @param broker_id The ID of this broker
     * @param num_reqReceive_threads Number of request receiving threads to create
     * @param skip_networking If true, skip starting listener/receiver threads (sequencer-only mode)
     */
    NetworkManager(int broker_id, int num_reqReceive_threads = NUM_NETWORK_IO_THREADS,
                   bool skip_networking = false);
    
    /**
     * Destructor ensures clean shutdown of all threads
     */
    ~NetworkManager();

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
    /** @param topic Copy of topic name (thread may start after handshake is gone). */
    void AckThread(std::string topic, uint32_t ack_level, int ack_fd, int ack_efd);
    size_t GetOffsetToAck(const char* topic, uint32_t ack_level);
	void SubscribeNetworkThread(int sock, int efd, const char* topic, int connection_id);

    // Non-blocking architecture thread handlers
    void PublishReceiveThread();
    void CXLAllocationWorker();

    // Request handling helpers
    void HandlePublishRequest(int client_socket, const EmbarcaderoReq& handshake,
                             const struct sockaddr_in& client_address);
    void HandleSubscribeRequest(int client_socket, const EmbarcaderoReq& handshake);
    bool SendMessageData(int sock_fd, int epoll_fd, void* buffer, size_t buffer_size,
                        size_t& send_limit);
    bool IsConnectionAlive(int fd, char* buffer);

    // Non-blocking architecture helpers
    bool DrainHeader(int fd, ConnectionState* state);
    bool DrainPayload(int fd, ConnectionState* state);
    void CheckStagingPoolRecovery(int epoll_fd, absl::flat_hash_map<int, std::unique_ptr<ConnectionState>>& connections);
    void SetupPublishConnection(ConnectionState* state, const NewPublishConnection& conn);
    
    // Thread-safe queues
    folly::MPMCQueue<std::optional<struct NetworkRequest>> request_queue_;
    folly::MPMCQueue<struct LargeMsgRequest> large_msg_queue_;

    // Non-blocking architecture queues and state
    std::unique_ptr<StagingPool> staging_pool_;
    std::unique_ptr<folly::MPMCQueue<PendingBatch>> cxl_allocation_queue_;
    std::unique_ptr<folly::MPMCQueue<NewPublishConnection>> publish_connection_queue_;

    // Phase 3: Performance metrics (lock-free counters)
    std::atomic<uint64_t> metric_connections_routed_{0};
    std::atomic<uint64_t> metric_batches_drained_{0};
    std::atomic<uint64_t> metric_batches_copied_{0};
    std::atomic<uint64_t> metric_cxl_retries_{0};
    std::atomic<uint64_t> metric_staging_exhausted_{0};
    std::atomic<uint64_t> metric_ring_full_{0};  // CXL ring full events (non-blocking gating)
    std::atomic<uint64_t> metric_batches_dropped_{0};  // Batches dropped (max retries exceeded)

    // Thread management
    int broker_id_;
    std::vector<std::thread> threads_;
    int num_reqReceive_threads_;
    std::atomic<int> thread_count_{0};
    bool stop_threads_ = false;
    std::atomic<bool> listening_{false};

    // Acknowledgment management
    absl::flat_hash_map<size_t, int> ack_connections_;  // <client_id, ack_sock>
    absl::Mutex ack_mu_;
    absl::Mutex sub_mu_;
    absl::flat_hash_map<int, std::unique_ptr<SubscriberState>> sub_state_;  // <client_id, state>
    int ack_efd_; // Epoll file descriptor for acknowledgments
    int ack_fd_ = -1; // Socket file descriptor for acknowledgments
    
    // Manager dependencies
    CXLManager* cxl_manager_ = nullptr;
    DiskManager* disk_manager_ = nullptr;
    TopicManager* topic_manager_ = nullptr;
    Embarcadero::GetNumBrokersCallback get_num_brokers_callback_;
};

} // namespace Embarcadero
#endif // EMBARCADERO_NETWORK_MANAGER_H_
