#include <atomic>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <limits>
#include <chrono>
#include <errno.h>

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

#include <glog/logging.h>
#include "mimalloc.h"

#include "network_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "../embarlet/topic_manager.h"
#include "../common/performance_utils.h"
#include "../common/wire_formats.h"

namespace Embarcadero {

//----------------------------------------------------------------------------
// Utility Functions
//----------------------------------------------------------------------------

/**
 * Closes socket and epoll file descriptors safely
 */
inline void CleanupSocketAndEpoll(int socket_fd, int epoll_fd) {
	if (socket_fd >= 0) {
		close(socket_fd);
	}
	if (epoll_fd >= 0) {
		close(epoll_fd);
	}
}

/**
 * Set SO_RCVBUF/SO_SNDBUF on an accepted payload socket so rcv_space is large.
 * Called after accept(); without this, accepted sockets use kernel default (~43KB).
 * @threading Called from MainThread (accept loop)
 */
static void SetAcceptedSocketBuffers(int fd) {
	const int buffer_size = 256 * 1024 * 1024;  // 256 MB (match client; reduces EAGAIN at 10GB/s)
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_RCVBUF) on accepted socket failed: " << strerror(errno);
	}
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_SNDBUF) on accepted socket failed: " << strerror(errno);
	}
	// [[CRITICAL FIX]] Disable Nagle's algorithm - was missing, causing latency!
	int flag = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
		LOG(WARNING) << "setsockopt(TCP_NODELAY) on accepted socket failed: " << strerror(errno);
	}
	// Enable TCP_QUICKACK so kernel sends ACKs immediately (reduces client RTT / backoff on blocking recv path)
	if (setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag)) < 0) {
		LOG(WARNING) << "setsockopt(TCP_QUICKACK) on accepted socket failed: " << strerror(errno);
	}
	// Enable SO_BUSY_POLL for ultra-low latency (kernel polls socket queue directly)
	int busy_poll = 50;  // microseconds to busy poll
	if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll)) < 0) {
		// SO_BUSY_POLL may not be available on all kernels, so don't treat as fatal
		LOG(WARNING) << "setsockopt(SO_BUSY_POLL) on accepted socket failed (may not be supported): " << strerror(errno);
	}
	// [[DIAGNOSTIC]] Verify kernel actually applied the buffer sizes
	int actual_rcv = 0, actual_snd = 0;
	socklen_t len = sizeof(actual_rcv);
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_rcv, &len);
	getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual_snd, &len);
}

// [[PERF]] Named constants for recovery loop (avoid magic numbers)
static constexpr int kRecoveryCheckInterval = 100;  // Check every N epoll iterations
static constexpr int kMaxPBRRetries = 20;           // WAIT_PBR_SLOT: recovery attempts before fallback handoff


// [[Phase 3.2]] written is cumulative (monotonic); fetch_add is sufficient. CXL layout uses
// volatile size_t; __atomic_fetch_add is the correct way to get atomic RMW (GCC/clang extension).
void NetworkManager::UpdateWrittenForOrder0(TInode* tinode, size_t logical_offset, uint32_t num_msg) {
	(void)logical_offset;
	if (!tinode) return;
	volatile size_t* written_ptr = &tinode->offsets[broker_id_].written;
	__atomic_fetch_add(reinterpret_cast<size_t*>(const_cast<size_t*>(written_ptr)), num_msg, __ATOMIC_RELEASE);
	CXL::flush_cacheline(CXL::ToFlushable(&tinode->offsets[broker_id_].written));
	CXL::store_fence();  // Required for CXL visibility; reader (AckThread) must see updated written
}



/**
 * [[ORDER0_INLINE]] Try to advance written frontier collaboratively.
 * Uses CAS to ensure only one thread processes each PBR slot (gapless written advancement).
 *
 * Algorithm:
 * 1. Load current frontier slot
 * 2. Check if that slot's batch_complete==1
 * 3. If yes, CAS to claim it
 * 4. Winner updates written, losers retry next slot
 * 5. Continue until finding incomplete slot (gap) or shutdown
 *
 * This ensures written advances monotonically without holes, even with parallel recv threads.
 * Key invariant: PBR slot N contains logical offsets assigned in allocation order → gapless.
 */
void NetworkManager::TryAdvanceWrittenFrontier(const char* topic, size_t my_slot, uint32_t num_msg, TInode* tinode) {
	(void)my_slot;  // Unused: frontier walks all slots, not just my_slot
	(void)num_msg;  // Unused: read num_msg from BatchHeader instead

	if (!tinode || !cxl_manager_) return;

	// Get or create frontier state for this topic
	Order0FrontierState* frontier = nullptr;
	{
		absl::MutexLock lock(&frontier_mu_);
		auto it = order0_frontiers_.find(topic);
		if (it == order0_frontiers_.end()) {
			order0_frontiers_[topic] = std::make_unique<Order0FrontierState>();
			frontier = order0_frontiers_[topic].get();
		} else {
			frontier = it->second.get();
		}
	}

	// Get PBR ring info
	size_t num_slots = BATCHHEADERS_SIZE / sizeof(BatchHeader);
	BatchHeader* pbr_base = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_manager_->GetCXLAddr()) +
		tinode->offsets[broker_id_].batch_headers_offset);

	// Try to advance frontier through consecutive complete slots
	int cas_retries = 0;
	while (!stop_threads_) {  // [[FIX: Shutdown check]] Prevents infinite loop during shutdown
		size_t slot = frontier->next_complete_slot.load(std::memory_order_acquire);
		size_t actual_slot = slot % num_slots;
		BatchHeader* bh = &pbr_base[actual_slot];

		// Check if this slot is complete
		if (!__atomic_load_n(&bh->batch_complete, __ATOMIC_ACQUIRE)) {
			break;  // Gap found - stop here
		}

		// Try to claim this slot via CAS
		if (!frontier->next_complete_slot.compare_exchange_weak(slot, slot + 1,
				std::memory_order_acq_rel, std::memory_order_acquire)) {
			// Lost race - add exponential backoff to reduce contention
			if (++cas_retries > 3) {
				std::this_thread::yield();
				cas_retries = 0;
			}
			continue;  // Retry
		}

		// Won the slot - update written atomically
		cas_retries = 0;  // Reset backoff
		size_t batch_num_msg = bh->num_msg;
		volatile size_t* written_ptr = &tinode->offsets[broker_id_].written;
		__atomic_fetch_add(
			reinterpret_cast<size_t*>(const_cast<size_t*>(written_ptr)),
			batch_num_msg, __ATOMIC_RELEASE);

		// Update TInode written_addr (last message end position)
		tinode->offsets[broker_id_].written_addr = bh->log_idx + bh->total_size;

		// Flush for CXL visibility
		CXL::flush_cacheline(CXL::ToFlushable(&tinode->offsets[broker_id_].written));
		CXL::flush_cacheline(CXL::ToFlushable(&tinode->offsets[broker_id_].written_addr));
		CXL::store_fence();

		// Clear batch_complete for ring reuse
		__atomic_store_n(&bh->batch_complete, 0, __ATOMIC_RELEASE);
	}
}

/**
 * Configures a socket for non-blocking operation with TCP optimizations (used for ACK and subscribe paths)
 */
bool NetworkManager::ConfigureNonBlockingSocket(int fd) {
	// Set non-blocking mode
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) {
		LOG(ERROR) << "fcntl F_GETFL failed: " << strerror(errno);
		return false;
	}

	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) == -1) {
		LOG(ERROR) << "fcntl F_SETFL failed: " << strerror(errno);
		return false;
	}

	// Enable socket reuse
	int flag = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
		LOG(WARNING) << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno);
		// Non-fatal, continue (this shouldn't fail but don't kill the connection)
	}

	// Enable TCP_NODELAY (disable Nagle's algorithm)
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0) {
		LOG(WARNING) << "setsockopt(TCP_NODELAY) failed: " << strerror(errno);
		// Non-fatal, continue (latency may be slightly higher)
	}

	// Enable TCP_QUICKACK for low-latency ACKs
	if (setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag)) != 0) {
		LOG(WARNING) << "setsockopt(TCP_QUICKACK) failed: " << strerror(errno);
		// Non-fatal, continue
	}
	// Enable SO_BUSY_POLL for ultra-low latency (kernel polls socket queue directly)
	int busy_poll = 50;  // microseconds to busy poll
	if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll)) < 0) {
		// SO_BUSY_POLL may not be available on all kernels, so don't treat as fatal
		LOG(WARNING) << "setsockopt(SO_BUSY_POLL) failed (may not be supported): " << strerror(errno);
	}

	// Increase socket buffers for high-throughput (128 MB; match SetAcceptedSocketBuffers)
	const int buffer_size = 256 * 1024 * 1024;  // 256 MB
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_SNDBUF) failed: " << strerror(errno);
		// Non-fatal, continue (will use default buffer size)
	} else {
		int actual = 0;
		socklen_t len = sizeof(actual);
		if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual, &len) == 0 && actual < buffer_size) {
			LOG(WARNING) << "SO_SNDBUF capped: requested " << buffer_size << " got " << actual
			             << ". Raise net.core.wmem_max (e.g. scripts/tune_kernel_buffers.sh)";
		}
	}
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_RCVBUF) failed: " << strerror(errno);
		// Non-fatal, continue (will use default buffer size)
	} else {
		int actual = 0;
		socklen_t len = sizeof(actual);
		if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &len) == 0 && actual < buffer_size) {
			LOG(WARNING) << "SO_RCVBUF capped: requested " << buffer_size << " got " << actual
			             << ". Raise net.core.rmem_max (e.g. scripts/tune_kernel_buffers.sh)";
		}
	}

	return true;
}

/**
 * Sets up an acknowledgment socket with connection retry logic
 */
bool NetworkManager::SetupAcknowledgmentSocket(int& ack_fd,
		const struct sockaddr_in& client_address,
		uint32_t port) {
	ack_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (ack_fd < 0) {
		LOG(ERROR) << "Socket creation failed for acknowledgment connection";
		return false;
	}

	if (!ConfigureNonBlockingSocket(ack_fd)) {
		close(ack_fd);
		return false;
	}

	// Setup server address for connection
	// [[FIX: B1_ACK_ZERO]] Use inet_ntop (thread-safe) and explicit uint16_t port to avoid inet_ntoa/truncation issues
	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(static_cast<uint16_t>(port & 0xFFFFu));
	char client_ip[INET_ADDRSTRLEN];
	if (inet_ntop(AF_INET, &client_address.sin_addr, client_ip, sizeof(client_ip)) == nullptr) {
		LOG(ERROR) << "SetupAcknowledgmentSocket: inet_ntop failed for broker " << broker_id_;
		close(ack_fd);
		return false;
	}
	server_addr.sin_addr.s_addr = inet_addr(client_ip);

	// Create epoll for connection monitoring
	ack_efd_ = epoll_create1(0);
	if (ack_efd_ == -1) {
		LOG(ERROR) << "epoll_create1 failed for acknowledgment connection";
		close(ack_fd);
		return false;
	}

	// Try connecting with retries
	const int MAX_RETRIES = 5;
	int retries = 0;

	while (retries < MAX_RETRIES) {
		int connect_result = connect(ack_fd,
				reinterpret_cast<const sockaddr*>(&server_addr),
				sizeof(server_addr));

		if (connect_result == 0) {
			// Connection succeeded immediately (sync)
			LOG(INFO) << "SetupAcknowledgmentSocket: Broker " << broker_id_
			          << " ACK connection established to " << client_ip << ":" << port << " (sync)";
			break;
		}

		if (errno != EINPROGRESS) {
			LOG(ERROR) << "SetupAcknowledgmentSocket: Broker " << broker_id_
			           << " connect failed to " << client_ip << ":" << port << ": " << strerror(errno);
			CleanupSocketAndEpoll(ack_fd, ack_efd_);
			return false;
		}

		// Connection is in progress, wait for completion with epoll
		struct epoll_event event;
		event.data.fd = ack_fd;
		event.events = EPOLLOUT;

		if (epoll_ctl(ack_efd_, EPOLL_CTL_ADD, ack_fd, &event) == -1) {
			LOG(ERROR) << "epoll_ctl failed: " << strerror(errno);
			CleanupSocketAndEpoll(ack_fd, ack_efd_);
			return false;
		}

		// Wait for socket to become writable
		struct epoll_event events[1];
		int n = epoll_wait(ack_efd_, events, 1, 5000);  // 5-second timeout

		if (n > 0 && (events[0].events & EPOLLOUT)) {
			// Check if the connection was successful
			int sock_error;
			socklen_t len = sizeof(sock_error);
			if (getsockopt(ack_fd, SOL_SOCKET, SO_ERROR, &sock_error, &len) < 0) {
				LOG(ERROR) << "getsockopt failed: " << strerror(errno);
				CleanupSocketAndEpoll(ack_fd, ack_efd_);
				return false;
			}

			if (sock_error == 0) {
				// Connection successful
				LOG(INFO) << "SetupAcknowledgmentSocket: Broker " << broker_id_
				          << " ACK connection established to " << client_ip << ":" << port << " (async)";
				break;
			} else {
				LOG(ERROR) << "SetupAcknowledgmentSocket: Broker " << broker_id_
				           << " connection failed to " << client_ip << ":" << port << ": " << strerror(sock_error);
			}
		} else if (n == 0) {
			// Timeout occurred
			LOG(ERROR) << "SetupAcknowledgmentSocket: Broker " << broker_id_
			           << " connection timed out to " << client_ip << ":" << port << ", retrying...";
		} else {
			// epoll_wait error
			LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
			CleanupSocketAndEpoll(ack_fd, ack_efd_);
			return false;
		}

		// Remove fd from epoll before retrying
		epoll_ctl(ack_efd_, EPOLL_CTL_DEL, ack_fd, NULL);
		retries++;
		sleep(1);  // Wait before retrying
	}

	if (retries == MAX_RETRIES) {
		LOG(ERROR) << "SetupAcknowledgmentSocket: Broker " << broker_id_
		           << " max retries reached connecting to " << client_ip << ":" << port << ". ACK channel will not work.";
		CleanupSocketAndEpoll(ack_fd, ack_efd_);
		return false;
	}

	// Close the connection-monitoring epoll before creating send-monitoring epoll
	close(ack_efd_);

	// Setup epoll for the connected socket
	ack_efd_ = epoll_create1(0);
	if (ack_efd_ == -1) {
		LOG(ERROR) << "Failed to create epoll for ack monitoring";
		close(ack_fd);
		return false;
	}

	struct epoll_event event;
	event.data.fd = ack_fd;
	event.events = EPOLLOUT;
	if (epoll_ctl(ack_efd_, EPOLL_CTL_ADD, ack_fd, &event) == -1) {
		LOG(ERROR) << "epoll_ctl failed for ack connection";
		CleanupSocketAndEpoll(ack_fd, ack_efd_);
		return false;
	}

	return true;
}

/**
 * Checks if a connection is still alive
 */
bool NetworkManager::IsConnectionAlive(int fd, char* buffer) {
	if (fd <= 0) {
		return false;
	}

	int result = recv(fd, buffer, 1, MSG_PEEK | MSG_DONTWAIT);
	return result != 0;  // 0 indicates a closed connection
}

//----------------------------------------------------------------------------
// Constructor/Destructor
//----------------------------------------------------------------------------

NetworkManager::NetworkManager(int broker_id, int num_reqReceive_threads)
	: request_queue_(64),
	large_msg_queue_(10000),
	broker_id_(broker_id),
	num_reqReceive_threads_(num_reqReceive_threads) {

		// Single path: blocking recv direct to BLog (no staging, no non-blocking epoll)
		LOG(INFO) << "NetworkManager: Blocking recv direct to BLog (single code path)";

		// Create main listener thread
		threads_.emplace_back(&NetworkManager::MainThread, this);

		// Create request handler threads (each handles publish via HandlePublishRequest)
		for (int i = 0; i < num_reqReceive_threads; i++) {
			threads_.emplace_back(&NetworkManager::ReqReceiveThread, this);
		}

		// Wait for all threads to start
		while (thread_count_.load() != (1 + num_reqReceive_threads_)) {
			// Busy wait until all threads are ready
		}
	}

NetworkManager::~NetworkManager() {
	Shutdown();
}

void NetworkManager::Shutdown() {
	if (shutdown_started_.exchange(true, std::memory_order_acq_rel)) {
		return;
	}

	// Signal threads to stop
	stop_threads_.store(true, std::memory_order_release);

	// Wake any threads blocked on socket/epoll waits.
	{
		absl::MutexLock lock(&ack_mu_);
		for (auto& [client_id, ack_sock] : ack_connections_) {
			(void)client_id;
			if (ack_sock >= 0) {
				shutdown(ack_sock, SHUT_RDWR);
			}
		}
	}
	if (ack_fd_ >= 0) {
		shutdown(ack_fd_, SHUT_RDWR);
	}

	// Send sentinel values to wake up blocked threads
	std::optional<struct NetworkRequest> sentinel = std::nullopt;
	for (int i = 0; i < num_reqReceive_threads_; i++) {
		request_queue_.blockingWrite(sentinel);
	}

	// Join all threads
	for (std::thread& thread : threads_) {
		if (thread.joinable()) {
			thread.join();
		}
	}
}

void NetworkManager::SetCXLManager(CXLManager* cxl_manager) {
	cxl_manager_ = cxl_manager;
}


void NetworkManager::EnqueueRequest(struct NetworkRequest request) {
	request_queue_.blockingWrite(request);
}

//----------------------------------------------------------------------------
// Main Server Thread
//----------------------------------------------------------------------------

void NetworkManager::MainThread() {
	thread_count_.fetch_add(1, std::memory_order_relaxed);

	// Create server socket
	int server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket < 0) {
		LOG(ERROR) << "Socket creation failed: " << strerror(errno);
		return;
	}

	// Configure socket options
	int flag = 1;
	if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
		LOG(ERROR) << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno);
		close(server_socket);
		return;
	}

	if (setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
		LOG(ERROR) << "setsockopt(TCP_NODELAY) failed: " << strerror(errno);
		close(server_socket);
		return;
	}

	// Configure server address
	struct sockaddr_in server_address;
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(PORT + broker_id_);
	server_address.sin_addr.s_addr = INADDR_ANY;

	// Bind socket with retry logic
	{
		int bind_attempts = 0;
		while (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
			LOG(ERROR) << "Error binding socket to port " << (PORT + broker_id_)
				<< " for broker " << broker_id_ << ": " << strerror(errno);
			if (++bind_attempts >= 6) {
				close(server_socket);
				return;
			}
			sleep(5);  // Retry after delay
		}
	}

	// Start listening
	if (listen(server_socket, SOMAXCONN) == -1) {
		LOG(ERROR) << "Error starting listener: " << strerror(errno);
		close(server_socket);
		return;
	}

	listening_.store(true, std::memory_order_release);

	// Create epoll instance
	int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		LOG(ERROR) << "epoll_create1 failed: " << strerror(errno);
		close(server_socket);
		return;
	}

	// Add server socket to epoll
	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = server_socket;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &event) == -1) {
		LOG(ERROR) << "epoll_ctl failed: " << strerror(errno);
		close(server_socket);
		close(epoll_fd);
		return;
	}

	// Main event loop
	const int MAX_EVENTS = 16;
	struct epoll_event events[MAX_EVENTS];

	while (!stop_threads_) {
		// PERFORMANCE OPTIMIZATION: Reduced timeout for better responsiveness
		int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);
		for (int i = 0; i < n; i++) {
			if (events[i].data.fd == server_socket) {
				// Accept new connection
				struct NetworkRequest req;
				struct sockaddr_in client_addr;
				socklen_t client_addr_len = sizeof(client_addr);

			req.client_socket = accept(server_socket,
					(struct sockaddr*)&client_addr,
					&client_addr_len);

			if (req.client_socket < 0) {
				LOG(ERROR) << "Error accepting connection: " << strerror(errno);
				continue;
			}

			// Apply large socket buffers so rcv_space is not ~43KB (kernel default).
			// Without this, throughput is TCP-window limited (~884 MB/s).
			SetAcceptedSocketBuffers(req.client_socket);

			// Enqueue the request for processing
			request_queue_.blockingWrite(req);
			}
		}
	}

	// Cleanup
	close(server_socket);
	close(epoll_fd);
}

//----------------------------------------------------------------------------
// Request Processing Threads
//----------------------------------------------------------------------------

void NetworkManager::ReqReceiveThread() {
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct NetworkRequest> opt_req;

	while (!stop_threads_) {
		// Wait for a new request
		request_queue_.blockingRead(opt_req);

		// Check if this is a sentinel value (shutdown signal)
		if (!opt_req.has_value()) {
			break;
		}

		const struct NetworkRequest &req = opt_req.value();
		// Ensure blocking handshake recv wakes periodically so shutdown can be observed.
		struct timeval recv_timeout;
		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 200 * 1000;  // 200ms
		setsockopt(req.client_socket, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

		// Get client address information
		struct sockaddr_in client_address;
		socklen_t client_address_len = sizeof(client_address);
		getpeername(req.client_socket, (struct sockaddr*)&client_address, &client_address_len);

		// Perform handshake to determine request type (robust partial read)
		EmbarcaderoReq handshake{};
		size_t read_total = 0;

		while (read_total < sizeof(handshake)) {
			int ret = recv(req.client_socket,
					reinterpret_cast<char*>(&handshake) + read_total,
					sizeof(handshake) - read_total,
					0);
			if (ret <= 0) {
				if (ret < 0) {
					if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
					    !stop_threads_) {
						continue;
					}
					LOG(ERROR) << "Error receiving handshake: " << strerror(errno);
				}
				close(req.client_socket);
				if (stop_threads_) {
					break;
				}
				return;
			}
			read_total += static_cast<size_t>(ret);
		}
		if (stop_threads_) {
			close(req.client_socket);
			break;
		}

		// Ensure topic string is terminated to avoid strlen overrun
		handshake.topic[sizeof(handshake.topic) - 1] = '\0';

		// Process based on request type (single path: blocking recv direct to BLog)
		switch (handshake.client_req) {
			case Publish:
				HandlePublishRequest(req.client_socket, handshake, client_address);
				break;

			case Subscribe:
				HandleSubscribeRequest(req.client_socket, handshake);
				break;
		}
	}
}

//----------------------------------------------------------------------------
// Publish Request Handling
//----------------------------------------------------------------------------

void NetworkManager::HandlePublishRequest(
		int client_socket,
		const EmbarcaderoReq& handshake,
		const struct sockaddr_in& client_address) {
	if (stop_threads_) {
		close(client_socket);
		return;
	}

	// Validate topic
	if (strlen(handshake.topic) == 0) {
		LOG(ERROR) << "Topic cannot be null";
		close(client_socket);
		return;
	}

	// Setup acknowledgment channel if needed
	int ack_fd = client_socket;

	// Ensure blocking recv loops wake periodically to observe shutdown.
	struct timeval recv_timeout;
	recv_timeout.tv_sec = 0;
	recv_timeout.tv_usec = 200 * 1000;  // 200ms
	setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

	if (handshake.ack >= 1) {
		absl::MutexLock lock(&ack_mu_);
		auto it = ack_connections_.find(handshake.client_id);

		if (it != ack_connections_.end()) {
			ack_fd = it->second;
		} else {
		// Create new acknowledgment connection
		// Capture ack_efd_ locally before starting thread to prevent race condition
		int local_ack_efd = -1;
		if (!SetupAcknowledgmentSocket(ack_fd, client_address, handshake.port)) {
			close(client_socket);
			return;
		}

		// Capture the ack_efd_ value immediately after setup, before it can be overwritten
		local_ack_efd = ack_efd_;
		
		ack_fd_ = ack_fd;
		ack_connections_[handshake.client_id] = ack_fd;

		// [[CRITICAL: Validate topic before starting AckThread]]
		// Empty topic → GetOffsetToAck returns wrong TInode → client ACK timeout
		if (strlen(handshake.topic) == 0) {
			LOG(ERROR) << "HandlePublishRequest: Empty topic in handshake for broker " << broker_id_
			           << ", client_id=" << handshake.client_id << ". NOT starting AckThread!";
			// Still keep connection open for publish, but ACKs will not work correctly
		} else {
			LOG(INFO) << "HandlePublishRequest: Starting AckThread for broker " << broker_id_
			          << ", topic='" << handshake.topic << "', client_id=" << handshake.client_id;
			// Pass local_ack_efd to thread so it uses the correct epoll instance
			threads_.emplace_back(&NetworkManager::AckThread, this, handshake.topic, handshake.ack, ack_fd, local_ack_efd);
		}
		}
	}

	// Process message batches
	bool running = true;

	while (running && !stop_threads_) {
		// Read batch header
		BatchHeader batch_header;
		batch_header.client_id = handshake.client_id;
		batch_header.ordered = 0;

		ssize_t bytes_read = recv(client_socket, &batch_header, sizeof(BatchHeader), 0);
		if (bytes_read <= 0) {
			if (bytes_read < 0) {
				if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
				    !stop_threads_) {
					continue;
				}
				LOG(ERROR) << "Error receiving batch header: " << strerror(errno);
			} else {
				// bytes_read == 0 indicates connection closed by peer
				LOG(WARNING) << "NetworkManager: Connection closed by publisher (client_id=" 
				            << handshake.client_id << ", topic=" << handshake.topic 
				            << "). No batch data received. This may indicate publisher failed to send or disconnected early.";
			}
			running = false;
			break;
		}

		// Finish reading batch header if partial read
		while (bytes_read < static_cast<ssize_t>(sizeof(BatchHeader))) {
			ssize_t recv_ret = recv(client_socket,
					((uint8_t*)&batch_header) + bytes_read,
					sizeof(BatchHeader) - bytes_read,
					0);
			if (recv_ret < 0) {
				if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
				    !stop_threads_) {
					continue;
				}
				LOG(ERROR) << "Error receiving batch header: " << strerror(errno);
				running = false;
				return;
			}
			bytes_read += recv_ret;
		}
		if (stop_threads_) {
			break;
		}

		// Allocate buffer for message batch
		size_t to_read = batch_header.total_size;
		void* segment_header = nullptr;
		void* buf = nullptr;
		size_t logical_offset = 0;  // Initialize to prevent garbage values in UpdateWrittenForOrder0
		SequencerType seq_type;
		BatchHeader* batch_header_location = nullptr;

		std::function<void(void*, size_t)> non_emb_seq_callback = nullptr;

		// [[Issue #3]] Get Topic* once per batch for single epoch check and ReservePBRSlotAfterRecv(Topic*, ..., epoch_checked).
		Topic* topic_ptr = cxl_manager_->GetTopicPtr(handshake.topic);
		bool epoch_checked_this_batch = false;

		// Use GetCXLBuffer for batch-level allocation and zero-copy receive
		// BLOCKING MODE: Wait indefinitely for ring space - NEVER close connection
		size_t wait_iterations = 0;

		while (!buf && !stop_threads_) {
			if (stop_threads_) {
				break;
			}
			// [[Issue #3]] Single epoch check per batch: do once for EMBARCADERO, then pass epoch_checked to GetCXLBuffer and ReservePBRSlotAfterRecv.
			// [[PERF_REGRESSION_FIX]] Removed outdated ORDER_0_TAIL_ACK special case - ORDER=0 now uses PBR uniformly.
			if (topic_ptr && topic_ptr->GetSeqtype() == EMBARCADERO) {
				if (!epoch_checked_this_batch) {
					if (topic_ptr->CheckEpochOnce()) {
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
						continue;
					}
					epoch_checked_this_batch = true;
				}
			} else {
				epoch_checked_this_batch = false;
			}
			non_emb_seq_callback = cxl_manager_->GetCXLBuffer(batch_header, handshake.topic, buf,
					segment_header, logical_offset, seq_type, batch_header_location, epoch_checked_this_batch);

			if (buf != nullptr) {
				break;  // Success
			}

			// Ring full - wait for consumer to make space
			wait_iterations++;
			// [[FIX_RING_FULL_YIELD]] Use yield() immediately for ring full - consumer needs CPU time.
			// Ring full means consumer is slow, so yield to allow consumer threads to catch up.
			std::this_thread::yield();
		}

		if (!buf) {
			// Only happens if stop_threads_ is set (shutdown)
			LOG(WARNING) << "NetworkManager (blocking): Shutting down, closing connection to client_id="
			             << handshake.client_id;
			break;
		}

		// [[PERF_REGRESSION_FIX]] Removed ORDER_0_LOG_IDX - not needed now that ORDER=0 uses PBR.

		// [[DESIGN: PBR reserve after receive]] For EMBARCADERO, GetCXLBuffer only allocates BLog (buf);
		// batch_header_location is intentionally null here and is set later by ReservePBRSlotAfterRecv.
		if (batch_header_location == nullptr) {
			if (seq_type != EMBARCADERO) {
				static std::atomic<size_t> batch_header_location_null_count{0};
				size_t cnt = batch_header_location_null_count.fetch_add(1, std::memory_order_relaxed) + 1;
				LOG(ERROR) << "NetworkManager: GetCXLBuffer returned null batch_header_location (count=" << cnt
				           << ") topic=" << handshake.topic << " seq_type=" << static_cast<int>(seq_type)
				           << " batch_seq=" << batch_header.batch_seq << " — batch will not be sequenced or acked";
			}
		} else {
			// batch_header_location is set
		}

		// Receive message data (byte-accurate accounting only)
		size_t read = 0;
		bool batch_data_complete = false;
		while (running && !stop_threads_) {
			bytes_read = recv(client_socket, (uint8_t*)buf + read, to_read, 0);
			if (bytes_read < 0) {
				if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
				    !stop_threads_) {
					continue;
				}
				LOG(ERROR) << "Error receiving message data: " << strerror(errno);
				running = false;
				batch_data_complete = false;
				break;
			}
			if (bytes_read == 0) {
				LOG(WARNING) << "Connection closed while receiving message data (remaining=" << to_read 
				            << ", received=" << read << " of " << batch_header.total_size 
				            << " bytes). Batch incomplete - will not mark batch_complete or send ACK.";
				running = false;
				batch_data_complete = false;
				break;
			}

			read += static_cast<size_t>(bytes_read);
			to_read -= static_cast<size_t>(bytes_read);

			if (to_read == 0) {
				batch_data_complete = true;
				break;
			}
		}

		// Only process batch if all data was received
		if (!batch_data_complete) {
			LOG(WARNING) << "NetworkManager: Batch incomplete (received " << read << " of " 
			            << batch_header.total_size << " bytes) for batch_seq=" << batch_header.batch_seq
			            << ". Closing connection to avoid stream desync.";
			// Connection is now out-of-sync; close to avoid interpreting payload as next header.
			running = false;
			break;
		}
		// [[DESIGN: PBR reserve after receive]] Reserve PBR slot only after payload is fully received.
	// [[PERF_REGRESSION_FIX]] Reverted ORDER_0_SKIP_PBR: ORDER=0 now uses PBR like all other orders.
	// This allows DelegationThread to process ORDER=0 batches, restoring 10-12 GB/s performance.
	TInode* tinode = nullptr;
		if (seq_type == EMBARCADERO) {
		size_t post_recv_attempts = 0;
		// Retry post-recv PBR reservation with exponential backoff (max 10 seconds)
		int sleep_ms = 1;  // Start at 1 ms
		auto post_recv_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!batch_header_location && !stop_threads_) {
			// [[FIX: Force Epoch Refresh]] Pass false for epoch_already_checked to force ReservePBRSlotAfterRecv
			// to refresh the epoch from CXL. This ensures that if recv() blocked for a long time,
			// we don't stamp the batch with a stale epoch that gets dropped by the sequencer.
			bool ok = topic_ptr
				? cxl_manager_->ReservePBRSlotAfterRecv(topic_ptr, batch_header, buf,
						segment_header, logical_offset, batch_header_location, false)
				: cxl_manager_->ReservePBRSlotAfterRecv(handshake.topic, batch_header, buf,
						segment_header, logical_offset, batch_header_location);
			if (ok) {
				break;  // Success
			}
			post_recv_attempts++;
			// Check deadline; if exceeded, close connection (avoid blocking forever)
			if (std::chrono::steady_clock::now() >= post_recv_deadline) {
				LOG(ERROR) << "NetworkManager: PBR backpressure timeout (10s) for batch_seq="
				           << batch_header.batch_seq << " client_id=" << handshake.client_id
				           << " — closing connection to prevent indefinite stall";
				running = false;
				break;
			}
			
			// [[FIX_BACKOFF_ADAPTIVE]] Use adaptive backoff: yield for small delays, sleep for large ones.
			// Network issues need thread cooperation, but very long delays should actually sleep.
			if (sleep_ms <= 1) {
				std::this_thread::yield();
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
			}
			sleep_ms = std::min(100, sleep_ms * 2);
		}
		
		if (!batch_header_location) {
			// Either deadline exceeded or stop_threads_ set
			if (stop_threads_) {
				VLOG(1) << "NetworkManager: Shutdown during post-recv PBR reservation for batch_seq="
				        << batch_header.batch_seq;
			} else {
				LOG(ERROR) << "NetworkManager: PBR reservation failed for batch_seq="
				           << batch_header.batch_seq << " client_id=" << handshake.client_id
				           << " after " << post_recv_attempts << " attempts";
			}
			running = false;
			break;
		}
	}

		// Post-receive parsing only where required
		MessageHeader* header = nullptr;  // Last message header for callback
		if (seq_type == KAFKA && batch_header.num_msg > 0) {
			MessageHeader* current_header = reinterpret_cast<MessageHeader*>(buf);
			size_t remaining = batch_header.total_size;
			for (size_t i = 0; i < batch_header.num_msg; ++i) {
				if (remaining < sizeof(MessageHeader)) {
					LOG(WARNING) << "NetworkManager: KAFKA batch too small for header, remaining=" << remaining;
					break;
				}
				if (current_header->paddedSize == 0 || current_header->paddedSize > remaining) {
					LOG(WARNING) << "NetworkManager: KAFKA invalid paddedSize=" << current_header->paddedSize
					             << " remaining=" << remaining;
					break;
				}
				current_header->logical_offset = logical_offset;
				if (segment_header == nullptr) {
					LOG(ERROR) << "segment_header is null!";
				}
				current_header->segment_header = segment_header;
				current_header->next_msg_diff = current_header->paddedSize;
				logical_offset++;

				remaining -= current_header->paddedSize;
				header = current_header;  // Track last header for callback
				current_header = reinterpret_cast<MessageHeader*>(
					reinterpret_cast<uint8_t*>(current_header) + current_header->paddedSize);
			}
		}

	// Whether we're using BlogMessageHeader (Order 5 + env): used only to skip v1 validation when publisher sent v2.
	bool is_blog_header_enabled = false;
	if (seq_type == EMBARCADERO && !tinode) {
		tinode = (TInode*)cxl_manager_->GetTInode(handshake.topic);
		if (tinode && tinode->order == 5 && HeaderUtils::ShouldUseBlogHeader())
			is_blog_header_enabled = true;
	}

	// Signal batch completion for ALL order levels
	// This must be done AFTER all messages in the batch are received and marked complete
	// CRITICAL: Only mark batch_complete if all data was successfully received
	if (batch_header_location != nullptr && batch_data_complete) {
		// [[INVARIANT: BatchHeader must be sane before marking complete]]
		if (batch_header.num_msg == 0 || batch_header.total_size == 0) {
			LOG(WARNING) << "NetworkManager: Invalid batch header before completion (num_msg="
			             << batch_header.num_msg << ", total_size=" << batch_header.total_size
			             << "). Skipping batch_complete to avoid corrupt sequencing.";
			running = false;
			break;
		}
		// For Sequencer 5 with BlogHeader: messages already valid from publisher
		// For other orders: Ensure message validation
		if (seq_type != EMBARCADERO || (tinode && tinode->order != 5) || !is_blog_header_enabled) {
			// For Sequencer 4 and other modes: Validate v1 headers without blocking
			MessageHeader* first_msg = reinterpret_cast<MessageHeader*>(buf);
			size_t remaining = batch_header.total_size;
			for (size_t i = 0; i < batch_header.num_msg; ++i) {
				if (remaining < sizeof(MessageHeader)) {
					LOG(WARNING) << "NetworkManager: v1 batch too small for header, remaining=" << remaining;
					break;
				}
				if (first_msg->paddedSize == 0 || first_msg->paddedSize > remaining) {
					LOG(WARNING) << "NetworkManager: v1 invalid paddedSize=" << first_msg->paddedSize
					             << " remaining=" << remaining;
					break;
				}
				remaining -= first_msg->paddedSize;
				first_msg = reinterpret_cast<MessageHeader*>(
					reinterpret_cast<uint8_t*>(first_msg) + first_msg->paddedSize
				);
			}
		}
		// [[DESIGN: Write PBR entry only after full receive]] Batch is fully in blog; write the
		// complete BatchHeader to the PBR slot once. Slot was zeroed in GetCXLBuffer.
		// [[PERF: Batch flush pattern]] Topic::PublishPBRSlotAfterRecv writes both cachelines, then one fence.
		batch_header.flags = kBatchHeaderFlagValid;
		// [[CORFU_ORDER3]] Skip batch_header_location operations if nullptr (Order 3 doesn't use it)
		if (batch_header_location != nullptr) {
			bool published = false;
			if (topic_ptr && topic_manager_) {
				published = topic_manager_->PublishPBRSlotAfterRecv(topic_ptr, batch_header, batch_header_location);
			} else if (topic_manager_) {
				published = topic_manager_->PublishPBRSlotAfterRecv(handshake.topic, batch_header, batch_header_location);
			}
			if (!published) {
				LOG(ERROR) << "NetworkManager: Failed to publish PBR slot for batch_seq="
				           << batch_header.batch_seq << " topic=" << handshake.topic;
				running = false;
				break;
			}
		}

		// [[ORDER_0_ACK_RACE_FIX]] Track the highest logical offset where batch_complete=1 was set.
		// Used on connection close to advance written without waiting for DelegationThread.
		if (topic_ptr) {
			size_t batch_end_offset = logical_offset + batch_header.num_msg;
			topic_ptr->TrackBatchComplete(batch_end_offset);
		}

		// [[ARCHITECTURE]] DelegationThread handles per-message metadata + written updates for all orders
	} else if (batch_header_location == nullptr) {
		LOG(WARNING) << "NetworkManager: batch_header_location is null for batch with " << batch_header.num_msg << " messages, order_level=" << seq_type;
	}

		// Finalize batch processing
		if (non_emb_seq_callback) {
			// Batch flush for better performance (only for KAFKA mode)
			if (seq_type == KAFKA && header) {
				// Flush the last message header to ensure visibility
#ifdef __INTEL__
				_mm_clflushopt(header);
#elif defined(__AMD__)
				_mm_clwb(header);
#endif
			}
			
			non_emb_seq_callback((void*)header, logical_offset - 1);
			if (seq_type == CORFU) {
				// Replication ack not implemented
			}
		}
	}

	// [[ORDER_0_ACK_RACE_FIX]] When publisher closes, directly update written to last_batch_complete_offset.
	// This avoids race with DelegationThread (no message chain walk needed).
	{
		Topic* topic_final = cxl_manager_->GetTopicPtr(handshake.topic);
		if (topic_final) {
			topic_final->UpdateWrittenToLastComplete();
		}
	}

	close(client_socket);
}

//----------------------------------------------------------------------------
// Subscribe Request Handling
//----------------------------------------------------------------------------

void NetworkManager::HandleSubscribeRequest(
		int client_socket,
		const EmbarcaderoReq& handshake) {

	LOG(INFO) << "Broker " << broker_id_ << " received subscribe request for topic=" << handshake.topic;

	// Configure socket for optimal throughput
	if (!ConfigureNonBlockingSocket(client_socket)) {
		close(client_socket);
		return;
	}

	// Set larger send buffer
	int send_buffer_size = 16 * 1024 * 1024;  // 16MB
	if (setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) == -1) {
		LOG(ERROR) << "Subscriber setsockopt SO_SNDBUF failed";
		close(client_socket);
		return;
	}

	// Enable zero-copy
	int flag = 1;
	if (setsockopt(client_socket, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag)) < 0) {
		LOG(ERROR) << "Subscriber setsockopt SO_ZEROCOPY failed";
		close(client_socket);
		return;
	}

	// Create epoll for monitoring writability
	int epoll_fd = epoll_create1(0);
	if (epoll_fd < 0) {
		LOG(ERROR) << "Subscribe thread epoll_create1 failed: " << strerror(errno);
		close(client_socket);
		return;
	}

	struct epoll_event event;
	event.data.fd = client_socket;
	event.events = EPOLLOUT | EPOLLET;  // Edge-triggered: send until EAGAIN then wait (reduces epoll_wait syscalls)
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event) < 0) {
		LOG(ERROR) << "epoll_ctl failed: " << strerror(errno);
		close(client_socket);
		close(epoll_fd);
		return;
	}

	// Initialize or update subscriber state - each connection gets its own state
	// Use unique connection ID to avoid collisions with reused socket FDs
	// All connections start from offset 0 to receive the same data (redundancy/throughput)
	static std::atomic<int> connection_counter{0};
	int unique_connection_id = connection_counter.fetch_add(1);
	
	{
		absl::MutexLock lock(&sub_mu_);
		// Ensure vector is large enough for this connection_id
		if (sub_state_.size() <= static_cast<size_t>(unique_connection_id)) {
			sub_state_.resize(unique_connection_id + 1);
		}
		auto state = std::make_unique<SubscriberState>();
		state->last_offset = 0;  // Always start from beginning
		state->last_addr = handshake.last_addr;
		state->initialized = true;
		sub_state_[unique_connection_id] = std::move(state);
	}

	// Process subscription - pass unique_connection_id for independent state
	SubscribeNetworkThread(client_socket, epoll_fd, handshake.topic, unique_connection_id);

	// Cleanup subscriber state when connection ends
	{
		absl::MutexLock lock(&sub_mu_);
		if (static_cast<size_t>(unique_connection_id) < sub_state_.size()) {
			sub_state_[unique_connection_id].reset();
		}
	}

	// Cleanup
	close(client_socket);
	close(epoll_fd);
}

void NetworkManager::SubscribeNetworkThread(
		int sock,
		int efd,
		const char* topic,
		int connection_id) {

	size_t zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
	// [[FIX: BUG_A]] Topic may not exist yet when subscriber connects before publisher.
	// Wait up to 30 seconds for the topic to be created, then read its order.
	int order = -1;
	{
		constexpr int kMaxWaitSec = 30;
		constexpr int kPollIntervalMs = 100;
		int waited_ms = 0;
		while (!stop_threads_ && waited_ms < kMaxWaitSec * 1000) {
			int cur_order = topic_manager_->GetTopicOrder(topic);
			if (topic_manager_->GetTopic(topic) != nullptr) {
				order = cur_order;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
			waited_ms += kPollIntervalMs;
		}
		if (order == -1) {
			if (topic_manager_->GetTopic(topic) == nullptr) {
				LOG(ERROR) << "SubscribeNetworkThread: Topic '" << topic << "' not found after " << kMaxWaitSec << "s. Exiting.";
				return;
			}
			order = topic_manager_->GetTopicOrder(topic);
		}
	}
	LOG(INFO) << "SubscribeNetworkThread started for topic=" << topic << " order=" << order << " connection_id=" << connection_id;

	// Define batch metadata structure for Sequencer 5
	// This metadata is sent before each batch to help subscribers reconstruct message ordering
	struct BatchMetadata {
		size_t batch_total_order;  // Starting total_order for this batch
		uint32_t num_messages;     // Number of messages in this batch
		uint16_t header_version;   // Message header format version (1=MessageHeader, 2=BlogMessageHeader)
		uint16_t flags;            // Reserved for future flags
	} batch_meta = {0, 0, 2, 0};  // Initialize header_version=2 for BlogMessageHeader

	static_assert(sizeof(BatchMetadata) == sizeof(wire::BatchMetadata), 
		"Local BatchMetadata must match wire::BatchMetadata");
	static_assert(offsetof(BatchMetadata, header_version) == offsetof(wire::BatchMetadata, header_version),
		"header_version field offset must match wire::BatchMetadata");

	// PERFORMANCE OPTIMIZATION: Cache state pointer to avoid repeated vector lookups
	std::unique_ptr<SubscriberState>* cached_state_ptr = nullptr;
	{
		absl::MutexLock lock(&sub_mu_);
		if (static_cast<size_t>(connection_id) >= sub_state_.size() || !sub_state_[connection_id]) {
			LOG(ERROR) << "SubscribeNetworkThread: No state found for connection_id " << connection_id;
			return;
		}
		cached_state_ptr = &sub_state_[connection_id];
	}

	while (!stop_threads_) {
		// Get message data to send
		void* msg = nullptr;
		size_t messages_size = 0;
		struct LargeMsgRequest req;
		// [[BUG_FIX: STATE_AFTER_SEND]] Only for order 0: pending state to apply after successful send
		size_t order0_pending_offset = static_cast<size_t>(-1);
		void* order0_pending_addr = nullptr;

		if (large_msg_queue_.read(req)) {
			// Process from large message queue
			msg = req.msg;
			messages_size = req.len;
		} else {
			// Get new messages from CXL manager. Narrow mutex: copy-out, call without lock, copy-in.
			size_t local_offset;
			{
				absl::MutexLock lock(&(*cached_state_ptr)->mu);
				local_offset = (*cached_state_ptr)->last_offset;
			}
			// Order 2 (total order) and Order 5 (strong order): use batch metadata so subscriber gets total_order
			if (order == 5 || order == 2) {
				size_t batch_total_order = 0;
				uint32_t num_messages = 0;
				if (!topic_manager_->GetBatchToExportWithMetadata(
							topic,
							local_offset,
							msg,
							messages_size,
							batch_total_order,
							num_messages)){
					std::this_thread::yield();
					continue;
				}
				{
					absl::MutexLock lock(&(*cached_state_ptr)->mu);
					(*cached_state_ptr)->last_offset = local_offset;
				}
				batch_meta.batch_total_order = batch_total_order;
				batch_meta.num_messages = num_messages;
				batch_meta.header_version = ((order == 5 || order == 2) && HeaderUtils::ShouldUseBlogHeader()) ? 2 : 1;
				batch_meta.flags = 0;
			} else if (order > 0) {
				if (!topic_manager_->GetBatchToExport(
							topic,
							local_offset,
							msg,
							messages_size)){
					std::this_thread::yield();
					continue;
				}
				{
					absl::MutexLock lock(&(*cached_state_ptr)->mu);
					(*cached_state_ptr)->last_offset = local_offset;
				}
			} else {
			// Order 0: copy-out, call GetMessageAddr without holding mutex (it may spin on next_msg_diff), then copy-in
			size_t local_offset;
			void* local_addr = nullptr;
			{
				absl::MutexLock lock(&(*cached_state_ptr)->mu);
				local_offset = (*cached_state_ptr)->last_offset;
				local_addr = (*cached_state_ptr)->last_addr;
			}
			if (!topic_manager_->GetMessageAddr(topic, local_offset, local_addr, msg, messages_size)) {
				std::this_thread::yield();
				continue;
			}
			// [[BUG_FIX: STATE_AFTER_SEND]] Do NOT update state here; defer until after SendMessageData succeeds.
			order0_pending_offset = local_offset;
			order0_pending_addr = local_addr;
			// [[BUG_FIX: NO_SHARED_QUEUE]] For Order 0, do not use shared large_msg_queue_ (avoids cross-connection fragment mix-up). Send full range; TCP chunks internally.
		}
		}

		// Validate message size
		if (messages_size < 64 && messages_size != 0) {
			LOG(ERROR) << "Message size is below 64 bytes: " << messages_size;
			continue;
		}

		// Order 2 and Order 5: Send batch metadata first (total_order, num_messages) for order-aware consume
		if (order == 5 || order == 2) {
			// batch_meta is already populated by GetBatchToExportWithMetadata
			ssize_t meta_sent = send(sock, &batch_meta, sizeof(batch_meta), MSG_NOSIGNAL);
			if (meta_sent != sizeof(batch_meta)) {
				LOG(ERROR) << "Failed to send batch metadata: " << strerror(errno);
				break;
			}
		}

		if (messages_size > 0) {
			// Send message data
		}

		// Send message data
		if (!SendMessageData(sock, efd, msg, messages_size, zero_copy_send_limit)) {
			LOG(WARNING) << "SubscribeNetworkThread [B" << broker_id_ << "]: SendMessageData failed, "
			             << "messages_size=" << messages_size << ", connection_id=" << connection_id
			             << ". Breaking loop (connection will close).";
			break;  // Connection error - state not advanced, so reconnect can retry from same position
		}

		// [[BUG_FIX: STATE_AFTER_SEND]] Advance state only after successful send (Order 0).
		if (order == 0 && order0_pending_offset != static_cast<size_t>(-1)) {
			absl::MutexLock lock(&(*cached_state_ptr)->mu);
			(*cached_state_ptr)->last_offset = order0_pending_offset;
			(*cached_state_ptr)->last_addr = order0_pending_addr;
		}
	}
}

bool NetworkManager::SendMessageData(
		int sock_fd,
		int epoll_fd,
		void* buffer,
		size_t buffer_size,
		size_t& send_limit) {

	size_t sent_bytes = 0;

	while (sent_bytes < buffer_size) {
		// Edge-triggered: send until EAGAIN (or done), then wait once for EPOLLOUT
		while (sent_bytes < buffer_size) {
			size_t remaining_bytes = buffer_size - sent_bytes;
			size_t to_send = std::min(remaining_bytes, send_limit);
			int send_flags = 0;
			if (to_send >= (1UL << 16)) {
				send_flags = MSG_ZEROCOPY;
			}
			int ret = send(sock_fd, (uint8_t*)buffer + sent_bytes, to_send, send_flags);

			if (ret > 0) {
				sent_bytes += ret;
				send_limit = ZERO_COPY_SEND_LIMIT;
			} else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
				send_limit = std::max(send_limit / 2, 1UL << 16);
				break;  // Wait for epoll
			} else if (ret < 0) {
				LOG(ERROR) << "SendMessageData: send() failed: " << strerror(errno)
				           << ", to_send=" << to_send << ", sent_bytes=" << sent_bytes
				           << ", buffer_size=" << buffer_size;
				return false;
			}
		}
		if (sent_bytes >= buffer_size) {
			break;
		}
		struct epoll_event events[10];
		int n = epoll_wait(epoll_fd, events, 10, -1);
		if (n == -1) {
			LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
			return false;
		}
		for (int i = 0; i < n; ++i) {
			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
				LOG(INFO) << "Socket error or hang-up";
				return false;
			}
		}
	}

	return true;  // All data sent successfully
}

//----------------------------------------------------------------------------
// Acknowledgment Handling
//----------------------------------------------------------------------------

size_t NetworkManager::GetOffsetToAck(const char* topic, uint32_t ack_level){
	// [[CRITICAL: Validate topic is not empty]]
	// Empty topic → wrong TInode → wrong ACKs → client timeout
	// This can happen if AckThread is started with incorrect handshake.topic
	if (!topic || strlen(topic) == 0) {
		LOG(ERROR) << "GetOffsetToAck: Empty or null topic for broker " << broker_id_
		           << " (ack_level=" << ack_level << ")";
		return (size_t)-1;
	}

	// Early return for ack_level 0 (no acknowledgments expected)
	if (ack_level == 0) {
		return (size_t)-1;  // Return sentinel value indicating no ack needed
	}

	TInode* tinode = (TInode*)cxl_manager_->GetTInode(topic);
	if (!tinode) {
		LOG(WARNING) << "GetOffsetToAck: TInode not found for topic '" << topic
		             << "' on broker " << broker_id_;
		return (size_t)-1;  // Topic not found / defensive: GetTInode contract may change
	}
	const int replication_factor = tinode->replication_factor;
	const int order = tinode->order;
	const SequencerType seq_type = tinode->seq_type;
	const int num_brokers = get_num_brokers_callback_();
	size_t min = std::numeric_limits<size_t>::max();

	// Handle ack_level 2 explicitly (ack only after replication)
	if (ack_level == 2 && replication_factor > 0) {
		// ACK Level 2: Only acknowledge after full replication completes
		// [[ACK_LEVEL_2_SEMANTICS]] - Durability guarantee:
		// - Messages are acknowledged only after being replicated to disk on all replicas
		// - Durability is "within periodic sync window" (default: 250ms or 64MiB)
		// - This means ack_level=2 provides eventual durability, not immediate fsync durability

		// [[PHASE_2]] Use CompletionVector instead of replication_done array (design §3.4).
		// For ack_level=2 the tail replica advances CV after replication; max semantics with sequencer.
		// See docs/COMPLETION_VECTOR_ACK_LEVELS.md.
		if (seq_type == EMBARCADERO) {
			CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
				reinterpret_cast<uint8_t*>(cxl_manager_->GetCXLAddr()) + Embarcadero::kCompletionVectorOffset);
			CompletionVectorEntry* my_cv_entry = &cv[broker_id_];

			// Read CV (tail replica updates after replication; sequencer may also advance for ack_level=1)
			// [[CXL_VISIBILITY]] full_fence after flush so read sees CXL; LFENCE does not order CLFLUSHOPT.
			CXL::flush_cacheline(my_cv_entry);
			CXL::full_fence();
			
			// [[ACK_OPTIMIZATION]] Read cumulative message count directly from CV
			uint64_t completed_logical_offset = my_cv_entry->completed_logical_offset.load(std::memory_order_acquire);

			// Sentinel check via pbr_head
			if (completed_logical_offset == 0) {
				uint64_t completed_pbr_index = my_cv_entry->completed_pbr_head.load(std::memory_order_acquire);
				if (completed_pbr_index == static_cast<uint64_t>(-1)) {
					return (size_t)-1;  // No replication progress yet
				}
				return 0;
			}
			
			// [[ACK_LEVEL_2_COUNT_SEMANTICS]] - Client expects message COUNT.
			return completed_logical_offset;
		}

		// [[PHASE_1]] Legacy path: Poll replication_done array for non-EMBARCADERO sequencers
		// [[PERF: Single loop - invalidate then read same broker]] (was 2*replication_factor iterations)
		// Replication threads (DiskManager) write replication_done and flush; AckThread reads it.
		// On non-coherent CXL, reader must invalidate cache to observe those writes.
		size_t r[replication_factor];
		for (int i = 0; i < replication_factor; i++) {
			int b = Embarcadero::GetReplicationSetBroker(broker_id_, replication_factor, num_brokers, i);
			volatile uint64_t* rep_done_ptr = &tinode->offsets[b].replication_done[broker_id_];
			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(rep_done_ptr)));
			CXL::full_fence();  // Ensure invalidation completes before read (CXL visibility)
			r[i] = *rep_done_ptr;  // Read after invalidation
			if (min > r[i]) min = r[i];
		}
		// [[ACK_LEVEL_2_COUNT_SEMANTICS]] - Client expects message COUNT, not last offset.
		// replication_done stores last_logical_offset (0-based). For N messages, last_offset = N-1.
		if (min == std::numeric_limits<size_t>::max()) {
			return (size_t)-1;  // No replication progress yet
		}
		return min + 1;  // Convert last_offset (0-based) to message count
	}

	// [[B0_ACK_FIX]] ACK Level 1 + ORDER=4/5 + EMBARCADERO: Use CompletionVector REGARDLESS of replication_factor.
	// When replication_factor==0 we were taking the else branch and returning tinode->offsets[0].ordered,
	// which has same-process visibility bug (B0 on head never sees sequencer's updates). CV path fixes that.
	if (ack_level == 1 && (order == 4 || order == 5) && seq_type == EMBARCADERO) {
		CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
			reinterpret_cast<uint8_t*>(cxl_manager_->GetCXLAddr()) + Embarcadero::kCompletionVectorOffset);
		CompletionVectorEntry* my_cv_entry = &cv[broker_id_];
		CXL::flush_cacheline(my_cv_entry);
		CXL::full_fence();  // CXL visibility: MFENCE orders CLFLUSHOPT completion before read

		uint64_t completed_logical_offset = my_cv_entry->completed_logical_offset.load(std::memory_order_acquire);
		if (completed_logical_offset == 0) {
			uint64_t completed_pbr_index = my_cv_entry->completed_pbr_head.load(std::memory_order_acquire);
			if (completed_pbr_index == static_cast<uint64_t>(-1)) {
				return (size_t)-1;
			}
			return 0;
		}
		return completed_logical_offset;
	}

	// ACK Level 1: Acknowledge after written to shared memory and ordered (order 0/1/2 or non-EMBARCADERO)
	if(replication_factor > 0){
		if(ack_level == 1){
			if(order == 0){
				// [[CRITICAL_FIX: Invalidate cache before reading written for ORDER=0]]
				// UpdateWrittenForOrder0 flushes from writer; AckThread must invalidate to see it
				volatile uint64_t* written_ptr = &tinode->offsets[broker_id_].written;
				CXL::flush_cacheline(const_cast<const void*>(
					reinterpret_cast<const volatile void*>(written_ptr)));
				CXL::load_fence();  // [[P7]] LFENCE is sufficient for read-after-invalidate on non-coherent CXL
				return tinode->offsets[broker_id_].written;
			}
			// ORDER > 0 fallback: read ordered count (other sequencer types or order 1/2; order 4/5 handled above)
			{
				// [[CRITICAL_FIX: Invalidate cache before reading ordered for ORDER > 0]]
				if (order > 0) {
					volatile uint64_t* ordered_ptr = &tinode->offsets[broker_id_].ordered;
					CXL::flush_cacheline(const_cast<const void*>(
						reinterpret_cast<const volatile void*>(ordered_ptr)));
					CXL::load_fence();  // [[P7]] LFENCE is sufficient for read-after-invalidate on non-coherent CXL
				}
				return tinode->offsets[broker_id_].ordered;
			}
		}

		// Corfu ensures ordered set after replication so do not need to check replication factor
		if(seq_type == CORFU){
			// [[CRITICAL_FIX: Invalidate cache before reading ordered for Corfu]]
			volatile uint64_t* ordered_ptr = &tinode->offsets[broker_id_].ordered;
			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(ordered_ptr)));
			CXL::load_fence();  // [[P7]] LFENCE is sufficient for read-after-invalidate on non-coherent CXL
			return tinode->offsets[broker_id_].ordered;
		}

		// For order=4,5 with EMBARCADERO, use ordered count instead of replication_done
		// because Sequencer4/5 updates ordered counters per broker
		// Fallback: Check replication_done for other cases
		// [[PHASE_3_ALIGN_REPLICATION_SET]] - Use canonical replication set computation
		size_t r[replication_factor];
		for (int i = 0; i < replication_factor; i++) {
			int b = Embarcadero::GetReplicationSetBroker(broker_id_, replication_factor, num_brokers, i);
			r[i] = tinode->offsets[b].replication_done[broker_id_];
			if (min > r[i]) {
				min = r[i];
			}
		}
		return min;
	}else{
		// No replication: Acknowledge after written (order=0) or ordered (order>0)
		if(order == 0){
			// [[CRITICAL_FIX: Invalidate cache before reading written for ORDER=0]]
			// UpdateWrittenForOrder0 flushes from writer; AckThread must invalidate to see it
			volatile uint64_t* written_ptr = &tinode->offsets[broker_id_].written;
			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(written_ptr)));
			CXL::load_fence();  // [[P7]] LFENCE is sufficient for read-after-invalidate on non-coherent CXL
			return tinode->offsets[broker_id_].written;
		}else{
			// [[CRITICAL_FIX: Invalidate cache before reading ordered for ORDER > 0]]
			// full_fence after flush so B0 (and others) see sequencer's ordered updates from CXL
			volatile uint64_t* ordered_ptr = &tinode->offsets[broker_id_].ordered;
			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(ordered_ptr)));
			CXL::load_fence();  // [[P7]] LFENCE is sufficient for read-after-invalidate on non-coherent CXL
			return tinode->offsets[broker_id_].ordered;
		}
	}
}

void NetworkManager::AckThread(const char* topic_cstr, uint32_t ack_level, int ack_fd, int ack_efd) {
	struct epoll_event events[10];
	char buf[1];

	// Create std::string for internal use (required by GetOffsetToAck interface)
	std::string topic(topic_cstr ? topic_cstr : "");

	LOG(INFO) << "AckThread: Starting for broker " << broker_id_ << ", topic='" << topic
	          << "' (len=" << topic.size() << "), ack_level=" << ack_level;

	// [[CRITICAL: Validate topic is not empty]]
	// If topic is empty, GetOffsetToAck will use wrong TInode → wrong ACKs → client timeout
	if (topic.empty()) {
		LOG(ERROR) << "AckThread: Empty topic for broker " << broker_id_
		           << ". Cannot send ACKs correctly. Exiting AckThread.";
		close(ack_fd);
		if (ack_efd >= 0) close(ack_efd);
		return;
	}

	constexpr int kBrokerIdEpollTimeoutMs = 100;
	constexpr int kAckEpollTimeoutMs = 100;
	constexpr int kMaxConsecutiveTimeouts = 20;
	constexpr int kMaxConsecutiveErrors = 5;

	auto reset_ack_epoll = [&](const char* context) -> bool {
		if (ack_efd >= 0) {
			close(ack_efd);
		}
		ack_efd = epoll_create1(0);
		if (ack_efd == -1) {
			LOG(ERROR) << "AckThread: epoll_create1 failed in " << context << ": " << strerror(errno);
			return false;
		}
		struct epoll_event event;
		event.data.fd = ack_fd;
		event.events = EPOLLOUT;
		if (epoll_ctl(ack_efd, EPOLL_CTL_ADD, ack_fd, &event) == -1) {
			LOG(ERROR) << "AckThread: epoll_ctl failed in " << context << ": " << strerror(errno);
			close(ack_efd);
			ack_efd = -1;
			return false;
		}
		return true;
	};

	// Send broker_id first so client can distinguish
	size_t acked_size = 0;
	int consecutive_timeouts = 0;
	int consecutive_errors = 0;
	while (acked_size < sizeof(broker_id_)) {
		if (stop_threads_) {
			break;
		}
		// Add 5-second timeout to prevent infinite blocking if epoll fd is invalid
		int n = epoll_wait(ack_efd, events, 10, kBrokerIdEpollTimeoutMs);
		
		if (n == 0) {
			consecutive_timeouts++;
			LOG(WARNING) << "AckThread: Timeout sending broker_id for broker " << broker_id_
			             << " (timeout " << consecutive_timeouts << "/" << kMaxConsecutiveTimeouts << ")";
			if (consecutive_timeouts >= kMaxConsecutiveTimeouts) {
				if (stop_threads_) break;
				if (!reset_ack_epoll("broker_id_timeout")) {
					close(ack_fd);
					return;
				}
				consecutive_timeouts = 0;
			}
			continue;
		}
		if (n < 0) {
			consecutive_errors++;
			LOG(ERROR) << "AckThread: epoll_wait failed while sending broker_id: " << strerror(errno)
			           << " (error " << consecutive_errors << "/" << kMaxConsecutiveErrors << ")";
			if (consecutive_errors >= kMaxConsecutiveErrors) {
				if (stop_threads_) break;
				if (!reset_ack_epoll("broker_id_error")) {
					close(ack_fd);
					return;
				}
				consecutive_errors = 0;
			}
			continue;
		}
		consecutive_timeouts = 0;
		consecutive_errors = 0;

		for (int i = 0; i < n; i++) {
			if (events[i].events & EPOLLOUT) {
				bool retry;
				do {
					retry = false;
					if (stop_threads_) break;
					ssize_t bytes_sent = send(
							ack_fd,
							(char*)&broker_id_ + acked_size,
							sizeof(broker_id_) - acked_size,
							0);

					if (bytes_sent < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							retry = true;
							continue;
						} else if (errno == EINTR) {
							retry = true;
							continue;
						} else {
							LOG(ERROR) << "Offset acknowledgment failed: " << strerror(errno);
							return;
						}
					} else {
						acked_size += bytes_sent;
					}
				} while (retry && acked_size < sizeof(broker_id_));

				if (acked_size >= sizeof(broker_id_)) {
					break;  // All data sent
				}
			}
		}
	} // end of send loop

	// [[DIAGNOSTIC: Confirm broker_id was sent to client]]
	LOG(INFO) << "AckThread: Broker " << broker_id_ << " sent broker_id to client for topic='" << topic << "'";

	size_t next_to_ack_offset = 0;

	consecutive_timeouts = 0;
	consecutive_errors = 0;
	size_t consecutive_ack_stalls = 0;
	
		while (!stop_threads_) {
		// Simplified polling: single check per iteration to reduce cache operations
		// Trade-off: slightly higher ACK latency (~100μs) for much lower CXL traffic
		bool found_ack = false;
		size_t current_ack = GetOffsetToAck(topic.c_str(), ack_level);

		if (current_ack != (size_t)-1 && next_to_ack_offset <= current_ack) {
			found_ack = true;
			consecutive_ack_stalls = 0;
		}
		
		if (!found_ack) {
			consecutive_ack_stalls++;
			// Simplified backoff: brief pause, then yield after 20 consecutive stalls
			if (consecutive_ack_stalls < 20) {
				// Brief pause for responsiveness
				for (int i = 0; i < 100; ++i) {
					CXL::cpu_pause();
				}
			} else {
				// Yield after many consecutive stalls to prevent starvation
				std::this_thread::yield();
				consecutive_ack_stalls = 0;
			}
			continue;
		}

		// ACK is ready, use the value we already fetched
		size_t ack = current_ack;

		if(ack != (size_t)-1 && next_to_ack_offset <= ack){
			next_to_ack_offset = ack + 1;
			acked_size = 0;
			// Send offset acknowledgment
			// Add timeout to epoll_wait to prevent infinite blocking
			while (acked_size < sizeof(ack)) {
				if (stop_threads_) break;
				int n = epoll_wait(ack_efd, events, 10, kAckEpollTimeoutMs);
				if (n == 0) {
					consecutive_timeouts++;
					if (consecutive_timeouts >= kMaxConsecutiveTimeouts) {
						if (stop_threads_) break;
						LOG(WARNING) << "AckThread: repeated ACK send timeouts for broker " << broker_id_
						             << ", resetting epoll";
						if (!reset_ack_epoll("ack_timeout")) {
							close(ack_fd);
							return;
						}
						consecutive_timeouts = 0;
					}
					continue;
				}
				if (n < 0) {
					consecutive_errors++;
					LOG(ERROR) << "AckThread: epoll_wait failed while sending ack: " << strerror(errno)
					           << " (error " << consecutive_errors << "/" << kMaxConsecutiveErrors << ")";
					if (consecutive_errors >= kMaxConsecutiveErrors) {
						if (stop_threads_) break;
						if (!reset_ack_epoll("ack_error")) {
							close(ack_fd);
							return;
						}
						consecutive_errors = 0;
					}
					continue;
				}
				consecutive_timeouts = 0;
				consecutive_errors = 0;

				for (int i = 0; i < n; i++) {
					if (events[i].events & EPOLLOUT) {
						bool retry;
						do {
							retry = false;
							if (stop_threads_) break;
							ssize_t bytes_sent = send(
									ack_fd,
									(char*)&ack + acked_size,
									sizeof(ack) - acked_size,
									0);

							if (bytes_sent < 0) {
								if (errno == EAGAIN || errno == EWOULDBLOCK) {
									retry = true;
									continue;
								} else if (errno == EINTR) {
									retry = true;
									continue;
								} else {
									LOG(ERROR) << "Offset acknowledgment failed: " << strerror(errno);
									return;
								}
							} else {
								acked_size += bytes_sent;
							}
						} while (retry && acked_size < sizeof(ack));

						if (acked_size >= sizeof(ack)) {
							break;  // All data sent
						}
					}
				}
			} // end of send loop
		}else{
			// Check if connection is still alive
			if (!IsConnectionAlive(ack_fd, buf)) {
				LOG(INFO) << "Acknowledgment connection closed: " << ack_fd;
				LOG(INFO) << "AckThread for broker " << broker_id_ << " exiting - publisher disconnected";
				// CRITICAL FIX: Don't shut down the entire broker when publisher disconnects
				// Subscriber connections should remain active and independent
				break;
			}
		}
	}// end of while loop
	close(ack_fd);
	if (ack_efd >= 0) {
		close(ack_efd);
	}
}


} // namespace Embarcadero
