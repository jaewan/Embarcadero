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
    WAIT_PBR_SLOT,     // Direct-CXL: payload in CXL, waiting for PBR slot (retry in recovery)
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

    // [[RECV_DIRECT_TO_CXL]] Direct recv-to-CXL path: CXL buffer and PBR slot for same-thread completion
    void* cxl_buf;                    // CXL buffer when recv_direct_to_cxl (payload recv target)
    BatchHeader* batch_header_location;  // PBR slot for batch_complete
    void* segment_header;             // From ReservePBRSlotAndWriteEntry (for callback if needed)
    size_t logical_offset;            // From ReservePBRSlotAndWriteEntry

    // Retry tracking for CXL allocation
    int cxl_allocation_attempts;      // Exponential backoff counter
    int pbr_retry_count;              // WAIT_PBR_SLOT: recovery attempts before fallback handoff
    int wait_cxl_retry_count;         // WAIT_CXL: backoff exponent (attempt every RECOVERY_INTERVAL * 2^min(this,4))

    // Connection metadata
    uint32_t client_id;
    std::string topic;
    bool epoll_registered;            // Whether socket is in epoll
    EmbarcaderoReq handshake;         // Original handshake for topic/ack info
    // [[PERF]] Cached Topic* to avoid 3× topics_mutex_ per batch. void* to avoid Topic in header include order.
    // Validated periodically (every kCachedTopicValidateInterval batches) to avoid use-after-free if topic is deleted/recreated (Issue #1).
    void* cached_topic{nullptr};
    uint32_t batch_count_since_validate{0};
    // [[Issue #3]] Single epoch check per batch: set true after CheckEpochOnce in WAIT_HEADER, pass to ReservePBRSlotAndWriteEntry in WAIT_PAYLOAD, then clear
    bool epoch_checked_for_batch{false};

    ConnectionState()
        : fd(-1),
          phase(INIT),
          header_offset(0),
          staging_buf(nullptr),
          payload_offset(0),
          cxl_buf(nullptr),
          batch_header_location(nullptr),
          segment_header(nullptr),
          logical_offset(0),
          cxl_allocation_attempts(0),
          pbr_retry_count(0),
          wait_cxl_retry_count(0),
          client_id(0),
          epoll_registered(false),
          cached_topic(nullptr),
          batch_count_since_validate(0),
          epoch_checked_for_batch(false) {
        memset(&handshake, 0, sizeof(handshake));
    }
};

/**
 * Pending batch waiting for CXL allocation
 * When cxl_buf_payload_already != nullptr: payload is already in CXL at that address (direct path
 * handed off after PBR reserve failed); worker only does ReservePBRSlotAndWriteEntry + completion.
 * conn_state is shared_ptr so ConnectionState outlives connection teardown while batch is in queue
 * (avoids use-after-free when client disconnects before CXLAllocationWorker processes the batch).
 */
struct PendingBatch {
    std::shared_ptr<ConnectionState> conn_state;  // Keeps connection state alive until batch is processed
    void* staging_buf;                 // Staging buffer with payload (null if payload already in CXL)
    void* cxl_buf_payload_already;    // When set: payload already in CXL; worker skips alloc+copy
    BatchHeader batch_header;          // Complete batch header
    EmbarcaderoReq handshake;          // Original handshake for topic/ack info

    PendingBatch()
        : conn_state(nullptr),
          staging_buf(nullptr),
          cxl_buf_payload_already(nullptr) {}

    PendingBatch(std::shared_ptr<ConnectionState> cs, void* buf, const BatchHeader& bh, const EmbarcaderoReq& hs)
        : conn_state(std::move(cs)),
          staging_buf(buf),
          cxl_buf_payload_already(nullptr),
          batch_header(bh),
          handshake(hs) {}
};

/**
 * Publish pipeline components for profiling (time spent per component, aggregated at end).
 */
enum PublishPipelineComponent : int {
	kDrainHeader = 0,
	kReserveBLogSpace,
	kDrainPayloadToBuffer,
	kReservePBRSlotAndWriteEntry,
	kCompleteBatchInCXL,
	kGetCXLBuffer,
	kCopyAndFlushPayload,
	kNumPipelineComponents
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
    bool DrainPayloadToBuffer(int fd, ConnectionState* state, void* dest_buf);
    void CheckStagingPoolRecovery(int epoll_fd, absl::flat_hash_map<int, std::shared_ptr<ConnectionState>>& connections);
    void SetupPublishConnection(ConnectionState* state, const NewPublishConnection& conn);

    // [[PERF]] Shared completion path helpers (happy path + recovery path)
    void UpdateWrittenForOrder0(TInode* tinode, size_t logical_offset, uint32_t num_msg);
    /** Single completion path: set num_msg, batch_complete, flush batch_header, optional Order0 written, one store_fence. */
    void CompleteBatchInCXL(BatchHeader* batch_header_location, uint32_t num_msg, TInode* tinode, size_t logical_offset);

    // [[PERF]] Publish pipeline profiling: per-component time (ns) and count, report aggregated at end
    void RecordProfile(PublishPipelineComponent c, uint64_t ns);
    void LogPublishPipelineProfile();

    // [[CODE_REVIEW]] Issue #1: Validate cached_topic periodically to avoid use-after-free if topic deleted/recreated
    static constexpr uint32_t kCachedTopicValidateInterval = 64;
    void EnsureCachedTopicValid(ConnectionState* state);
    // [[CODE_REVIEW]] Issue #5: Single helper for PBR watermark check (replaces 6+ ternary patterns)
    bool CheckPBRWatermark(ConnectionState* state, int pct);

    // Thread-safe queues
    folly::MPMCQueue<std::optional<struct NetworkRequest>> request_queue_;
    folly::MPMCQueue<struct LargeMsgRequest> large_msg_queue_;

    // Non-blocking architecture queues and state
    std::unique_ptr<StagingPool> staging_pool_;
    std::unique_ptr<folly::MPMCQueue<PendingBatch>> cxl_allocation_queue_;
    std::unique_ptr<folly::MPMCQueue<NewPublishConnection>> publish_connection_queue_;

    // Phase 3: Performance metrics (lock-free counters)
    std::atomic<uint64_t> metric_connections_routed_{0};
    std::atomic<uint64_t> metric_batches_drained_{0};       // Staging path: batches drained to staging
    std::atomic<uint64_t> metric_batches_recv_direct_cxl_{0};  // Direct-CXL path: batches completed in recv thread
    std::atomic<uint64_t> metric_batches_copied_{0};
    std::atomic<uint64_t> metric_cxl_retries_{0};
    std::atomic<uint64_t> metric_staging_exhausted_{0};
    std::atomic<uint64_t> metric_ring_full_{0};  // CXL ring full events (non-blocking gating)
    std::atomic<uint64_t> metric_direct_cxl_pbr_fallback_dropped_{0};  // PBR failed + queue full (handoff dropped)
    std::atomic<uint64_t> metric_direct_cxl_connection_drop_with_buf_{0};  // Connection closed while cxl_buf set (BLog leak)
    std::atomic<uint64_t> metric_blog_leaked_bytes_{0};  // Cumulative BLog bytes leaked on connection drop (alert if high)
    std::atomic<uint64_t> metric_batches_dropped_{0};  // Batches dropped (max retries exceeded)

    // Publish pipeline profile: total_ns and count per component (thread-safe). When false, no timing/RecordProfile in hot path.
    bool enable_pipeline_profile_{true};
    std::array<std::atomic<uint64_t>, kNumPipelineComponents> profile_total_ns_{};
    std::array<std::atomic<uint64_t>, kNumPipelineComponents> profile_count_{};
    static constexpr uint64_t kProfileLogIntervalBatches = 5000;  // Log profile every N batches (0 = only at shutdown)

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
