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

#include <glog/logging.h>
#include "mimalloc.h"

#include "network_manager.h"
#include "staging_pool.h"
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
 * Configures a socket for non-blocking operation with TCP optimizations
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

	// Increase socket buffers for high-throughput (32MB)
	const int buffer_size = 32 * 1024 * 1024;  // 32 MB
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_SNDBUF) failed: " << strerror(errno);
		// Non-fatal, continue (will use default buffer size)
	}
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_RCVBUF) failed: " << strerror(errno);
		// Non-fatal, continue (will use default buffer size)
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
	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	server_addr.sin_addr.s_addr = inet_addr(inet_ntoa(client_address.sin_addr));

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
			// Connection succeeded immediately
			break;
		}

		if (errno != EINPROGRESS) {
			LOG(ERROR) << "Connect failed: " << strerror(errno);
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
				break;
			} else {
				LOG(ERROR) << "Connection failed: " << strerror(sock_error);
			}
		} else if (n == 0) {
			// Timeout occurred
			LOG(ERROR) << "Connection timed out, retrying...";
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
		LOG(ERROR) << "Max retries reached. Connection failed.";
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

		// Initialize non-blocking architecture if enabled
		const auto& config = GetConfig().config();
		bool use_nonblocking = config.network.use_nonblocking.get();

		if (use_nonblocking) {
			// Initialize staging pool
			size_t buffer_size_mb = config.network.staging_pool_buffer_size_mb.get();
			size_t num_buffers = config.network.staging_pool_num_buffers.get();
			size_t buffer_size = buffer_size_mb * 1024 * 1024;

			staging_pool_ = std::make_unique<StagingPool>(buffer_size, num_buffers);

			// Initialize CXL allocation queue (128 pending batches)
			cxl_allocation_queue_ = std::make_unique<folly::MPMCQueue<PendingBatch>>(128);

			// Initialize publish connection queue (64 pending connections)
			publish_connection_queue_ = std::make_unique<folly::MPMCQueue<NewPublishConnection>>(64);

			LOG(INFO) << "NetworkManager: Non-blocking mode enabled "
			          << "(staging_pool=" << num_buffers << "×" << buffer_size_mb << "MB)";
		} else {
			LOG(INFO) << "NetworkManager: Using blocking mode (EMBARCADERO_USE_NONBLOCKING=0)";
		}

		// Create main listener thread
		threads_.emplace_back(&NetworkManager::MainThread, this);

		// Create request handler threads
		for (int i = 0; i < num_reqReceive_threads; i++) {
			threads_.emplace_back(&NetworkManager::ReqReceiveThread, this);
		}

		// Create non-blocking architecture threads if enabled
		if (use_nonblocking) {
			int num_recv_threads = config.network.num_publish_receive_threads.get();
			int num_cxl_workers = config.network.num_cxl_allocation_workers.get();

			// Launch PublishReceiveThreads
			for (int i = 0; i < num_recv_threads; i++) {
				threads_.emplace_back(&NetworkManager::PublishReceiveThread, this);
			}

			// Launch CXLAllocationWorkers
			for (int i = 0; i < num_cxl_workers; i++) {
				threads_.emplace_back(&NetworkManager::CXLAllocationWorker, this);
			}

			LOG(INFO) << "NetworkManager: Launched " << num_recv_threads
			          << " PublishReceiveThreads + " << num_cxl_workers
			          << " CXLAllocationWorkers";
		}

		// Wait for all threads to start
		while (thread_count_.load() != (1 + num_reqReceive_threads_)) {
			// Busy wait until all threads are ready
		}

		VLOG(3) << "[NetworkManager]: Constructed with " << num_reqReceive_threads_
			<< " request threads for broker " << broker_id_;
	}

NetworkManager::~NetworkManager() {
	// Signal threads to stop
	stop_threads_ = true;

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

	VLOG(3) << "[NetworkManager]: Destructed";
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
	const int EPOLL_TIMEOUT_MS = 1;  // 1 millisecond timeout

	while (!stop_threads_) {
		// PERFORMANCE OPTIMIZATION: Reduced timeout for better responsiveness
		int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);  // 100ms instead of EPOLL_TIMEOUT_MS
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
					LOG(ERROR) << "Error receiving handshake: " << strerror(errno);
				}
				close(req.client_socket);
				return;
			}
			read_total += static_cast<size_t>(ret);
		}

		// Ensure topic string is terminated to avoid strlen overrun
		handshake.topic[sizeof(handshake.topic) - 1] = '\0';

		// Check if non-blocking mode is enabled
		bool use_nonblocking = GetConfig().config().network.use_nonblocking.get();

		// Process based on request type
		switch (handshake.client_req) {
			case Publish:
				// Phase 2: Route to non-blocking PublishReceiveThread if enabled
				if (use_nonblocking && publish_connection_queue_) {
					// Enqueue connection for non-blocking handling
					NewPublishConnection new_conn(req.client_socket, handshake, client_address);
					if (!publish_connection_queue_->write(new_conn)) {
						// Queue full - fall back to blocking mode instead of closing connection
						static std::atomic<size_t> nonblocking_queue_full_count{0};
						size_t count = nonblocking_queue_full_count.fetch_add(1, std::memory_order_relaxed) + 1;
						if (count <= 10 || count % 100 == 0) {
							LOG(WARNING) << "ReqReceiveThread: Non-blocking queue full, falling back to blocking mode "
							          << "(client_id=" << handshake.client_id << ", count=" << count << ")";
						}
						// Fall back to blocking mode
						HandlePublishRequest(req.client_socket, handshake, client_address);
					} else {
						metric_connections_routed_.fetch_add(1, std::memory_order_relaxed);
						VLOG(2) << "ReqReceiveThread: Enqueued publish connection for non-blocking handling "
						       << "(fd=" << req.client_socket << ", client_id=" << handshake.client_id
						       << ", topic=" << handshake.topic << ")";
					}
				} else {
					// Blocking path (either disabled or initialization failed)
					if (use_nonblocking && !publish_connection_queue_) {
						static bool warned = false;
						if (!warned) {
							LOG(ERROR) << "ReqReceiveThread: Non-blocking mode enabled but queue not initialized! "
							          << "Falling back to blocking mode.";
							warned = true;
						}
					}
					HandlePublishRequest(req.client_socket, handshake, client_address);
				}
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
	static std::atomic<size_t> batches_received_complete{0};
	static std::atomic<size_t> batches_marked_complete{0};

	// Validate topic
	if (strlen(handshake.topic) == 0) {
		LOG(ERROR) << "Topic cannot be null";
		close(client_socket);
		return;
	}

	// Setup acknowledgment channel if needed
	int ack_fd = client_socket;

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
				LOG(ERROR) << "Error receiving batch header: " << strerror(errno);
				running = false;
				return;
			}
			bytes_read += recv_ret;
		}

		// Allocate buffer for message batch
		size_t to_read = batch_header.total_size;
		void* segment_header = nullptr;
		void* buf = nullptr;
		size_t logical_offset;
		SequencerType seq_type;
		BatchHeader* batch_header_location = nullptr;

		std::function<void(void*, size_t)> non_emb_seq_callback = nullptr;

		// Use GetCXLBuffer for batch-level allocation and zero-copy receive
		// With ring gating enabled, this may return nullptr if ring is full
		// Retry with exponential backoff instead of closing connection
		constexpr int MAX_CXL_RETRIES_BLOCKING = 20;
		int cxl_retry_count = 0;

		while (cxl_retry_count < MAX_CXL_RETRIES_BLOCKING) {
			non_emb_seq_callback = cxl_manager_->GetCXLBuffer(batch_header, handshake.topic, buf,
					segment_header, logical_offset, seq_type, batch_header_location);

			if (buf != nullptr) {
				break;  // Success
			}

			// Ring full - retry with exponential backoff
			cxl_retry_count++;
			static std::atomic<size_t> blocking_ring_full_count{0};
			size_t total_ring_full = blocking_ring_full_count.fetch_add(1, std::memory_order_relaxed) + 1;

			if (cxl_retry_count >= MAX_CXL_RETRIES_BLOCKING) {
				LOG(ERROR) << "NetworkManager (blocking): Failed to get CXL buffer after "
				           << MAX_CXL_RETRIES_BLOCKING << " retries (ring full, total_count="
				           << total_ring_full << "). Closing connection to client_id="
				           << handshake.client_id;
				break;
			}

			// Log first few and periodic retries
			if (total_ring_full <= 10 || total_ring_full % 1000 == 0) {
				LOG(WARNING) << "NetworkManager (blocking): Ring full, retry " << cxl_retry_count
				             << " for client_id=" << handshake.client_id
				             << " (total_ring_full=" << total_ring_full << ")";
			}

			// Exponential backoff: 1ms, 2ms, 4ms, 8ms, 16ms, 32ms, 64ms, 128ms, ...
			int backoff_ms = 1 << std::min(cxl_retry_count - 1, 7);
			std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
		}

		if (!buf) {
			// Still failed after retries - close connection
			break;
		}
		
		if (batch_header_location == nullptr) {
			// [[SENIOR_ASSESSMENT_FIX]] Log ERROR + counter for ORDER=5 visibility; batch will not be sequenced
			static std::atomic<size_t> batch_header_location_null_count{0};
			size_t cnt = batch_header_location_null_count.fetch_add(1, std::memory_order_relaxed) + 1;
			LOG(ERROR) << "NetworkManager: GetCXLBuffer returned null batch_header_location (count=" << cnt
			           << ") topic=" << handshake.topic << " seq_type=" << static_cast<int>(seq_type)
			           << " batch_seq=" << batch_header.batch_seq << " — batch will not be sequenced or acked";
		} else {
			static thread_local size_t getcxl_logs = 0;
			if (++getcxl_logs % 10000 == 0) {
				VLOG(1) << "NetworkManager: GetCXLBuffer batch_header_location=" << batch_header_location
				        << " batch_seq=" << batch_header.batch_seq << " num_msg=" << batch_header.num_msg
				        << " (log_count=" << getcxl_logs << ")";
			}
		}

		// Receive message data (byte-accurate accounting only)
		size_t read = 0;
		bool batch_data_complete = false;
		while (running && !stop_threads_) {
			bytes_read = recv(client_socket, (uint8_t*)buf + read, to_read, 0);
			if (bytes_read < 0) {
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
		size_t completed_batches = batches_received_complete.fetch_add(1, std::memory_order_relaxed) + 1;
		// [[PERF: VLOG for progress - LOG(INFO) every 200 was hot-path I/O]]
		if (completed_batches <= 10 || completed_batches % 5000 == 0) {
			VLOG(1) << "NetworkManager: batch_data_complete total=" << completed_batches
			        << " last_batch_seq=" << batch_header.batch_seq
			        << " client_id=" << batch_header.client_id
			        << " total_size=" << batch_header.total_size;
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

	// [[BLOG_HEADER: Lightweight sanity check (no per-message conversion)]]
	// Since publisher now emits BlogMessageHeader directly when EMBARCADERO_USE_BLOG_HEADER=1,
	// receiver no longer needs to convert v1→v2. Just validate boundaries and sizes.
	TInode* tinode = nullptr;
	bool is_blog_header_enabled = false;
	if (seq_type == EMBARCADERO) {
		tinode = (TInode*)cxl_manager_->GetTInode(handshake.topic);
		if (tinode && tinode->order == 5 && HeaderUtils::ShouldUseBlogHeader()) {
			is_blog_header_enabled = true;
		}
	}

	if (is_blog_header_enabled && batch_header.num_msg > 0) {
		// [[BLOG_HEADER: Receiver Stage - Set receiver region fields (bytes 0-15)]]
		// Paper spec §3.1: Receiver sets received=1 and ts when payload write completes
		// This is done here after batch is fully received (zero-copy into CXL)
		BlogMessageHeader* current_msg = reinterpret_cast<BlogMessageHeader*>(buf);
		uint64_t receive_timestamp = Embarcadero::CXL::rdtsc();  // Single timestamp for batch
		size_t remaining_bytes = batch_header.total_size;
		
		for (size_t i = 0; i < batch_header.num_msg; ++i) {
			// Validate message header bounds
			if (!wire::ValidateV2Payload(current_msg->size, remaining_bytes)) {
				VLOG(2) << "NetworkManager: Message " << i << " has invalid size=" << current_msg->size;
				break;
			}
			
			// Set receiver region fields (bytes 0-15) - Paper spec Stage 1
			current_msg->received = 1;  // Mark as received
			current_msg->ts = receive_timestamp;  // Receipt timestamp
			
			// Compute stride and verify alignment
			size_t stride = wire::ComputeStrideV2(current_msg->size);
			if (stride % 64 != 0) {
				VLOG(2) << "NetworkManager: Message " << i << " stride " << stride << " not 64B aligned";
				break;
			}
			if (stride > remaining_bytes) {
				VLOG(2) << "NetworkManager: Message " << i << " stride " << stride
				        << " exceeds remaining_bytes " << remaining_bytes;
				break;
			}
			
			// Move to next message
			if (i < batch_header.num_msg - 1) {
				current_msg = reinterpret_cast<BlogMessageHeader*>(
					reinterpret_cast<uint8_t*>(current_msg) + stride
				);
			}
			remaining_bytes -= stride;
		}
		VLOG(3) << "NetworkManager: Set receiver fields for " << batch_header.num_msg << " BlogMessageHeader messages for ORDER=5";
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
		// For Sequencer 5, batch is complete once all data is received (to_read == 0)
		// Now it's safe to mark batch as complete for ALL order levels
		__atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE);
		// [[CRITICAL: Flush batch_complete for non-coherent CXL visibility]]
		// Sequencer (running on different broker/CPU) must see batch_complete update
		// Without flush, sequencer will never see batch_complete=1 and batches won't be processed
		CXL::flush_cacheline(batch_header_location);
		// Flush the second cache line too to ensure all BatchHeader fields are visible
		const void* batch_header_next_line = reinterpret_cast<const void*>(
			reinterpret_cast<const uint8_t*>(batch_header_location) + 64);
		CXL::flush_cacheline(batch_header_next_line);
		CXL::store_fence();
		size_t marked_batches = batches_marked_complete.fetch_add(1, std::memory_order_relaxed) + 1;
		// [[PERF: VLOG for progress - LOG(INFO) every 200 was hot-path I/O]]
		if (marked_batches <= 10 || marked_batches % 5000 == 0) {
			VLOG(1) << "NetworkManager: batch_complete stores=" << marked_batches
			        << " last_batch_seq=" << batch_header.batch_seq
			        << " client_id=" << batch_header.client_id;
		}
		VLOG(4) << "NetworkManager: Marked batch complete for " << batch_header.num_msg << " messages, client_id=" << batch_header.client_id << ", order_level=" << seq_type;
		static thread_local size_t batch_complete_logs = 0;
		if (++batch_complete_logs <= 10 || batch_complete_logs % 5000 == 0) {
			VLOG(1) << "NetworkManager: batch_complete=1 batch_seq=" << batch_header.batch_seq
			        << " num_msg=" << batch_header.num_msg
			        << " total_size=" << batch_header.total_size
			        << " client_id=" << batch_header.client_id
			        << " (total_complete=" << batch_complete_logs << ")";
		}
	} else {
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
				//TODO(Jae) Replication ack
			}
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
	event.events = EPOLLOUT;
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
		sub_state_.erase(unique_connection_id);
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
	int order = topic_manager_->GetTopicOrder(topic);

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

	// PERFORMANCE OPTIMIZATION: Cache state pointer to avoid repeated hash map lookups
	std::unique_ptr<SubscriberState>* cached_state_ptr = nullptr;
	{
		absl::MutexLock lock(&sub_mu_);
		auto it = sub_state_.find(connection_id);
		if (it == sub_state_.end() || !it->second) {
			LOG(ERROR) << "SubscribeNetworkThread: No state found for connection_id " << connection_id;
			return;
		}
		cached_state_ptr = &it->second;
	}

	while (!stop_threads_) {
		// Get message data to send
		void* msg = nullptr;
		size_t messages_size = 0;
		struct LargeMsgRequest req;

		if (large_msg_queue_.read(req)) {
			// Process from large message queue
			msg = req.msg;
			messages_size = req.len;
			VLOG(3) << "[DEBUG] poped from queue:" << messages_size;
		} else {
			// Get new messages from CXL manager
			// PERFORMANCE OPTIMIZATION: Use cached state pointer (no hash map lookup)
			absl::MutexLock lock(&(*cached_state_ptr)->mu);

			if (order == 5) {
			// For Sequencer 5: Get batch with metadata
			size_t batch_total_order = 0;
			uint32_t num_messages = 0;
			static int no_data_count = 0;
			if (!topic_manager_->GetBatchToExportWithMetadata(
						topic,
						(*cached_state_ptr)->last_offset,
						msg,
						messages_size,
						batch_total_order,
						num_messages)){
				no_data_count++;
				if (no_data_count % 1000 == 0) {
					LOG(INFO) << "SubscribeNetworkThread: No data available for export yet (count=" << no_data_count 
					          << "), topic=" << topic << ", last_offset=" << (*cached_state_ptr)->last_offset;
				}
				std::this_thread::yield();
				continue;
			}
			no_data_count = 0;  // Reset counter when data becomes available
			// Store metadata for sending
			batch_meta.batch_total_order = batch_total_order;
			batch_meta.num_messages = num_messages;
			// [[BLOG_HEADER: Set header_version based on feature flag]]
			batch_meta.header_version = (order == 5 && HeaderUtils::ShouldUseBlogHeader()) ? 2 : 1;
			batch_meta.flags = 0;
			// [[PERF: Demoted from LOG(INFO) to VLOG(2) - this is hot path per batch]]
			VLOG(2) << "SubscribeNetworkThread: Sending batch metadata, total_order=" << batch_total_order 
			        << ", num_messages=" << num_messages << ", header_version=" << batch_meta.header_version 
			        << ", topic=" << topic;
			} else if (order > 0){
				if (!topic_manager_->GetBatchToExport(
							topic,
							(*cached_state_ptr)->last_offset,
							msg,
							messages_size)){
						std::this_thread::yield();
						continue;
}
		}else{
			if (topic_manager_->GetMessageAddr(
								topic,
								(*cached_state_ptr)->last_offset,
								(*cached_state_ptr)->last_addr,
								msg,
								messages_size)) {
					// [[BLOG_HEADER: Split large messages with version-aware boundary]]
					// Split large messages into chunks for better flow control
					bool using_blog_header = (order == 5 && HeaderUtils::ShouldUseBlogHeader());
					
					while (messages_size > zero_copy_send_limit) {
						struct LargeMsgRequest r;
						r.msg = msg;

						// Ensure we don't cut in the middle of a message
						// For v2 BlogMessageHeader, compute size from BlogMessageHeader fields
						size_t padded_size = 0;
						if (using_blog_header) {
							Embarcadero::BlogMessageHeader* v2_hdr = 
								reinterpret_cast<Embarcadero::BlogMessageHeader*>(msg);
							// Compute padded size from v2 fields
							size_t payload_size = v2_hdr->size;
							if (!wire::ValidateV2Payload(payload_size, messages_size)) {
								LOG(ERROR) << "SubscribeNetworkThread: Invalid v2 payload_size=" << payload_size
								           << " for splitting, messages_size=" << messages_size;
								break;
							}
							padded_size = wire::ComputeStrideV2(payload_size);
						} else {
							// v1 MessageHeader uses paddedSize
							MessageHeader* v1_hdr = reinterpret_cast<MessageHeader*>(msg);
							if (!wire::ValidateV1PaddedSize(v1_hdr->paddedSize, messages_size)) {
								LOG(ERROR) << "SubscribeNetworkThread: Invalid v1 paddedSize=" << v1_hdr->paddedSize
								           << " for splitting, messages_size=" << messages_size;
								break;
							}
							padded_size = v1_hdr->paddedSize;
						}
						
						if (padded_size == 0) {
							LOG(ERROR) << "SubscribeNetworkThread: Invalid message size, skipping to avoid SIGFPE";
							break; // Exit the large message splitting loop
						}
						int mod = zero_copy_send_limit % padded_size;
							r.len = zero_copy_send_limit - mod;

							large_msg_queue_.blockingWrite(r);
							msg = (uint8_t*)msg + r.len;
							messages_size -= r.len;
						}
			} else {
				// No new messages, yield and try again
				std::this_thread::yield();
				continue;
			}
		}
		}

		// Validate message size
		if (messages_size < 64 && messages_size != 0) {
			LOG(ERROR) << "Message size is below 64 bytes: " << messages_size;
			continue;
		}

		// For Sequencer 5: Send batch metadata first if order > 0
		if (order == 5) {
			// batch_meta is already populated by GetBatchToExportWithMetadata
			
			// Send batch metadata
			ssize_t meta_sent = send(sock, &batch_meta, sizeof(batch_meta), MSG_NOSIGNAL);
			if (meta_sent != sizeof(batch_meta)) {
				LOG(ERROR) << "Failed to send batch metadata: " << strerror(errno);
				break;
			}
		}

		// Send message data
		if (!SendMessageData(sock, efd, msg, messages_size, zero_copy_send_limit)) {
			break;  // Connection error
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
		// Wait for socket to be writable
		struct epoll_event events[10];
		int n = epoll_wait(epoll_fd, events, 10, -1);

		if (n == -1) {
			LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
			return false;
		}

		for (int i = 0; i < n; ++i) {
			if (events[i].events & EPOLLOUT) {
				// Calculate how much to send in this iteration
				size_t remaining_bytes = buffer_size - sent_bytes;
				size_t to_send = std::min(remaining_bytes, send_limit);

				// Send data
				int ret;
				if (to_send < 1UL << 16) { // < 64KB
					ret = send(sock_fd, (uint8_t*)buffer + sent_bytes, to_send, 0);
				} else {
					ret = send(sock_fd, (uint8_t*)buffer + sent_bytes, to_send, 0);
					// Could use MSG_ZEROCOPY for large messages if needed
				}

				if (ret > 0) {
					// Data sent successfully
					sent_bytes += ret;
					send_limit = ZERO_COPY_SEND_LIMIT;  // Reset to default
				} else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
					// Would block, reduce send size and retry
					send_limit = std::max(send_limit / 2, 1UL << 16);  // Cap at 64K
					continue;
				} else if (ret < 0) {
					// Fatal error
					LOG(ERROR) << "Error sending data: " << strerror(errno)
						<< ", to_send: " << to_send;
					return false;
				}
			} else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
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
		// [[PERF: Single loop - invalidate then read same broker]] (was 2*replication_factor iterations)
		// Replication threads (DiskManager) write replication_done and flush; AckThread reads it.
		// On non-coherent CXL, reader must invalidate cache to observe those writes.
		size_t r[replication_factor];
		for (int i = 0; i < replication_factor; i++) {
			int b = Embarcadero::GetReplicationSetBroker(broker_id_, replication_factor, num_brokers, i);
			volatile uint64_t* rep_done_ptr = &tinode->offsets[b].replication_done[broker_id_];
			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(rep_done_ptr)));
			r[i] = *rep_done_ptr;  // Read after invalidation
			if (min > r[i]) min = r[i];
		}
		CXL::load_fence();
		// [[ACK_LEVEL_2_COUNT_SEMANTICS]] - Client expects message COUNT, not last offset.
		// replication_done stores last_logical_offset (0-based). For N messages, last_offset = N-1.
		if (min == std::numeric_limits<size_t>::max()) {
			return (size_t)-1;  // No replication progress yet
		}
		return min + 1;  // Convert last_offset (0-based) to message count
	}

	// ACK Level 1: Acknowledge after written to shared memory and ordered
	if(replication_factor > 0){
		if(ack_level == 1){
			if(order == 0){
				return tinode->offsets[broker_id_].written;
			}else{
				// [[CRITICAL_FIX: Invalidate cache before reading ordered for ORDER > 0]]
				// Same issue as ORDER=4/5: sequencer updates ordered, AckThread must invalidate
				if (order > 0) {
					volatile uint64_t* ordered_ptr = &tinode->offsets[broker_id_].ordered;
					CXL::flush_cacheline(const_cast<const void*>(
						reinterpret_cast<const volatile void*>(ordered_ptr)));
					CXL::load_fence();
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
			CXL::load_fence();
			return tinode->offsets[broker_id_].ordered;
		}

		// For order=4,5 with EMBARCADERO, use ordered count instead of replication_done
		// because Sequencer4/5 updates ordered counters per broker
		if((order == 4 || order == 5) && seq_type == EMBARCADERO){
			// [[CRITICAL_FIX: Invalidate cache before reading ordered]]
			// Sequencer (head broker) updates ordered and flushes
			// AckThread (this broker) must invalidate cache to see updates
			// Without invalidation, GetOffsetToAck() returns stale cached value
			// This causes ACK condition to fail even though ordered has increased
			// This is the root cause of last 1.9% ACK stall!
			volatile uint64_t* ordered_ptr = &tinode->offsets[broker_id_].ordered;
			// [[DIAGNOSTIC: Log cache invalidation for ORDER=4/5]]
			static thread_local size_t invalidation_count = 0;
			static thread_local size_t last_logged_ordered = 0;
			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(ordered_ptr)));
			CXL::load_fence();
			size_t ordered_value = tinode->offsets[broker_id_].ordered;
			if (++invalidation_count % 1000 == 0 || ordered_value != last_logged_ordered) {
				VLOG(2) << "GetOffsetToAck[ORDER=4/5] B" << broker_id_ << ": invalidated cache, ordered=" 
				        << ordered_value << " (count=" << invalidation_count << ")";
				last_logged_ordered = ordered_value;
			}
			return ordered_value;
		}

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
			return tinode->offsets[broker_id_].written;
		}else{
			// [[CRITICAL_FIX: Invalidate cache before reading ordered for ORDER > 0]]
			volatile uint64_t* ordered_ptr = &tinode->offsets[broker_id_].ordered;
			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(ordered_ptr)));
			CXL::load_fence();
			return tinode->offsets[broker_id_].ordered;
		}
	}
}

void NetworkManager::AckThread(const char* topic, uint32_t ack_level, int ack_fd, int ack_efd) {
	struct epoll_event events[10];
	char buf[1];

	LOG(INFO) << "AckThread: Starting for broker " << broker_id_ << ", topic='" << topic
	          << "' (len=" << (topic ? strlen(topic) : 0) << "), ack_level=" << ack_level;

	// [[CRITICAL: Validate topic is not empty]]
	// If topic is empty, GetOffsetToAck will use wrong TInode → wrong ACKs → client timeout
	if (!topic || strlen(topic) == 0) {
		LOG(ERROR) << "AckThread: Empty or null topic for broker " << broker_id_
		           << ". Cannot send ACKs correctly. Exiting AckThread.";
		close(ack_fd);
		if (ack_efd >= 0) close(ack_efd);
		return;
	}

	constexpr int kBrokerIdEpollTimeoutMs = 5000;
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
		// Add 5-second timeout to prevent infinite blocking if epoll fd is invalid
		int n = epoll_wait(ack_efd, events, 10, kBrokerIdEpollTimeoutMs);
		
		if (n == 0) {
			consecutive_timeouts++;
			LOG(WARNING) << "AckThread: Timeout sending broker_id for broker " << broker_id_
			             << " (timeout " << consecutive_timeouts << "/" << kMaxConsecutiveTimeouts << ")";
			if (consecutive_timeouts >= kMaxConsecutiveTimeouts) {
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

	size_t next_to_ack_offset = 0;
	auto last_log_time = std::chrono::steady_clock::now();

	consecutive_timeouts = 0;
	consecutive_errors = 0;
	static thread_local size_t consecutive_ack_stalls = 0;
	constexpr size_t kAckInvalidationThreshold = 1000;  // Invalidate after 1000 consecutive stalls (for ack_level=2)
	constexpr size_t kAck1InvalidationThreshold = 100;   // Invalidate after 100 consecutive stalls (for ack_level=1, ORDER>0)
	
		while (!stop_threads_) {
		auto cycle_start = std::chrono::steady_clock::now();
		// [[CONFIG: AckThread spin/drain/sleep]] - Env overrides for CXL/sequencer tuning.
		// EMBARCADERO_ACK_SPIN_US, _DRAIN_US, _SLEEP_LIGHT_US, _SLEEP_HEAVY_MS, _HEAVY_SLEEP_STALL_THRESHOLD
		const int spin_us = []() {
			const char* e = std::getenv("EMBARCADERO_ACK_SPIN_US");
			return e ? std::atoi(e) : 500;
		}();
		const auto SPIN_DURATION = std::chrono::microseconds(spin_us > 0 ? spin_us : 500);

		// Spin-then-sleep: spin first to catch ACK updates; only sleep when no progress.
		// Sleep duration is adaptive: light when recently active, heavy when long idle (stall threshold).
		bool found_ack = false;
		
		// [[PHASE_1_FIX_READER_INVALIDATION]] - Periodic cache invalidation for ack_level=1 and ack_level=2
		// For ack_level=1 with ORDER > 0: Sequencer (head broker) writes ordered count, AckThread (this broker) reads it
		// For ack_level=2: Replication threads write replication_done, AckThread reads it
		// On non-coherent CXL, we must invalidate our local cache to observe remote writes
		TInode* tinode = (TInode*)cxl_manager_->GetTInode(topic);
		if (tinode) {
			size_t invalidation_threshold = (ack_level == 1 && tinode->order > 0) 
				? kAck1InvalidationThreshold 
				: kAckInvalidationThreshold;
			
			if (consecutive_ack_stalls >= invalidation_threshold) {
				if (ack_level == 2 && tinode->replication_factor > 0) {
					int num_brokers = get_num_brokers_callback_();
					// Invalidate replication_done cache lines for all replicas in the set
					for (int i = 0; i < tinode->replication_factor; i++) {
						int b = GetReplicationSetBroker(broker_id_, tinode->replication_factor, num_brokers, i);
						volatile uint64_t* rep_done_ptr = &tinode->offsets[b].replication_done[broker_id_];
						CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(rep_done_ptr)));
					}
					CXL::load_fence();
					consecutive_ack_stalls = 0;
				} else if (ack_level == 1 && tinode->order > 0) {
					// [[CRITICAL FIX: ACK=1 Cache Invalidation for ORDER > 0]]
					// For ORDER=4/5, sequencer (head broker) updates tinode->offsets[broker_id_].ordered
					// This broker's AckThread must invalidate cache to see sequencer's updates
					// The sequencer region (ordered, ordered_offset) is at offset 512 within offset_entry
					// [[PERF: Read AFTER invalidation, not before]] - Reading before flush defeats the purpose
					volatile uint64_t* ordered_ptr = &tinode->offsets[broker_id_].ordered;
					CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(ordered_ptr)));
					CXL::load_fence();
					// Now read the fresh value after invalidation
					size_t ordered_after = *ordered_ptr;
					VLOG(2) << "AckThread B" << broker_id_ << ": Cache invalidated, ordered=" << ordered_after;
					consecutive_ack_stalls = 0;
				}
			}
		}
		
		// [[SENIOR_REVIEW_FIX: Removed redundant cache invalidation from spin loop]]
		// GetOffsetToAck() already invalidates cache internally before reading ordered
		// Having periodic invalidation here causes DOUBLE invalidation (redundant!)
		// Performance impact: Unnecessary clflushopt + lfence overhead
		// GetOffsetToAck() handles invalidation correctly, so we don't need it here
		
		// [[SENIOR_REVIEW_FIX: Cache GetOffsetToAck() result to avoid redundant calls]]
		// GetOffsetToAck() invalidates cache internally, so we cache the result
		// and re-use it for logging and ACK decision to avoid redundant invalidations
		size_t cached_ack = (size_t)-1;
		
		while (std::chrono::steady_clock::now() - cycle_start < SPIN_DURATION) {
			// GetOffsetToAck() invalidates cache internally - no need for separate invalidation
			cached_ack = GetOffsetToAck(topic, ack_level);
			if (cached_ack != (size_t)-1 && next_to_ack_offset <= cached_ack) {
				found_ack = true;
				consecutive_ack_stalls = 0;  // Reset on success
				break;  // ACK is ready, exit spin loop and send immediately
			}
			CXL::cpu_pause();  // Efficient spin-wait on CXL
		}
		
		if (!found_ack) {
			consecutive_ack_stalls++;
		}

		// If no ACK found after spinning, log progress and sleep briefly
		// [[SENIOR_REVIEW_FIX: Use cached GetOffsetToAck() result]]
		// [[PERF: Reuse tinode from top of cycle - avoid second GetTInode() in 3s log block]]
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 3) {
			// Use cached value from spin loop (or re-fetch if not available)
			size_t ack = (cached_ack != (size_t)-1) ? cached_ack : GetOffsetToAck(topic, ack_level);
			size_t ordered = tinode ? tinode->offsets[broker_id_].ordered : 0;
			
			// [[PHASE_0_INSTRUMENTATION]] - Enhanced ACK diagnostics
			LOG(INFO) << "Broker:" << broker_id_ << " Acknowledgments " << ack
			          << " (next_to_ack=" << next_to_ack_offset
			          << ", ordered=" << ordered << ", ack_level=" << ack_level << ")";
			if (tinode) {
				int replication_factor = tinode->replication_factor;
				int num_brokers = get_num_brokers_callback_();
				LOG(INFO) << "[AckThread B" << broker_id_ << "]: Replication set (factor=" 
					<< replication_factor << ", num_brokers=" << num_brokers << "):";
				for (int i = 0; i < replication_factor; i++) {
					int b = GetReplicationSetBroker(broker_id_, replication_factor, num_brokers, i);
					size_t rep_done = tinode->offsets[b].replication_done[broker_id_];
					LOG(INFO) << "\t  broker[" << b << "].replication_done[" << broker_id_ << "]=" << rep_done;
				}
				// Log min calculation for ack_level=2
				if (ack_level == 2 && replication_factor > 0) {
					size_t min_rep_done = std::numeric_limits<size_t>::max();
					for (int i = 0; i < replication_factor; i++) {
						int b = GetReplicationSetBroker(broker_id_, replication_factor, num_brokers, i);
						size_t r = tinode->offsets[b].replication_done[broker_id_];
						if (min_rep_done > r) min_rep_done = r;
					}
					LOG(INFO) << "\t  min(replication_done)=" << min_rep_done << " (used for ack_level=2)";
				}
			}
			last_log_time = now;
		}

		if (!found_ack) {
			// [[CONFIG: Drain spin]] - Catch late CXL ordered update before sleep. EMBARCADERO_ACK_DRAIN_US (default 1000).
			const int drain_us = []() {
				const char* e = std::getenv("EMBARCADERO_ACK_DRAIN_US");
				return e ? std::atoi(e) : 1000;
			}();
			const int drain_eff = drain_us > 0 ? drain_us : 1000;
			auto drain_end = std::chrono::steady_clock::now() + std::chrono::microseconds(drain_eff);
			while (std::chrono::steady_clock::now() < drain_end) {
				cached_ack = GetOffsetToAck(topic, ack_level);
				if (cached_ack != (size_t)-1 && next_to_ack_offset <= cached_ack) {
					found_ack = true;
					consecutive_ack_stalls = 0;
					break;
				}
				CXL::cpu_pause();
			}
		}
		if (!found_ack) {
			// [[CONFIG: Adaptive sleep]] - Light/heavy sleep by stall count. EMBARCADERO_ACK_SLEEP_LIGHT_US, _SLEEP_HEAVY_MS, _HEAVY_SLEEP_STALL_THRESHOLD.
			const int sleep_light_us = []() {
				const char* e = std::getenv("EMBARCADERO_ACK_SLEEP_LIGHT_US");
				return e ? std::atoi(e) : 100;
			}();
			const int sleep_heavy_ms = []() {
				const char* e = std::getenv("EMBARCADERO_ACK_SLEEP_HEAVY_MS");
				return e ? std::atoi(e) : 1;
			}();
			const size_t heavy_threshold = []() {
				const char* e = std::getenv("EMBARCADERO_ACK_HEAVY_SLEEP_STALL_THRESHOLD");
				return e ? static_cast<size_t>(std::atoi(e)) : 200u;
			}();
			if (consecutive_ack_stalls < heavy_threshold) {
				std::this_thread::sleep_for(std::chrono::microseconds(sleep_light_us > 0 ? sleep_light_us : 100));
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(sleep_heavy_ms > 0 ? sleep_heavy_ms : 1));
			}
			continue;
		}

		// ACK is ready, fetch it and send
		// [[SENIOR_REVIEW_FIX: Use cached GetOffsetToAck() result from spin loop]]
		// We found ACK in spin loop, so cached_ack is valid and fresh
		// No need to call GetOffsetToAck() again - re-use cached value
		size_t ack = cached_ack;

		// [[DIAGNOSTIC: Log ACK decision]]
		static thread_local size_t ack_check_count = 0;
		if (++ack_check_count % 100 == 0 || (ack != (size_t)-1 && next_to_ack_offset <= ack)) {
			VLOG(2) << "AckThread B" << broker_id_ << ": ack=" << ack 
			        << " next_to_ack=" << next_to_ack_offset 
			        << " condition=" << (ack != (size_t)-1 && next_to_ack_offset <= ack ? "TRUE" : "FALSE")
			        << " (check_count=" << ack_check_count << ")";
		}

		if(ack != (size_t)-1 && next_to_ack_offset <= ack){
			LOG(INFO) << "AckThread: Broker " << broker_id_ << " sending ack for " << ack << " messages (prev=" << next_to_ack_offset-1 << ")";
			next_to_ack_offset = ack + 1;
			acked_size = 0;
			// Send offset acknowledgment
			// Add timeout to epoll_wait to prevent infinite blocking
			while (acked_size < sizeof(ack)) {
				int n = epoll_wait(ack_efd, events, 10, kAckEpollTimeoutMs);
				if (n == 0) {
					consecutive_timeouts++;
					if (consecutive_timeouts >= kMaxConsecutiveTimeouts) {
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

//----------------------------------------------------------------------------
// Non-Blocking Architecture Implementation
//----------------------------------------------------------------------------

/**
 * Drains batch header using non-blocking recv
 * @return true when header is complete, false if more data needed or error
 */
bool NetworkManager::DrainHeader(int fd, ConnectionState* state) {
    size_t remaining = sizeof(BatchHeader) - state->header_offset;

    ssize_t n = recv(fd, reinterpret_cast<uint8_t*>(&state->batch_header) + state->header_offset,
                     remaining, MSG_DONTWAIT);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Will retry on next EPOLLIN
            return false;
        }
        LOG(ERROR) << "DrainHeader: recv failed: " << strerror(errno) << " (fd=" << fd << ")";
        return false;
    }

    if (n == 0) {
        LOG(WARNING) << "DrainHeader: Connection closed, fd=" << fd;
        return false;
    }

    state->header_offset += n;

    bool complete = (state->header_offset == sizeof(BatchHeader));
    if (complete) {
        VLOG(3) << "DrainHeader: Complete. fd=" << fd
                << " batch_seq=" << state->batch_header.batch_seq
                << " total_size=" << state->batch_header.total_size;
    }

    return complete;
}

/**
 * Drains payload data into staging buffer using non-blocking recv
 * @return true when payload is complete, false if more data needed or error
 */
bool NetworkManager::DrainPayload(int fd, ConnectionState* state) {
    if (!state->staging_buf) {
        LOG(ERROR) << "DrainPayload: staging_buf is nullptr (fd=" << fd << ")";
        return false;
    }

    size_t remaining = state->batch_header.total_size - state->payload_offset;

    ssize_t n = recv(fd, reinterpret_cast<uint8_t*>(state->staging_buf) + state->payload_offset,
                     remaining, MSG_DONTWAIT);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Will retry on next EPOLLIN
            return false;
        }
        LOG(ERROR) << "DrainPayload: recv failed: " << strerror(errno) << " (fd=" << fd << ")";
        return false;
    }

    if (n == 0) {
        LOG(WARNING) << "DrainPayload: Connection closed, fd=" << fd
                     << " (received " << state->payload_offset << " of "
                     << state->batch_header.total_size << " bytes)";
        return false;
    }

    state->payload_offset += n;

    bool complete = (state->payload_offset == state->batch_header.total_size);
    if (complete) {
        VLOG(3) << "DrainPayload: Complete. fd=" << fd
                << " batch_seq=" << state->batch_header.batch_seq
                << " total_size=" << state->batch_header.total_size;
    }

    return complete;
}

/**
 * Check if staging pool has recovered capacity and resume paused sockets
 */
void NetworkManager::CheckStagingPoolRecovery(
        int epoll_fd,
        absl::flat_hash_map<int, std::unique_ptr<ConnectionState>>& connections) {

    if (!staging_pool_) return;

    // Only check if utilization has dropped below threshold
    size_t utilization = staging_pool_->GetUtilization();
    if (utilization >= 70) {
        return; // Still too high, wait
    }

    // Resume paused connections
    for (auto& [fd, state] : connections) {
        if (state->phase == WAIT_PAYLOAD && !state->staging_buf && !state->epoll_registered) {
            // Try to allocate staging buffer now
            void* buf = staging_pool_->Allocate(state->batch_header.total_size);
            if (buf) {
                state->staging_buf = buf;
                state->payload_offset = 0;

                // Re-register socket for EPOLLIN
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLET; // Edge-triggered
                ev.data.fd = fd;

                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
                    state->epoll_registered = true;
                    VLOG(2) << "CheckStagingPoolRecovery: Resumed fd=" << fd
                            << " (utilization=" << utilization << "%)";
                } else {
                    LOG(ERROR) << "CheckStagingPoolRecovery: epoll_ctl ADD failed: " << strerror(errno);
                    staging_pool_->Release(buf);
                    state->staging_buf = nullptr;
                }
            }
        }
    }
}

/**
 * Setup ACK connection for non-blocking publish handling
 */
void NetworkManager::SetupPublishConnection(ConnectionState* state, const NewPublishConnection& conn) {
    // Setup acknowledgment channel if needed
    if (conn.handshake.ack >= 1) {
        absl::MutexLock lock(&ack_mu_);
        auto it = ack_connections_.find(conn.handshake.client_id);

        if (it == ack_connections_.end()) {
            // Create new acknowledgment connection
            int ack_fd = -1;
            if (SetupAcknowledgmentSocket(ack_fd, conn.client_address, conn.handshake.port)) {
                int local_ack_efd = ack_efd_;
                ack_fd_ = ack_fd;
                ack_connections_[conn.handshake.client_id] = ack_fd;

                // [[CRITICAL: Validate topic before starting AckThread]]
                if (strlen(conn.handshake.topic) == 0) {
                    LOG(ERROR) << "SetupPublishConnection: Empty topic in handshake for broker " << broker_id_
                               << ", client_id=" << conn.handshake.client_id << ". NOT starting AckThread!";
                } else {
                    LOG(INFO) << "SetupPublishConnection: Starting AckThread for broker " << broker_id_
                              << ", topic='" << conn.handshake.topic << "', client_id=" << conn.handshake.client_id;
                    // Launch ACK thread
                    threads_.emplace_back(&NetworkManager::AckThread, this,
                                         conn.handshake.topic, conn.handshake.ack,
                                         ack_fd, local_ack_efd);
                }

                VLOG(2) << "SetupPublishConnection: Created ACK connection for client_id="
                       << conn.handshake.client_id;
            } else {
                LOG(ERROR) << "SetupPublishConnection: Failed to setup ACK socket for client_id="
                          << conn.handshake.client_id;
            }
        }
    }
}

/**
 * PublishReceiveThread: Epoll-based non-blocking socket draining
 * Drains sockets into staging buffers and enqueues for CXL allocation
 */
void NetworkManager::PublishReceiveThread() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG(FATAL) << "PublishReceiveThread: epoll_create1 failed: " << strerror(errno);
        return;
    }

    struct epoll_event events[64];
    absl::flat_hash_map<int, std::unique_ptr<ConnectionState>> connections;

    LOG(INFO) << "PublishReceiveThread started (epoll_fd=" << epoll_fd << ")";

    // Phase 3: Batch recovery check counter for optimization
    int recovery_check_counter = 0;
    constexpr int RECOVERY_CHECK_INTERVAL = 100; // Check every 100 iterations

    // Main event loop
    while (!stop_threads_) {
        // Check for new publish connections to register
        NewPublishConnection new_conn;
        while (publish_connection_queue_ && publish_connection_queue_->read(new_conn)) {
            // Create connection state
            auto state = std::make_unique<ConnectionState>();
            state->fd = new_conn.fd;
            state->phase = WAIT_HEADER;
            state->client_id = new_conn.handshake.client_id;
            state->topic = new_conn.handshake.topic;
            state->handshake = new_conn.handshake;
            state->epoll_registered = false;

            // Configure socket for non-blocking
            if (!ConfigureNonBlockingSocket(new_conn.fd)) {
                LOG(ERROR) << "PublishReceiveThread: Failed to configure non-blocking socket fd=" << new_conn.fd;
                close(new_conn.fd);
                continue;
            }

            // Register with epoll
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET; // Edge-triggered for efficiency
            ev.data.fd = new_conn.fd;

            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_conn.fd, &ev) < 0) {
                LOG(ERROR) << "PublishReceiveThread: epoll_ctl ADD failed: " << strerror(errno)
                          << " (fd=" << new_conn.fd << ")";
                close(new_conn.fd);
                continue;
            }

            state->epoll_registered = true;

            // Add to connections map (transfer ownership)
            int fd = new_conn.fd;
            connections[fd] = std::move(state);

            // Setup ACK connection if needed
            SetupPublishConnection(connections[fd].get(), new_conn);

            VLOG(1) << "PublishReceiveThread: Registered new connection fd=" << fd
                   << " client_id=" << new_conn.handshake.client_id
                   << " topic=" << new_conn.handshake.topic;
        }

        // Phase 3 Optimization: Reduced timeout for lower latency
        // 500µs provides good balance between responsiveness and CPU usage
        int n = epoll_wait(epoll_fd, events, 64, 0); // Non-blocking for maximum throughput

        if (n < 0) {
            if (errno == EINTR) {
                continue; // Interrupted, retry
            }
            LOG(ERROR) << "PublishReceiveThread: epoll_wait failed: " << strerror(errno);
            break;
        }

        // Process ready sockets
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            auto it = connections.find(fd);

            if (it == connections.end()) {
                LOG(WARNING) << "PublishReceiveThread: Unknown fd=" << fd << " in epoll event";
                continue;
            }

            ConnectionState* state = it->second.get();

            // Handle socket events
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                LOG(WARNING) << "PublishReceiveThread: Socket error/hangup on fd=" << fd;

                // Cleanup
                if (state->staging_buf) {
                    staging_pool_->Release(state->staging_buf);
                }
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                connections.erase(it);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                switch (state->phase) {
                    case WAIT_HEADER:
                        if (DrainHeader(fd, state)) {
                            // Header complete, transition to payload phase
                            state->phase = WAIT_PAYLOAD;

                            // Validate total_size
                            if (state->batch_header.total_size == 0 ||
                                state->batch_header.total_size > staging_pool_->GetBufferSize()) {
                                LOG(ERROR) << "PublishReceiveThread: Invalid batch total_size="
                                          << state->batch_header.total_size;

                                // Close connection
                                if (state->staging_buf) {
                                    staging_pool_->Release(state->staging_buf);
                                }
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                                close(fd);
                                connections.erase(it);
                                continue;
                            }

                            // Try to allocate staging buffer
                            state->staging_buf = staging_pool_->Allocate(state->batch_header.total_size);
                            if (!state->staging_buf) {
                                // Backpressure: pause socket
                                metric_staging_exhausted_.fetch_add(1, std::memory_order_relaxed);
                                VLOG(2) << "PublishReceiveThread: Staging pool exhausted, pausing fd=" << fd;
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                                state->epoll_registered = false;
                                // Will retry in CheckStagingPoolRecovery
                            }
                        }
                        break;

                    case WAIT_PAYLOAD:
                        if (state->staging_buf && DrainPayload(fd, state)) {
                            // Payload complete, enqueue for CXL allocation
                            metric_batches_drained_.fetch_add(1, std::memory_order_relaxed);

                            PendingBatch batch;
                            batch.conn_state = state;
                            batch.staging_buf = state->staging_buf;
                            batch.batch_header = state->batch_header;
                            batch.handshake = state->handshake;

                            if (!cxl_allocation_queue_->write(batch)) {
                                LOG(ERROR) << "PublishReceiveThread: Failed to enqueue batch "
                                          << "(queue full, fd=" << fd << ")";
                                staging_pool_->Release(state->staging_buf);
                            } else {
                                VLOG(3) << "PublishReceiveThread: Enqueued batch for CXL allocation "
                                       << "(fd=" << fd << ", batch_seq=" << state->batch_header.batch_seq << ")";
                            }

                            // Reset state for next batch
                            state->staging_buf = nullptr;
                            state->payload_offset = 0;
                            state->header_offset = 0;
                            state->phase = WAIT_HEADER;
                            state->cxl_allocation_attempts = 0;
                        }
                        break;

                    default:
                        LOG(ERROR) << "PublishReceiveThread: Invalid phase=" << state->phase
                                  << " for fd=" << fd;
                        break;
                }
            }
        }

        // Phase 3: Batched staging pool recovery (every N iterations)
        // Reduces overhead when pool is healthy
        if (++recovery_check_counter >= RECOVERY_CHECK_INTERVAL) {
            CheckStagingPoolRecovery(epoll_fd, connections);
            recovery_check_counter = 0;
        }
    }

    // Cleanup
    for (auto& [fd, state] : connections) {
        if (state->staging_buf) {
            staging_pool_->Release(state->staging_buf);
        }
        close(fd);
    }
    close(epoll_fd);

    LOG(INFO) << "PublishReceiveThread exiting";
}

/**
 * CXLAllocationWorker: Async CXL buffer allocation and copy
 * Dequeues PendingBatch entries, allocates CXL buffers, copies data, marks complete
 */
void NetworkManager::CXLAllocationWorker() {
    LOG(INFO) << "CXLAllocationWorker started";

    PendingBatch batch;
    static std::atomic<size_t> batches_processed{0};

    while (!stop_threads_) {
        // Try to dequeue a pending batch
        if (!cxl_allocation_queue_->read(batch)) {
            // Queue empty, sleep briefly
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // Allocate CXL buffer
        void* cxl_buf = nullptr;
        void* segment_header = nullptr;
        size_t logical_offset;
        SequencerType seq_type;
        BatchHeader* batch_header_location = nullptr;

        std::function<void(void*, size_t)> callback = cxl_manager_->GetCXLBuffer(
            batch.batch_header,
            batch.handshake.topic,
            cxl_buf,
            segment_header,
            logical_offset,
            seq_type,
            batch_header_location);

        if (!cxl_buf) {
            // CXL ring full, retry with exponential backoff
            batch.conn_state->cxl_allocation_attempts++;
            metric_cxl_retries_.fetch_add(1, std::memory_order_relaxed);
            metric_ring_full_.fetch_add(1, std::memory_order_relaxed);

            size_t ring_full_total = metric_ring_full_.load(std::memory_order_relaxed);
            if (ring_full_total <= 10 || ring_full_total % 1000 == 0) {
                LOG(WARNING) << "CXLAllocationWorker: Ring full, retry attempt "
                             << batch.conn_state->cxl_allocation_attempts
                             << " (batch_seq=" << batch.batch_header.batch_seq
                             << ", total_ring_full=" << ring_full_total << ")";
            }

            if (batch.conn_state->cxl_allocation_attempts > 10) {
                metric_batches_dropped_.fetch_add(1, std::memory_order_relaxed);
                size_t dropped = metric_batches_dropped_.load(std::memory_order_relaxed);
                LOG(ERROR) << "CXLAllocationWorker: Dropping batch after 10 retries "
                          << "(batch_seq=" << batch.batch_header.batch_seq
                          << ", total_dropped=" << dropped << ")";
                staging_pool_->Release(batch.staging_buf);
                continue;
            }

            // Exponential backoff: 100µs, 200µs, 400µs, 800µs, 1600µs
            int backoff_exp = std::min(batch.conn_state->cxl_allocation_attempts, 4);
            std::this_thread::sleep_for(std::chrono::microseconds(100 << backoff_exp));

            // Re-enqueue for retry
            if (!cxl_allocation_queue_->write(batch)) {
                metric_batches_dropped_.fetch_add(1, std::memory_order_relaxed);
                size_t dropped = metric_batches_dropped_.load(std::memory_order_relaxed);
                LOG(ERROR) << "CXLAllocationWorker: Failed to re-enqueue batch for retry "
                          << "(total_dropped=" << dropped << ")";
                staging_pool_->Release(batch.staging_buf);
            }
            continue;
        }

        // Phase 3 Optimization: Pipelined copy and flush
        // Copy in chunks and flush immediately for better cache utilization
        constexpr size_t CHUNK_SIZE = 256 * 1024; // 256KB chunks
        size_t total_size = batch.batch_header.total_size;
        uint8_t* dest = reinterpret_cast<uint8_t*>(cxl_buf);
        uint8_t* src = reinterpret_cast<uint8_t*>(batch.staging_buf);

        for (size_t offset = 0; offset < total_size; offset += CHUNK_SIZE) {
            size_t chunk_size = std::min(CHUNK_SIZE, total_size - offset);

            // Copy chunk
            memcpy(dest + offset, src + offset, chunk_size);

            // Immediately flush this chunk's cache lines (64B granularity)
            for (size_t i = 0; i < chunk_size; i += 64) {
                CXL::flush_cacheline(dest + offset + i);
            }
        }
        CXL::store_fence();

        // Phase 3: Track successful copy
        metric_batches_copied_.fetch_add(1, std::memory_order_relaxed);

        // Mark batch complete (critical for sequencer visibility)
        if (batch_header_location != nullptr) {
            __atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE);
            CXL::flush_cacheline(batch_header_location);

            // Flush second cache line too (BatchHeader is 128 bytes / 2 cache lines)
            const void* batch_header_next_line = reinterpret_cast<const void*>(
                reinterpret_cast<const uint8_t*>(batch_header_location) + 64);
            CXL::flush_cacheline(batch_header_next_line);

            CXL::store_fence();

            size_t processed = batches_processed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (processed <= 10 || processed % 5000 == 0) {
                VLOG(1) << "CXLAllocationWorker: batch_complete=1 batch_seq="
                       << batch.batch_header.batch_seq
                       << " num_msg=" << batch.batch_header.num_msg
                       << " total_size=" << batch.batch_header.total_size
                       << " (total_processed=" << processed << ")";
            }
        } else {
            LOG(ERROR) << "CXLAllocationWorker: batch_header_location is nullptr "
                      << "(batch_seq=" << batch.batch_header.batch_seq << ")";
        }

        // Release staging buffer
        staging_pool_->Release(batch.staging_buf);

        // Reset retry counter
        batch.conn_state->cxl_allocation_attempts = 0;

        // Invoke callback if provided (for KAFKA mode)
        if (callback) {
            // TODO: Implement message header extraction for callback
            // For now, just invoke with nullptr
            callback(nullptr, logical_offset);
        }
    }

    LOG(INFO) << "CXLAllocationWorker exiting";
}

} // namespace Embarcadero
