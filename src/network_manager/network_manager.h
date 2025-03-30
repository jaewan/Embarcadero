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
#include "../disk_manager/disk_manager.h"
#include "../cxl_manager/cxl_manager.h"

namespace Embarcadero {

class CXLManager;
class DiskManager;

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

    /**
     * Enqueues a network request for processing by worker threads
     */
    void EnqueueRequest(struct NetworkRequest request);
    
    void SetDiskManager(DiskManager* disk_manager) { disk_manager_ = disk_manager; }
    void SetCXLManager(CXLManager* cxl_manager) { cxl_manager_ = cxl_manager; }
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
    void AckThread(const char* topic, int ack_fd);
    void Ack1Thread(const char* topic, int ack_fd);
    void SubscribeNetworkThread(int sock, int efd, const char* topic, int client_id);

    // Request handling helpers
    void HandlePublishRequest(int client_socket, const EmbarcaderoReq& handshake, 
                             const struct sockaddr_in& client_address);
    void HandleSubscribeRequest(int client_socket, const EmbarcaderoReq& handshake);
    bool SendMessageData(int sock_fd, int epoll_fd, void* buffer, size_t buffer_size, 
                        size_t& send_limit);
    bool IsConnectionAlive(int fd, char* buffer);
    
    // Thread-safe queues
    folly::MPMCQueue<std::optional<struct NetworkRequest>> request_queue_;
    folly::MPMCQueue<struct LargeMsgRequest> large_msg_queue_;
    
    // Thread management
    int broker_id_;
    std::vector<std::thread> threads_;
    int num_reqReceive_threads_;
    std::atomic<int> thread_count_{0};
    bool stop_threads_ = false;

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

		Embarcadero::GetNumBrokersCallback get_num_brokers_callback_;
};

} // namespace Embarcadero
#endif // EMBARCADERO_NETWORK_MANAGER_H_
