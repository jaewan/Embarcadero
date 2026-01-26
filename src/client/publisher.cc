#include "publisher.h"
#include <random>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <thread>

Publisher::Publisher(char topic[TOPIC_NAME_SIZE], std::string head_addr, std::string port, 
		int num_threads_per_broker, size_t message_size, size_t queueSize, 
		int order, SequencerType seq_type)
	: head_addr_(head_addr),
	port_(port),
	client_id_(GenerateRandomNum()),
	num_threads_per_broker_(num_threads_per_broker),
	message_size_(message_size),
	queueSize_(queueSize / num_threads_per_broker),
	pubQue_(num_threads_per_broker_ * NUM_MAX_BROKERS, num_threads_per_broker_, client_id_, message_size, order),
	seq_type_(seq_type),
	sent_bytes_per_broker_(NUM_MAX_BROKERS),
	acked_messages_per_broker_(NUM_MAX_BROKERS),
	start_time_(std::chrono::steady_clock::now()){  // Initialize start_time_ immediately

		// Copy topic name
		memcpy(topic_, topic, TOPIC_NAME_SIZE);

		// Create gRPC stub for head broker
		std::string addr = head_addr + ":" + port;
		stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));

		// Initialize first broker
		nodes_[0] = head_addr + ":" + std::to_string(PORT);
		brokers_.emplace_back(0);

		VLOG(3) << "Publisher constructed with client_id: " << client_id_ 
			<< ", topic: " << topic 
			<< ", num_threads_per_broker: " << num_threads_per_broker_;
	}

Publisher::~Publisher() {
	VLOG(3) << "Publisher destructor called, cleaning up resources";

	// Signal all threads to terminate
	publish_finished_ = true;
	shutdown_ = true;
	context_.TryCancel();

	// Wait for all threads to complete (only if not already joined)
	for (auto& t : threads_) {
		if(t.joinable()){
			try {
				t.join();
			} catch (const std::exception& e) {
				LOG(ERROR) << "Exception in destructor joining thread: " << e.what();
			}
		}
	}

	if(cluster_probe_thread_.joinable()){
		cluster_probe_thread_.join();
	}

	if (ack_thread_.joinable()) {
		ack_thread_.join();
	}

	if (real_time_throughput_measure_thread_.joinable()) {
		real_time_throughput_measure_thread_.join();
	}

	if (kill_brokers_thread_.joinable()) {
		kill_brokers_thread_.join();
	}

	VLOG(3) << "Publisher destructor return";
}


void Publisher::Init(int ack_level) {
	ack_level_ = ack_level;
	

	// Generate unique port for acknowledgment server with retry logic
	// Ensure port is always in safe range 10000-65535 (avoid privileged ports < 1024)
	// Use modulo to ensure it fits in valid port range
	ack_port_ = (GenerateRandomNum() % (65535 - 10000 + 1)) + 10000;

	// Start acknowledgment thread if needed
	if (ack_level >= 1) {
		ack_thread_ = std::thread([this]() {
				this->EpollAckThread();
				});

		// Wait for acknowledgment thread to initialize
		while (thread_count_.load() != 1) {
			std::this_thread::yield();
		}
		thread_count_.store(0);
	}

	// Start cluster status monitoring thread
	cluster_probe_thread_ = std::thread([this]() {
			this->SubscribeToClusterStatus();
			});

	// Wait for connection to be established with timeout and logging
	auto connection_start = std::chrono::steady_clock::now();
	auto last_log_time = connection_start;
	constexpr auto CONNECTION_TIMEOUT = std::chrono::seconds(60);
	constexpr auto LOG_INTERVAL = std::chrono::seconds(5);
	
	while (!connected_) {
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - connection_start);
		
		// Check for timeout
		if (elapsed >= CONNECTION_TIMEOUT) {
			LOG(ERROR) << "Publisher::Init() timed out waiting for cluster connection after " 
			           << elapsed.count() << " seconds. This indicates gRPC SubscribeToCluster is failing.";
			LOG(ERROR) << "Check broker gRPC service availability and network connectivity.";
			break; // Exit to avoid infinite hang
		}
		
		// Log progress every 5 seconds
		if (now - last_log_time >= LOG_INTERVAL) {
			LOG(WARNING) << "Publisher::Init() waiting for cluster connection... (" 
			            << elapsed.count() << "s elapsed)";
			last_log_time = now;
		}
		
		Embarcadero::CXL::cpu_pause();
	}
	
	if (!connected_) {
		LOG(ERROR) << "Publisher::Init() failed - cluster connection was not established. "
		          << "Publisher will not be able to send messages.";
	}

	// Initialize Corfu sequencer if needed
	if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
		corfu_client_ = std::make_unique<CorfuSequencerClient>(
				CORFU_SEQUENCER_ADDR + std::to_string(CORFU_SEQ_PORT));
	}

	// Wait for all publisher threads to initialize
	while (thread_count_.load() != num_threads_.load()) {
		std::this_thread::yield();
	}
}

void Publisher::WarmupBuffers() {
	LOG(INFO) << "Pre-touching buffers to eliminate page fault variance...";
	// Delegate to the Buffer class which has access to private members
	pubQue_.WarmupBuffers();
}

void Publisher::Publish(char* message, size_t len) {
	// Calculate padding for 64-byte alignment
	const static size_t header_size = sizeof(Embarcadero::MessageHeader);
	size_t padded = len % 64;
	if (padded) {
		padded = 64 - padded;
	}

	// Total size includes message, padding and header
	size_t padded_total = len + padded + header_size;

#ifdef BATCH_OPTIMIZATION
	// Write to buffer using batch optimization
	if (!pubQue_.Write(client_order_, message, len, padded_total)) {
		LOG(ERROR) << "Failed to write message to queue (client_order=" << client_order_ << ")";
	}
#else
	// Non-batch write mode
	const static size_t batch_size = BATCH_SIZE;
	static size_t i = 0;
	static size_t j = 0;

	// Calculate how many messages fit in a batch
	size_t n = batch_size / (padded_total);
	if (n == 0) {
		n = 1;
	}

	// Write to the current buffer
	if (!pubQue_.Write(i, client_order_, message, len, padded_total)) {
		LOG(ERROR) << "Failed to write message to queue (client_order=" << client_order_ << ")";
	}

	// Move to next buffer after n messages
	j++;
	if (j == n) {
		i = (i + 1) % num_threads_.load();
		j = 0;
	}
#endif

	// Increment client order for next message
	client_order_++;
}

void Publisher::Poll(size_t n) {
	// Signal that publishing is finished
	publish_finished_ = true;
	// Ensure the final partial batch is sealed before shutting down reads
	WriteFinishedOrPuased();
	pubQue_.ReturnReads();

	// Wait for all messages to be queued
	// Use periodic spin-then-yield pattern for efficient polling
	// Spin for 1ms blocks, then yield once to reduce CPU waste while maintaining low latency
	constexpr auto SPIN_DURATION = std::chrono::milliseconds(1);
	while (client_order_ < n) {
		auto spin_start = std::chrono::steady_clock::now();
		// Spin block: high-frequency polling for 1ms
		while (std::chrono::steady_clock::now() - spin_start < SPIN_DURATION && client_order_ < n) {
			Embarcadero::CXL::cpu_pause();
		}
		// Yield once after spin to reduce CPU waste
		if (client_order_ < n) {
			std::this_thread::yield();
		}
	}

	// All messages queued, waiting for transmission to complete

	// CRITICAL FIX: Use atomic flag to prevent double-join race conditions
	if (!threads_joined_.exchange(true)) {
		// Only join threads once
		for (size_t i = 0; i < threads_.size(); ++i) {
			if (threads_[i].joinable()) {
				try {
				// Joining publisher thread
				threads_[i].join();
				// Successfully joined publisher thread
				} catch (const std::exception& e) {
					LOG(ERROR) << "Exception joining publisher thread " << i << ": " << e.what();
				}
			}
			// Publisher thread not joinable (already joined or detached)
		}
		// All publisher threads completed transmission
	}
	// Publisher threads already joined, skipping

	// If acknowledgments are enabled, wait for all acks
	if (ack_level_ >= 1) {
		auto last_log_time = std::chrono::steady_clock::now();
		constexpr auto SPIN_DURATION = std::chrono::milliseconds(1);
		while (ack_received_ < client_order_) {
			auto now = std::chrono::steady_clock::now();
			if(kill_brokers_){
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count() >= 100) {
					break;
				}
			}
			// Only log every 3 seconds to avoid spam
			if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 3) {
				LOG(INFO) << "Waiting for acknowledgments, received " << ack_received_ << " out of " << client_order_;
				last_log_time = now;
			}
			
			// Spin block: high-frequency polling for 1ms
			auto spin_start = std::chrono::steady_clock::now();
			while (std::chrono::steady_clock::now() - spin_start < SPIN_DURATION && ack_received_ < client_order_) {
				Embarcadero::CXL::cpu_pause();
			}
			// Yield once after spin to reduce CPU waste
			if (ack_received_ < client_order_) {
				std::this_thread::yield();
			}
		}
	}

	// IMPROVED: Graceful disconnect - keep gRPC context alive for subscriber
	// Only set publish_finished flag, don't shutdown entire system
	// The gRPC context remains active to support subscriber cluster management
	// Publisher data connections are already closed by joined threads
	
	LOG(INFO) << "Publisher finished sending " << client_order_ << " messages, keeping cluster context alive for subscriber";
	
	// NOTE: We do NOT set shutdown_=true or cancel context here
	// This allows the subscriber to continue using the cluster management infrastructure
	// The context will be cleaned up when the Publisher object is destroyed
}

void Publisher::DEBUG_check_send_finish() {
	WriteFinishedOrPuased();
	publish_finished_ = true;
	pubQue_.ReturnReads();

	// CRITICAL FIX: Don't join threads here as Poll() will handle thread cleanup
	// This prevents double-join issues and race conditions
	// DEBUG_check_send_finish: Signaled publishing completion, threads will be joined in Poll()
}

void Publisher::FailBrokers(size_t total_message_size, size_t message_size,
		double failure_percentage, 
		std::function<bool()> killbrokers) {
	kill_brokers_ = true;

	measure_real_time_throughput_ = true;
	size_t num_brokers = nodes_.size();

	// Initialize counters for sent bytes
	for (size_t i = 0; i < num_brokers; i++) {
		sent_bytes_per_broker_[i].store(0);
		acked_messages_per_broker_[i] = 0;
	}

	// Start thread to monitor progress and kill brokers at specified percentage
	kill_brokers_thread_ = std::thread([=, this]() {
		size_t bytes_to_kill_brokers = total_message_size * failure_percentage;

		while (!shutdown_ && total_sent_bytes_ < bytes_to_kill_brokers) {
			std::this_thread::yield();
		}

		if (!shutdown_) {
			killbrokers();
		}
	});

	// Start thread to measure real-time throughput
	real_time_throughput_measure_thread_ = std::thread([=, this]() {
		std::vector<size_t> prev_throughputs(num_brokers, 0);

		// Open file for writing throughput data
		//TODO(Jae) Rewrite this to be relative path
		std::string home_dir = getenv("HOME") ? getenv("HOME") : "."; // Get home dir or use current
		std::string filename = home_dir + "/Embarcadero/data/failure/real_time_acked_throughput.csv";
		std::ofstream throughputFile(filename);
		if (!throughputFile.is_open()) {
		LOG(ERROR) << "Failed to open file for writing throughput data: " << filename;
		return;
		}

		// Write CSV header
		throughputFile << "Timestamp(ms)"; // Add timestamp column
		for (size_t i = 0; i < num_brokers; i++) {
		throughputFile << ",Broker_" << i << "_GBps";
		}
		throughputFile << ",Total_GBps\n";

		// Measuring loop
		const int measurement_interval_ms = 5;
		const double time_factor_gbps = (1000.0 / measurement_interval_ms) / (1024.0 * 1024.0 * 1024.0); // Factor to get GB/s

		while (!shutdown_) {
			std::this_thread::sleep_for(std::chrono::milliseconds(measurement_interval_ms));
			size_t sum = 0;
			auto now = std::chrono::steady_clock::now();
			auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
			throughputFile << timestamp_ms; // Write timestamp

			for (size_t i = 0; i < num_brokers; i++) {
				size_t bytes = acked_messages_per_broker_[i] * message_size;
				size_t real_time_throughput = (bytes - prev_throughputs[i]);

				// Convert to GB/s for CSV
				double gbps = real_time_throughput * time_factor_gbps;
				throughputFile << "," << gbps;

				sum += real_time_throughput;
				prev_throughputs[i] = bytes;
			}

			// Convert total to GB/s
			double total_gbps = (sum * time_factor_gbps); 
			throughputFile << "," << total_gbps << "\n";
		}

		throughputFile.flush();
		throughputFile.close();
	});
}

void Publisher::WriteFinishedOrPuased() {
	pubQue_.SealAll();
}

void Publisher::EpollAckThread() {
	if (ack_level_ < 1) {
		return;
	}

	// Create server socket
	int server_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (server_sock < 0) {
		LOG(ERROR) << "Socket creation failed: " << strerror(errno);
		return;
	}

	// Configure socket options
	int flag = 1;
	if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
		LOG(ERROR) << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno);
		close(server_sock);
		return;
	}

	// Disable Nagle's algorithm for better latency
	if (setsockopt(server_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
		LOG(ERROR) << "setsockopt(TCP_NODELAY) failed: " << strerror(errno);
	}

	// Enable TCP_QUICKACK for low-latency ACKs
	if (setsockopt(server_sock, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag)) < 0) {
		LOG(WARNING) << "setsockopt(TCP_QUICKACK) failed: " << strerror(errno);
		// Non-fatal, continue
	}

	// Increase socket buffers for high-throughput (32MB)
	const int buffer_size = 32 * 1024 * 1024;  // 32 MB
	if (setsockopt(server_sock, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_SNDBUF) failed: " << strerror(errno);
		// Non-fatal, continue
	}
	if (setsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_RCVBUF) failed: " << strerror(errno);
		// Non-fatal, continue
	}

	// Set up server address
	sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(ack_port_);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	// Bind the socket with retry logic for port conflicts
	int bind_attempts = 0;
	const int max_bind_attempts = 10;
	while (bind_attempts < max_bind_attempts) {
		if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == 0) {
			break; // Bind successful
		}
		
		if (errno == EADDRINUSE) {
			// Port in use, try a different port
			bind_attempts++;
			// Ensure port is always in safe range 10000-65535
			ack_port_ = (GenerateRandomNum() % (65535 - 10000 + 1)) + 10000;
			server_addr.sin_port = htons(ack_port_);
			LOG(WARNING) << "Port " << (ack_port_ - 1) << " in use, trying port " << ack_port_ 
			             << " (attempt " << bind_attempts << "/" << max_bind_attempts << ")";
		} else {
			// Other bind error
			LOG(ERROR) << "Bind failed: " << strerror(errno);
			close(server_sock);
			return;
		}
	}
	
	if (bind_attempts >= max_bind_attempts) {
		LOG(ERROR) << "Failed to bind after " << max_bind_attempts << " attempts";
		close(server_sock);
		return;
	}

	// Start listening
	if (listen(server_sock, SOMAXCONN) < 0) {
		LOG(ERROR) << "Listen failed: " << strerror(errno);
		close(server_sock);
		return;
	}

	// Create epoll instance
	int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		LOG(ERROR) << "Failed to create epoll file descriptor: " << strerror(errno);
		close(server_sock);
		return;
	}

	// Add server socket to epoll
	epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = server_sock;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &event) == -1) {
		LOG(ERROR) << "Failed to add server socket to epoll: " << strerror(errno);
		close(server_sock);
		close(epoll_fd);
		return;
	}

	// Variables for epoll event handling
	const int max_events =  NUM_MAX_BROKERS > 0 ? NUM_MAX_BROKERS * 2 : 64;
	std::vector<epoll_event> events(max_events);
	int EPOLL_TIMEOUT = 10;  // 1 millisecond timeout
	std::map<int, int> client_sockets; // Map: client_fd -> broker_id (value is broker_id)

	// Map to track the last received cumulative ACK per socket for calculating increments
	// Initializing with -1 assumes ACK IDs (logical_offset) start >= 0.
	// The first calculation becomes ack - (size_t)-1 which equals ack + 1.
	absl::flat_hash_map<int, size_t> prev_ack_per_sock;

	// Track state for reading initial broker ID
	enum class ConnState { WAITING_FOR_ID, READING_ACKS };
	std::map<int, ConnState> socket_state;
	std::map<int, std::pair<int, size_t>> partial_id_reads; // fd -> {partial_id, bytes_read}

thread_count_.fetch_add(1); // Signal that initialization is complete

// Main epoll loop
while (!shutdown_) {
	int num_events = epoll_wait(epoll_fd, events.data(), max_events, EPOLL_TIMEOUT);

	if (num_events < 0) {
		if (errno == EINTR) {
			continue; // Interrupted, just retry
		}
		LOG(ERROR) << "AckThread: epoll_wait failed: " << strerror(errno);
		break; // Exit loop on unrecoverable error
	}

	for (int i = 0; i < num_events; i++) {
		int current_fd = events[i].data.fd;
		if (current_fd == server_sock) {
			// Handle new connection
			sockaddr_in client_addr;
			socklen_t client_addr_len = sizeof(client_addr);
			int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);

			if (client_sock == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// This can happen with level-triggered accept if already handled? Should be rare.
					VLOG(2) << "AckThread: accept returned EAGAIN/EWOULDBLOCK";
				} else {
					LOG(ERROR) << "AckThread: Accept failed: " << strerror(errno);
				}
				continue;
			}

			// Set client socket to non-blocking mode
			int flags = fcntl(client_sock, F_GETFL, 0);
			if (flags == -1 || fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
				LOG(ERROR) << "Failed to set client socket to non-blocking: " << strerror(errno);
				close(client_sock);
				continue;
			}

			// Add client socket to epoll
			event.events = EPOLLIN | EPOLLET;  // Edge-triggered mode
			event.data.fd = client_sock;

			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &event) == -1) {
				LOG(ERROR) << "Failed to add client socket to epoll: " << strerror(errno);
				close(client_sock);
			} else {
				client_sockets[client_sock] = -1; // Temporarily store fd, broker_id is unknown (-1)
				socket_state[client_sock] = ConnState::WAITING_FOR_ID; // Expect Broker ID first
				partial_id_reads[client_sock] = {0, 0};
				prev_ack_per_sock[client_sock] = (size_t)-1;
			}
		} else {
			// Handle data from existing connection
			int client_sock = current_fd;
			ConnState current_state = socket_state[client_sock]; // if this fails, something's very wrong
			bool connection_error_or_closed = false;

			while (!connection_error_or_closed) {
				if (current_state == ConnState::WAITING_FOR_ID){
					// --- Try to Read Broker ID ---
					int broker_id_buffer;
					auto& partial_read = partial_id_reads[client_sock];
					size_t needed = sizeof(broker_id_buffer) - partial_read.second;
					ssize_t recv_ret = recv(client_sock,
							(char*)&partial_read.first + partial_read.second, // Read into partial buffer
							needed, 0);

					if (recv_ret == 0) { connection_error_or_closed = true; break; }
					if (recv_ret < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) break; // No more data now
						if (errno == EINTR) continue; // Retry read
						LOG(ERROR) << "AckThread: recv error reading broker ID on fd " << client_sock << ": " << strerror(errno);
						connection_error_or_closed = true; break;
					}

					partial_read.second += recv_ret; // Increment bytes read for ID

					if (partial_read.second == sizeof(broker_id_buffer)) {
						// Full ID received
						broker_id_buffer = partial_read.first; // Get the ID
						if (broker_id_buffer < 0 || broker_id_buffer >= (int)acked_messages_per_broker_.size()) {
							LOG(ERROR) << "AckThread: Received invalid broker_id " << broker_id_buffer << " on fd " << client_sock;
							connection_error_or_closed = true; break; // Invalid ID, close connection
						}
						VLOG(1) << "AckThread: Received Broker ID " << broker_id_buffer << " from fd=" << client_sock;
						client_sockets[client_sock] = broker_id_buffer; // Update map value
						socket_state[client_sock] = ConnState::READING_ACKS; // Transition state
						current_state = ConnState::READING_ACKS; // Update local state for this loop
																										 // Clear partial read state for this FD
						partial_id_reads.erase(client_sock);
						// Continue reading potential ACK data in the same loop iteration
					}
					// If ID still not complete, loop will try recv() again if more data indicated by epoll
				}else if(current_state == ConnState::READING_ACKS){
					size_t acked_num_msg_buffer; // Temporary buffer for one ACK
																			 // TODO: Add buffering for partial ACK reads if needed, similar to ID read.
																			 // For simplicity now, assume ACKs arrive fully or cause error/EAGAIN.
					ssize_t recv_ret = recv(client_sock, &acked_num_msg_buffer, sizeof(acked_num_msg_buffer), 0);
					if (recv_ret == 0) { connection_error_or_closed = true; break; }
					if (recv_ret < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) break; // No more data now
						if (errno == EINTR) continue; // Retry read
						LOG(ERROR) << "AckThread: recv error reading ACK bytes on fd " << client_sock << ": " << strerror(errno);
						connection_error_or_closed = true; break;
					}
					if (recv_ret != sizeof(acked_num_msg_buffer)) {
						// Partial ACK read - requires buffering logic like the ID part.
						// For now, log warning and potentially close.
						LOG(WARNING) << "AckThread: Received partial ACK (" << recv_ret << "/" << sizeof(acked_num_msg_buffer) << " bytes) on fd: " << client_sock << ". Discarding.";
						// Decide if this constitutes an error state.
						// connection_error_or_closed = true; break;
						continue; // Or try reading more? Simple for now: discard and wait for next read event.
					}

					// --- Process Full ACK Bytes ---
					size_t acked_msg = acked_num_msg_buffer;
					int broker_id = client_sockets[client_sock]; // Get broker ID

					// Check if broker_id is valid (should be if state is READING_ACKS)
					if (broker_id < 0) {
						LOG(ERROR) << "AckThread: Invalid broker_id (-1) for fd " << client_sock << " in READING_ACKS state.";
						connection_error_or_closed = true; break;
					}

					size_t prev_acked = prev_ack_per_sock[client_sock]; // Assumes key exists

					if (acked_msg >= prev_acked || prev_acked == (size_t)-1) { // Check for valid cumulative value
						size_t new_acked_msgs = acked_msg - prev_acked;
						if (new_acked_msgs > 0) {
							acked_messages_per_broker_[broker_id]+=new_acked_msgs;
							ack_received_ += new_acked_msgs;
							prev_ack_per_sock[client_sock] = acked_msg; // Update last value for this socket
							VLOG(4) << "AckThread: fd=" << client_sock << " (Broker " << broker_id << ") ACK messages: " 
								<< acked_msg << " (+" << new_acked_msgs << ")";
						} else {
							// Duplicate cumulative value, ignore.
							VLOG(5) << "AckThread: fd=" << client_sock << " (Broker " << broker_id << 
								") Duplicate ACK messages received: " << acked_msg;
						}
					} else {
						LOG(WARNING) << "AckThread: Received non-monotonic ACK bytes on fd " << client_sock
							<< " (Broker " << broker_id << "). Received: " << acked_msg << ", Previous: " << prev_acked;
					}
					// Continue loop to read potentially more data from this socket event
				}else{
					LOG(ERROR) << "AckThread: Invalid state for fd " << client_sock;
					connection_error_or_closed = true; 
					break;
				}
			} // End outer `while (!connection_error_or_closed)` loop for EPOLLET
			if(connection_error_or_closed){
				VLOG(3) << "AckThread: Cleaning up connection fd=" << client_sock;
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr); // Ignore error
				close(client_sock);
				client_sockets.erase(client_sock);
				prev_ack_per_sock.erase(client_sock);
				socket_state.erase(client_sock);
				partial_id_reads.erase(client_sock); // Clean up partial ID state too
			}
		}//end else (handle data from existing connection)
	}// End for loop through epoll events
}// End while(!shutdown_)

// Clean up client sockets
for (auto const& [sock_fd, broker_id] : client_sockets) {
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sock_fd, nullptr);
	close(sock_fd);
}

// Clean up epoll and server socket
close(epoll_fd);
close(server_sock);
}

void Publisher::PublishThread(int broker_id, int pubQuesIdx) {
	static std::atomic<size_t> total_batches_sent{0};
	static std::atomic<size_t> total_batches_attempted{0};
	static std::atomic<size_t> total_batches_failed{0};

	int sock = -1;
	int efd = -1;

	// Lambda function to establish connection to a broker
	auto connect_to_server = [&](size_t brokerId) -> bool {
		// Close existing connections if any
		if (sock >= 0) close(sock);
		if (efd >= 0) close(efd);

		// Get broker address
		std::string addr;
		size_t num_brokers;
		{
			absl::MutexLock lock(&mutex_);
			auto it = nodes_.find(brokerId);
			if (it == nodes_.end()) {
				LOG(ERROR) << "Broker ID " << brokerId << " not found in nodes map";
				return false;
			}

			try {
				auto [_addr, _port] = ParseAddressPort(it->second);
				addr = _addr;
			} catch (const std::exception& e) {
				LOG(ERROR) << "Failed to parse address for broker " << brokerId 
				           << ": " << it->second << " - " << e.what();
				return false;
			}
			num_brokers = nodes_.size();
		}

		// Create socket
		LOG(INFO) << "PublishThread: Connecting to broker " << brokerId << " at " << addr << ":" << (PORT + brokerId);
		sock = GetNonblockingSock(const_cast<char*>(addr.c_str()), PORT + brokerId);
		if (sock < 0) {
			LOG(ERROR) << "PublishThread: Failed to create socket to broker " << brokerId << " at " << addr << ":" << (PORT + brokerId);
			return false;
		}

		// Create epoll instance
		efd = epoll_create1(0);
		if (efd < 0) {
			LOG(ERROR) << "epoll_create1 failed: " << strerror(errno);
			close(sock);
			sock = -1;
			return false;
		}

		// Register socket with epoll
		struct epoll_event event;
		event.data.fd = sock;
		event.events = EPOLLOUT;
		if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &event) != 0) {
			LOG(ERROR) << "epoll_ctl failed: " << strerror(errno);
			close(sock);
			close(efd);
			sock = -1;
			efd = -1;
			return false;
		}

		// Prepare handshake message
		Embarcadero::EmbarcaderoReq shake;
		shake.client_req = Embarcadero::Publish;
		shake.client_id = client_id_;
		memset(shake.topic, 0, sizeof(shake.topic));
		memcpy(shake.topic, topic_, std::min<size_t>(TOPIC_NAME_SIZE - 1, sizeof(shake.topic) - 1));
		shake.ack = ack_level_;
		shake.port = ack_port_;
		shake.num_msg = num_brokers;  // Using num_msg field to indicate number of brokers

		// Send handshake with epoll for non-blocking
		struct epoll_event events[10];
		bool running = true;
		size_t sent_bytes = 0;

		while (!shutdown_ && running) {
			// Use timeout instead of -1 to prevent indefinite hanging
			int n = epoll_wait(efd, events, 10, 1000); // 1 second timeout
			if (n == 0) {
				// Timeout - check if we should continue
				if (shutdown_ || publish_finished_) {
				// PublishThread: Handshake interrupted by shutdown
				break;
				}
				continue;
			}
			if (n < 0) {
				if (errno == EINTR) continue;
				LOG(ERROR) << "PublishThread: epoll_wait failed during handshake: " << strerror(errno);
				break;
			}
			for (int i = 0; i < n; i++) {
				if (events[i].events & EPOLLOUT) {
					ssize_t bytesSent = send(sock, 
							reinterpret_cast<int8_t*>(&shake) + sent_bytes, 
							sizeof(shake) - sent_bytes, 
							0);

					if (bytesSent <= 0) {
						if (errno != EAGAIN && errno != EWOULDBLOCK) {
							LOG(ERROR) << "Handshake send failed: " << strerror(errno);
							running = false;
							close(sock);
							close(efd);
							sock = -1;
							efd = -1;
							return false;
						}
						// EAGAIN/EWOULDBLOCK are expected in non-blocking mode
					} else {
						sent_bytes += bytesSent;
						if (sent_bytes == sizeof(shake)) {
							LOG(INFO) << "PublishThread: Handshake sent successfully to broker " << brokerId 
							         << " (client_id=" << client_id_ << ", topic=" << topic_ << ")";
							running = false;
							break;
						}
					}
				}
			}
		}

		if (sent_bytes != sizeof(shake)) {
			LOG(ERROR) << "PublishThread: Handshake incomplete - sent " << sent_bytes 
			          << " of " << sizeof(shake) << " bytes to broker " << brokerId;
			return false;
		}

		return true;
	};

	// Connect to initial broker
	LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Starting connection to broker " << broker_id;
	if (!connect_to_server(broker_id)) {
		LOG(ERROR) << "PublishThread[" << pubQuesIdx << "]: Failed to connect to broker " << broker_id;
		return;
	}
	LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Successfully connected to broker " << broker_id;

	// Signal thread is initialized
	thread_count_.fetch_add(1);
	LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Thread initialized, thread_count=" << thread_count_.load();

	// Track batch sequence for this thread
	size_t batch_seq = pubQuesIdx;
	
	// Track if we've sent at least one batch (to ensure connection is used)
	bool has_sent_batch = false;

	// Main publishing loop
	while (!shutdown_) {
		size_t len;
		int bytesSent = 0;

#ifdef BATCH_OPTIMIZATION
		// Read a batch from the queue
		Embarcadero::BatchHeader* batch_header = 
			static_cast<Embarcadero::BatchHeader*>(pubQue_.Read(pubQuesIdx));

		// Skip if no batch is available
		if (batch_header == nullptr || batch_header->total_size == 0) {
			if (publish_finished_ || shutdown_) {
				// CRITICAL: Don't exit immediately if we haven't sent any batches yet
				// This ensures the connection stays alive even if this thread got no batches
				// NetworkManager expects to receive at least one batch header per connection
				if (!has_sent_batch) {
					LOG(WARNING) << "PublishThread[" << pubQuesIdx << "]: No batches to send, but keeping connection alive. "
					            << "This thread may have been assigned to a buffer with no data.";
					// Wait a bit to see if batches arrive, then exit gracefully
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
				// PublishThread exiting
				LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Exiting - publish_finished=" 
				         << publish_finished_ << ", shutdown=" << shutdown_ 
				         << ", has_sent_batch=" << has_sent_batch;
				break;
		} else {
			// Log periodically when waiting for batches
			static thread_local size_t wait_count = 0;
			if (++wait_count % 100000 == 0) {
				LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Waiting for batches from buffer " 
				         << pubQuesIdx << " (wait_count=" << wait_count << ")";
			}
			Embarcadero::CXL::cpu_pause();
			continue;
		}
		}

		// Log when we successfully read a batch
		static thread_local size_t batch_count = 0;
		if (++batch_count % 100 == 0 || batch_count == 1) {
			LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Read batch " << batch_count 
			         << " from buffer " << pubQuesIdx 
			         << " (batch_seq=" << batch_header->batch_seq 
			         << ", num_msg=" << batch_header->num_msg 
			         << ", total_size=" << batch_header->total_size << ")";
		}
		total_batches_attempted.fetch_add(1, std::memory_order_relaxed);

		batch_header->client_id = client_id_;
		batch_header->broker_id = broker_id;

		// Get pointer to message data
		void* msg = reinterpret_cast<uint8_t*>(batch_header) + sizeof(Embarcadero::BatchHeader);
		len = batch_header->total_size;

		// Function to send batch header
		auto send_batch_header = [&]() -> void {
			// Handle sequencer-specific batch header processing
			if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
				batch_header->broker_id = broker_id;
				corfu_client_->GetTotalOrder(batch_header);

				// Update total order for each message in the batch
				Embarcadero::MessageHeader* header = static_cast<Embarcadero::MessageHeader*>(msg);
				size_t total_order = batch_header->total_order;

				for (size_t i = 0; i < batch_header->num_msg; i++) {
					header->total_order = total_order++;
					// Move to next message
					header = reinterpret_cast<Embarcadero::MessageHeader*>(
							reinterpret_cast<uint8_t*>(header) + header->paddedSize);
				}
			}

			// Send batch header with retry logic
			size_t total_sent = 0;
			const size_t header_size = sizeof(Embarcadero::BatchHeader);

			while (total_sent < header_size) {
				bytesSent = send(sock, 
						reinterpret_cast<uint8_t*>(batch_header) + total_sent, 
						header_size - total_sent, 
						0);

				if (bytesSent < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
						// Wait for socket to become writable
						struct epoll_event events[10];
						int n = epoll_wait(efd, events, 10, 1000);

						if (n == -1) {
							LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
							throw std::runtime_error("epoll_wait failed");
						}
					} else {
						// Fatal error
						LOG(ERROR) << "Failed to send batch header: " << strerror(errno);
						throw std::runtime_error("send failed");
					}
				} else {
					total_sent += bytesSent;
				}
			}
		};
#else
		// Non-batch mode
		void* msg = pubQue_.Read(pubQuesIdx, len);
		if (len == 0) {
			break;
		}

		// Create batch header
		Embarcadero::BatchHeader batch_header;
		batch_header.broker_id = broker_id;
		batch_header.client_id = client_id_;
		batch_header.total_size = len;
		batch_header.num_msg = len / static_cast<Embarcadero::MessageHeader*>(msg)->paddedSize;
		batch_header.batch_seq = batch_seq;

		// Function to send batch header
		auto send_batch_header = [&]() -> void {
			bytesSent = send(sock, reinterpret_cast<uint8_t*>(&batch_header), sizeof(batch_header), 0);

			// Handle partial sends
			while (bytesSent < static_cast<ssize_t>(sizeof(batch_header))) {
				if (bytesSent < 0) {
					LOG(ERROR) << "Batch send failed: " << strerror(errno);
					throw std::runtime_error("send failed");
				}

				bytesSent += send(sock, 
						reinterpret_cast<uint8_t*>(&batch_header) + bytesSent, 
						sizeof(batch_header) - bytesSent, 
						0);
			}
		};
#endif

		// Try to send batch header, handle failures
		try {
			send_batch_header();
			if (batch_count % 100 == 0 || batch_count == 1) {
				LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Sent batch header for batch " 
				         << batch_count << " to broker " << broker_id;
			}
		} catch (const std::exception& e) {
			total_batches_failed.fetch_add(1, std::memory_order_relaxed);
			LOG(ERROR) << "Exception sending batch header: " << e.what();
			std::string fail_msg = "Header Send Fail Broker " + std::to_string(broker_id) + " (" + e.what() + ")";
			RecordFailureEvent(fail_msg); // Record event

			// Handle broker failure by finding another broker
			int new_broker_id;
			{
				absl::MutexLock lock(&mutex_);

				// Remove the failed broker
				auto it = std::find(brokers_.begin(), brokers_.end(), broker_id);
				if (it != brokers_.end()) {
					brokers_.erase(it);
					nodes_.erase(broker_id);
				}

				// No brokers left
				if (brokers_.empty()) {
					LOG(ERROR) << "No brokers available, thread exiting";
					return;
				}

				// Select replacement broker
				new_broker_id = brokers_[(pubQuesIdx % num_threads_per_broker_) % brokers_.size()];
			}

			// Connect to new broker
			if (!connect_to_server(new_broker_id)) {
				RecordFailureEvent("Reconnect Fail Broker " + std::to_string(new_broker_id));
				LOG(ERROR) << "Failed to connect to replacement broker " << new_broker_id;
				return;
			}

			std::string reconn_msg = "Reconnect Success Broker " + std::to_string(new_broker_id) + " (from " + std::to_string(broker_id) + ")";
			RecordFailureEvent(reconn_msg);

			try {
				send_batch_header();
			} catch (const std::exception& e) {
				total_batches_failed.fetch_add(1, std::memory_order_relaxed);
				LOG(ERROR) << "Failed to send batch header to replacement broker: " << e.what();
				std::string fail_msg2 = "Header Send Fail (Post-Reconnect) Broker " + std::to_string(new_broker_id) + " (" + e.what() + ")";
				RecordFailureEvent(fail_msg2);
				return;
			}

			// Thread redirected from broker to new broker after failure

			broker_id = new_broker_id;
		}

		// Send message data
		size_t sent_bytes = 0;
		size_t zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;

		// CRITICAL: Ensure all batch data is sent before checking publish_finished_
		// This prevents premature thread exit while data is still in flight
		while (sent_bytes < len) {
			// Check for shutdown but don't exit mid-send - finish sending current batch
			if (shutdown_ && !publish_finished_) {
				LOG(WARNING) << "PublishThread[" << pubQuesIdx << "]: Shutdown requested but batch not fully sent ("
				           << sent_bytes << " of " << len << " bytes). Completing send...";
			}
			size_t remaining_bytes = len - sent_bytes;
			size_t to_send = std::min(remaining_bytes, zero_copy_send_limit);

		// PERF TUNED: Use MSG_ZEROCOPY for sends >= 64KB (Linux kernel optimal threshold)
        // Below 64KB: zero-copy overhead > benefit. Above 64KB: significant performance gain
        int send_flags = (to_send >= (64UL << 10)) ? MSG_ZEROCOPY : 0;

			bytesSent = send(sock, 
					static_cast<uint8_t*>(msg) + sent_bytes, 
					to_send, 
					send_flags);

			if (bytesSent > 0) {
				// Update statistics
				sent_bytes_per_broker_[broker_id].fetch_add(bytesSent, std::memory_order_relaxed);
				total_sent_bytes_.fetch_add(bytesSent, std::memory_order_relaxed);
				sent_bytes += bytesSent;

				// Reset backoff after successful send
				zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
			} else if (bytesSent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
				// Socket buffer full, wait for it to become writable
				struct epoll_event events[10];
				int n = epoll_wait(efd, events, 10, 1000);

				if (n == -1) {
					LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
					break;
				}

				// OPTIMIZATION: Less aggressive backoff to maintain higher throughput
				zero_copy_send_limit = std::max(zero_copy_send_limit * 3 / 4, 1UL << 16); // Reduce by 25%, min 64KB
			} else if (bytesSent < 0) {
				// Connection failure, switch to a different broker
				LOG(WARNING) << "Send failed to broker " << broker_id << ": " << strerror(errno);
				std::string fail_msg = "Data Send Fail Broker " + std::to_string(broker_id) + " errno=" + std::to_string(errno);
				RecordFailureEvent(fail_msg);

				int new_broker_id;
				{
					absl::MutexLock lock(&mutex_);

					// Remove the failed broker
					auto it = std::find(brokers_.begin(), brokers_.end(), broker_id);
					if (it != brokers_.end()) {
						brokers_.erase(it);
						nodes_.erase(broker_id);
					}

					// No brokers left
					if (brokers_.empty()) {
						LOG(ERROR) << "No brokers available, thread exiting";
						return;
					}

					// Select replacement broker
					new_broker_id = brokers_[(pubQuesIdx % num_threads_per_broker_) % brokers_.size()];
				}

				// Connect to new broker
				if (!connect_to_server(new_broker_id)) {
					RecordFailureEvent("Reconnect Fail Broker " + std::to_string(new_broker_id));
					LOG(ERROR) << "Failed to connect to replacement broker " << new_broker_id;
					return;
				}

				std::string reconn_msg = "Reconnect Success Broker " + std::to_string(new_broker_id) + " (from " + std::to_string(broker_id) + ")";
				RecordFailureEvent(reconn_msg);
				// Reset and try again with new broker
				try {
					send_batch_header();
				} catch (const std::exception& e) {
					LOG(ERROR) << "Failed to send batch header to replacement broker: " << e.what();
					RecordFailureEvent("Header Send Fail (Post-Reconnect) Broker " + std::to_string(new_broker_id) + " (" + e.what() + ")");
					return;
				}

				// Thread redirected from broker to new broker after failure

				broker_id = new_broker_id;
				sent_bytes = 0;
			}
		}

		// Mark that we've sent at least one batch
		has_sent_batch = true;
		size_t total_sent = total_batches_sent.fetch_add(1, std::memory_order_relaxed) + 1;

		// Log when batch is fully sent
		if (batch_count % 100 == 0 || batch_count == 1) {
			LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Fully sent batch " << batch_count 
			         << " to broker " << broker_id << " (" << len << " bytes, sent_bytes=" << sent_bytes << ")";
		}
		if (total_sent % 500 == 0) {
			LOG(INFO) << "Publisher: total_batches_sent=" << total_sent
			          << " total_batches_attempted=" << total_batches_attempted.load(std::memory_order_relaxed)
			          << " total_batches_failed=" << total_batches_failed.load(std::memory_order_relaxed);
		}

		// Verify all data was sent
		if (sent_bytes != len) {
			LOG(ERROR) << "PublishThread[" << pubQuesIdx << "]: Batch send incomplete! Sent " 
			          << sent_bytes << " of " << len << " bytes for batch " << batch_count 
			          << " to broker " << broker_id;
		}

		// Update batch sequence for next iteration
		batch_seq += num_threads_.load();
	}

	// IMPROVED: Keep connections alive for subscriber
	// Don't close data connections when publisher finishes - this would cause brokers to shutdown
	// The connections will be cleaned up when the Publisher object is destroyed
	// 
	// NOTE: We intentionally do NOT close sock and efd here to keep broker connections alive
	// This allows the subscriber to continue working after publisher finishes
	// Resources will be cleaned up in the Publisher destructor
	LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: Exiting main loop. Socket " << sock 
	         << " kept open for ACKs. publish_finished=" << publish_finished_ 
	         << ", shutdown=" << shutdown_;
}

void Publisher::SubscribeToClusterStatus() {
	// Prepare client info for initial request
	heartbeat_system::ClientInfo client_info;
	heartbeat_system::ClusterStatus cluster_status;

	{
		absl::MutexLock lock(&mutex_);
		for (const auto& it : nodes_) {
			client_info.add_nodes_info(it.first);
		}
	}

	// Create gRPC reader
	LOG(INFO) << "SubscribeToCluster: Creating gRPC reader for cluster status subscription...";
	std::unique_ptr<grpc::ClientReader<ClusterStatus>> reader(
			stub_->SubscribeToCluster(&context_, client_info));
	
	if (!reader) {
		LOG(ERROR) << "SubscribeToCluster: Failed to create gRPC reader. Check broker gRPC service availability.";
		return;
	}
	
	LOG(INFO) << "SubscribeToCluster: gRPC reader created successfully, waiting for cluster status...";

	// Process cluster status updates
	while (!shutdown_) {
		if (reader->Read(&cluster_status)) {
			LOG(INFO) << "SubscribeToCluster: Received cluster status update with " 
			         << cluster_status.new_nodes_size() << " new nodes";
			const auto& new_nodes = cluster_status.new_nodes();

			if (!new_nodes.empty()) {
				absl::MutexLock lock(&mutex_);

				// Adjust queue size based on number of brokers on first connection
				if (!connected_) {
					int num_brokers = 1 + new_nodes.size();
					queueSize_ /= num_brokers;
				}

				// Add new brokers (don't call AddPublisherThreads here - will be called in connection loop)
				for (const auto& addr : new_nodes) {
					int broker_id = GetBrokerId(addr);
					nodes_[broker_id] = addr;
					brokers_.emplace_back(broker_id);
				}

				// Sort brokers for deterministic round-robin assignment
				std::sort(brokers_.begin(), brokers_.end());
			}

			// If this is initial connection, connect to all brokers
			if (!connected_) {
				LOG(INFO) << "SubscribeToCluster: Initial connection - connecting to " 
				         << brokers_.size() << " brokers";
				
				// Connect to all brokers (including head node)
				for (int broker_id : brokers_) {
					LOG(INFO) << "SubscribeToCluster: Adding publisher threads for broker " << broker_id;
					if (!AddPublisherThreads(num_threads_per_broker_, broker_id)) {
						LOG(ERROR) << "Failed to add publisher threads for broker " << broker_id;
						return;
					}
				}

				// If no brokers were discovered, connect to head node (broker 0) as fallback
				if (brokers_.empty()) {
					LOG(WARNING) << "SubscribeToCluster: No brokers discovered, using head broker (0) as fallback";
					if (!AddPublisherThreads(num_threads_per_broker_, 0)) {
						LOG(ERROR) << "Failed to add publisher threads for head broker";
						return;
					}
					brokers_.push_back(0);
				}

				// Signal that we're connected
				connected_ = true;
				LOG(INFO) << "SubscribeToCluster: Connection established successfully. connected_=true";
			}
		} else {
			// Handle read error or end of stream
			if (!shutdown_) {
				static auto last_warning = std::chrono::steady_clock::now();
				static size_t read_fail_count = 0;
				auto now = std::chrono::steady_clock::now();
				read_fail_count++;
				
				// Log warning every 5 seconds with failure count
				if (now - last_warning > std::chrono::seconds(5)) {
					LOG(WARNING) << "SubscribeToCluster: reader->Read() returned false (stream ended or error). "
					            << "Failure count: " << read_fail_count 
					            << ". This may indicate gRPC connection issues or broker unavailability.";
					
					// Check if we're still waiting for initial connection
					if (!connected_) {
						LOG(ERROR) << "SubscribeToCluster: Initial connection not established after " 
						           << read_fail_count << " read attempts. "
						           << "Check if head broker gRPC service is running and accessible.";
					}
					
					last_warning = now;
				}
				
				// Add a small delay before reconnecting to avoid tight loop
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
	}

	// Finish the gRPC call
	grpc::Status status = reader->Finish();
	if (!status.ok() && !shutdown_) {
		LOG(ERROR) << "SubscribeToCluster failed: " << status.error_message();
	}
}

bool Publisher::AddPublisherThreads(size_t num_threads, int broker_id) {
	// Allocate buffers
	if (!pubQue_.AddBuffers(queueSize_)) {
		LOG(ERROR) << "Failed to add buffers for broker " << broker_id;
		return false;
	}

	// Create publisher threads
	for (size_t i = 0; i < num_threads; i++) {
		int thread_idx = num_threads_.fetch_add(1);
		threads_.emplace_back(&Publisher::PublishThread, this, broker_id, thread_idx);
	}

	return true;
}
