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
#include "../disk_manager/disk_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include "../embarlet/topic_manager.h"

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
		LOG(ERROR) << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno);
		return false;
	}

	// Enable TCP_NODELAY (disable Nagle's algorithm)
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0) {
		LOG(ERROR) << "setsockopt(TCP_NODELAY) failed: " << strerror(errno);
		return false;
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

		// Create main listener thread
		threads_.emplace_back(&NetworkManager::MainThread, this);

		// Create request handler threads
		for (int i = 0; i < num_reqReceive_threads; i++) {
			threads_.emplace_back(&NetworkManager::ReqReceiveThread, this);
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
	while (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
		LOG(ERROR) << "Error binding socket to port " << (PORT + broker_id_)
			<< " for broker " << broker_id_ << ": " << strerror(errno);
		sleep(5);  // Retry after delay
	}

	// Start listening
	if (listen(server_socket, SOMAXCONN) == -1) {
		LOG(ERROR) << "Error starting listener: " << strerror(errno);
		close(server_socket);
		return;
	}

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
		int n = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);
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

		// Perform handshake to determine request type
		EmbarcaderoReq handshake;
		size_t to_read = sizeof(handshake);

		// Read handshake data completely
		while (to_read > 0) {
			int ret = recv(req.client_socket, &handshake, to_read, 0);
			if (ret < 0) {
				LOG(ERROR) << "Error receiving handshake: " << strerror(errno);
				close(req.client_socket);
				return;
			}
			to_read -= ret;
		}

		// Process based on request type
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
			if (!SetupAcknowledgmentSocket(ack_fd, client_address, handshake.port)) {
				close(client_socket);
				return;
			}

			ack_fd_ = ack_fd;
			ack_connections_[handshake.client_id] = ack_fd;

			threads_.emplace_back(&NetworkManager::AckThread, this, handshake.topic, handshake.ack, ack_fd);
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

		std::function<void(void*, size_t)> non_emb_seq_callback =
			cxl_manager_->GetCXLBuffer(batch_header, handshake.topic, buf,
					segment_header, logical_offset, seq_type);

		if (!buf) {
			LOG(ERROR) << "Failed to get CXL buffer";
			break;
		}

		// Receive message data
		MessageHeader* header = nullptr;
		size_t read = 0;
		size_t header_size = sizeof(MessageHeader);
		size_t bytes_to_next_header = 0;

		while (running && !stop_threads_) {
			bytes_read = recv(client_socket, (uint8_t*)buf + read, to_read, 0);
			if (bytes_read < 0) {
				LOG(ERROR) << "Error receiving message data: " << strerror(errno);
				running = false;
				return;
			}

			// Process complete messages as they arrive
			while (bytes_to_next_header + header_size <= static_cast<size_t>(bytes_read)) {
				header = (MessageHeader*)((uint8_t*)buf + read + bytes_to_next_header);
				header->complete = 1;

				bytes_read -= bytes_to_next_header;
				read += bytes_to_next_header;
				to_read -= bytes_to_next_header;
				bytes_to_next_header = header->paddedSize;

				if (seq_type == KAFKA) {
					header->logical_offset = logical_offset;
					if (segment_header == nullptr) {
						LOG(ERROR) << "segment_header is null!";
					}
					header->segment_header = segment_header;
					header->next_msg_diff = header->paddedSize;

					// Flush cache lines
#ifdef __INTEL__
					_mm_clflushopt(header);
#elif defined(__AMD__)
					_mm_clwb(header);
#else
					LOG(ERROR) << "Neither Intel nor AMD processor detected";
#endif
					logical_offset++;
				}
			}

			read += bytes_read;
			to_read -= bytes_read;
			bytes_to_next_header -= bytes_read;

			if (to_read == 0) {
				break;
			}
		}

		// Finalize batch processing
		if (non_emb_seq_callback) {
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

	// Initialize or update subscriber state
	{
		absl::MutexLock lock(&sub_mu_);
		if (!sub_state_.contains(handshake.client_id)) {
			auto state = std::make_unique<SubscriberState>();
			state->last_offset = handshake.num_msg;
			state->last_addr = handshake.last_addr;
			state->initialized = true;
			sub_state_[handshake.client_id] = std::move(state);
		}
	}

	// Process subscription
	SubscribeNetworkThread(client_socket, epoll_fd, handshake.topic, handshake.client_id);

	// Cleanup
	close(client_socket);
	close(epoll_fd);
}

void NetworkManager::SubscribeNetworkThread(
		int sock,
		int efd,
		const char* topic,
		int client_id) {

	size_t zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
	int order = topic_manager_->GetTopicOrder(topic);

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
			absl::MutexLock lock(&sub_state_[client_id]->mu);

			if (order > 0){
				if (!topic_manager_->GetBatchToExport(
							topic,
							sub_state_[client_id]->last_offset,
							msg,
							messages_size)){
						std::this_thread::yield();
						continue;
					}
			}else{
				if (topic_manager_->GetMessageAddr(
									topic,
									sub_state_[client_id]->last_offset,
									sub_state_[client_id]->last_addr,
									msg,
									messages_size)) {
						// Split large messages into chunks for better flow control
						while (messages_size > zero_copy_send_limit) {
							struct LargeMsgRequest r;
							r.msg = msg;

							// Ensure we don't cut in the middle of a message
							int mod = zero_copy_send_limit % ((MessageHeader*)msg)->paddedSize;
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
	TInode* tinode = (TInode*)cxl_manager_->GetTInode(topic);
	static const int replication_factor = tinode->replication_factor;
	static const int order = tinode->order;
	static const SequencerType seq_type = tinode->seq_type;
	size_t min = std::numeric_limits<size_t>::max();
	//TODO(Jae) For now it is static. This should be changed for failure
	static const int num_brokers = get_num_brokers_callback_();

	if(replication_factor > 0){
		if(ack_level == 1){
			if(order == 0){
				return tinode->offsets[broker_id_].written;
			}else{
				return tinode->offsets[broker_id_].ordered;
			}
		}

		// Corfu ensures ordered set after replication so do not need to check replication factor
		if(seq_type == CORFU){
			return tinode->offsets[broker_id_].ordered;
		}

		size_t r[replication_factor];

		for (int i = 0; i < replication_factor; i++) {
			int b = (broker_id_ + num_brokers - i) % num_brokers;
			r[i] = tinode->offsets[b].replication_done[broker_id_];
			if (min > r[i]) {
				min = r[i];
			}
		}
		return min;
	}else{
		if(order == 0){
			return tinode->offsets[broker_id_].written;
		}else{
			return tinode->offsets[broker_id_].ordered;
		}
	}
}

void NetworkManager::AckThread(const char* topic, uint32_t ack_level, int ack_fd) {
	struct epoll_event events[10];
	char buf[1];

	// Send broker_id first so client can distinguish
	size_t acked_size = 0;
	while (acked_size < sizeof(broker_id_)) {
		int n = epoll_wait(ack_efd_, events, 10, -1);

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

	while (!stop_threads_) {
		size_t ack = GetOffsetToAck(topic, ack_level);
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 3) {
			LOG(INFO) << "Broker:" << broker_id_ << " Acknowledgments " << ack;
			TInode* tinode = (TInode*)cxl_manager_->GetTInode(topic);
			int replication_factor = tinode->replication_factor;
			for (int i = 0; i < replication_factor; i++) {
				int b = (broker_id_ + NUM_MAX_BROKERS - i) % NUM_MAX_BROKERS;
				LOG(INFO) <<"\t done:" <<  tinode->offsets[b].replication_done[broker_id_];
			}
			last_log_time = now;
		}

		if(ack != (size_t)-1 && next_to_ack_offset <= ack){
			next_to_ack_offset = ack + 1;
			acked_size = 0;
			// Send offset acknowledgment
			while (acked_size < sizeof(ack)) {
				int n = epoll_wait(ack_efd_, events, 10, -1);

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
				break;
			}
		}
	}// end of while loop
	close(ack_fd);
}
} // namespace Embarcadero
