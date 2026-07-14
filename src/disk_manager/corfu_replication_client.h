#ifndef CORFU_REPLICATION_CLIENT_H_
#define CORFU_REPLICATION_CLIENT_H_

#include <string>
#include <memory>
#include <vector>
#include <random>
#include <mutex>
#include <atomic>
#include "disk_manager/corfu_ordered_chain.h"

// Include the generated gRPC headers
#include "corfu_replication.grpc.pb.h"

namespace grpc {
class Channel;
}

namespace Corfu {

/**
 * @brief Thread-safe client for the Corfu Replication Service
 *
 * This class provides a thread-safe client implementation for interacting with the
 * CorfuReplicationService gRPC service. It handles connections, retries,
 * and exponential backoff automatically and can be safely used from multiple threads.
 */
class CorfuReplicationClient {
public:
    /**
     * @brief Construct a new Corfu Replication Client
     *
     * @param server_address The address of the server in format "hostname:port"
     */
    explicit CorfuReplicationClient(const char* topic, size_t replication_factor, const std::string& server_address);
    CorfuReplicationClient(const char* topic, size_t replication_factor,
                           std::vector<CorfuReplicaTarget> ordered_targets);

    /**
     * @brief Destroy the client and release resources
     */
    ~CorfuReplicationClient();

    // Prevent copying
    CorfuReplicationClient(const CorfuReplicationClient&) = delete;
    CorfuReplicationClient& operator=(const CorfuReplicationClient&) = delete;

    /**
     * @brief Establish connection to the server
     *
     * This method is thread-safe and can be called concurrently.
     *
     * @param timeout_seconds Maximum time to wait for connection in seconds
     * @return true if connection successful, false otherwise
     */
    bool Connect(int timeout_seconds = 5);

    /**
     * @brief Send data to be replicated
     *
     * This method is thread-safe and can be called concurrently from multiple threads.
     *
     * @param id Unique identifier for the replication request
     * @param data The data to be replicated
     * @param response_message Optional pointer to store server response message
     * @param max_retries Number of retry attempts on failure
     * @return true if replication successful, false otherwise
     */
    bool ReplicateData(size_t start_idx, size_t size, void* data,
                      int max_retries = 3);
    bool AppendOrdered(const CorfuAppendDescriptor& descriptor);
    bool CompleteHole(const CorfuAppendDescriptor& descriptor);
    // Complete an explicitly identified abandoned slot.  Callers must obtain
    // the slot from durable recovery state; this API never guesses a hole.
    bool CompleteJunkHole(const CorfuSlotKey& slot);

    /**
     * @brief Check if client is connected to server
     *
     * @return true if connected, false otherwise
     */
    bool IsConnected() const;

    /**
     * @brief Attempt to reconnect to the server
     *
     * This method is thread-safe. If multiple threads call Reconnect simultaneously,
     * only one will perform the actual reconnection while others will wait.
     *
     * @param timeout_seconds Maximum time to wait for connection in seconds
     * @return true if reconnection successful, false otherwise
     */
    bool Reconnect(int timeout_seconds = 5);

private:
    /**
     * @brief Create or recreate the gRPC channel and stub
     *
     * This method is not thread-safe and should be called with the mutex locked.
     */
    void CreateChannelLocked();

    /**
     * @brief Ensure client is connected before operations
     *
     * Thread-safe method to check connection and connect if needed.
     *
     * @return true if connected or connection established, false otherwise
     */
    bool EnsureConnected();

    /**
     * @brief Calculate backoff time with jitter for retries
     *
     * Thread-safe method to generate backoff times.
     *
     * @param retry_attempt Current retry attempt number
     * @return Backoff time in milliseconds
     */
    int CalculateBackoffMs(int retry_attempt);

		std::string topic_;
		size_t replication_factor_;
		std::string server_address_;
		std::vector<CorfuReplicaTarget> ordered_targets_;
		std::vector<std::unique_ptr<CorfuChainEndpoint>> chain_endpoints_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<corfureplication::CorfuReplicationService::Stub> stub_;
    std::atomic<bool> is_connected_{false};

    // Mutex to protect shared state
    mutable std::mutex mutex_;

    // Mutex specifically for random number generation
    mutable std::mutex rng_mutex_;
    std::mt19937 random_engine_;

    // Reconnection state
    std::mutex reconnect_mutex_;
    std::atomic<bool> reconnection_in_progress_{false};

		// Sequential replication guarantee
		std::atomic<size_t> last_sequentially_replicated_;
};

} // End of namespace Corfu

#endif // CORFU_REPLICATION_CLIENT_H_
