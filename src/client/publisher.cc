#include "publisher.h"
#include <random>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <thread>
#include <cstdlib>  // For getenv, atoi

namespace {
constexpr int kAckPortMin = 10000;
constexpr int kAckPortMax = 65535;
constexpr int kAckPortRange = kAckPortMax - kAckPortMin + 1;
}  // namespace

Publisher::Publisher(char topic[TOPIC_NAME_SIZE], std::string head_addr, std::string port, 
		int num_threads_per_broker, size_t message_size, size_t queueSize, 
		int order, SequencerType seq_type)
	: head_addr_(head_addr),
	port_(port),
	client_id_(GenerateRandomNum()),
	num_threads_per_broker_(num_threads_per_broker),
	message_size_(message_size),
	queueSize_((num_threads_per_broker > 0) ? (queueSize / static_cast<size_t>(num_threads_per_broker)) : queueSize),
	pubQue_(num_threads_per_broker_ * NUM_MAX_BROKERS, num_threads_per_broker_, client_id_, message_size, order),
	seq_type_(seq_type),
	sent_bytes_per_broker_(NUM_MAX_BROKERS),
	start_time_(std::chrono::steady_clock::now()),  // Initialize immediately; declaration order before acked_messages_per_broker_
	acked_messages_per_broker_(NUM_MAX_BROKERS){

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

	// Signal all threads to terminate [[CRITICAL: Atomic store for cross-thread visibility]]
	publish_finished_.store(true, std::memory_order_release);
	shutdown_.store(true, std::memory_order_release);
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

		// Wait for acknowledgment thread to initialize (with timeout — EpollAckThread may fail to start)
		constexpr auto ACK_THREAD_INIT_TIMEOUT = std::chrono::seconds(30);
		auto ack_wait_start = std::chrono::steady_clock::now();
		while (thread_count_.load(std::memory_order_acquire) != 1) {
			auto elapsed = std::chrono::steady_clock::now() - ack_wait_start;
			if (elapsed >= ACK_THREAD_INIT_TIMEOUT) {
				LOG(ERROR) << "Publisher::Init() timed out after " << ACK_THREAD_INIT_TIMEOUT.count()
				           << "s waiting for ACK thread. EpollAckThread may have failed (e.g. bind/listen).";
				break;
			}
			std::this_thread::yield();
		}
		thread_count_.store(0, std::memory_order_release);
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
	
	while (!connected_.load(std::memory_order_acquire)) {  // [[CRITICAL_FIX: Atomic load with acquire semantics]]
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
	
	if (!connected_.load(std::memory_order_acquire)) {  // [[CRITICAL_FIX: Atomic load]]
		LOG(ERROR) << "Publisher::Init() failed - cluster connection was not established. "
		          << "Publisher will not be able to send messages.";
	}

	// Initialize Corfu sequencer if needed
	if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
		corfu_client_ = std::make_unique<CorfuSequencerClient>(
				CORFU_SEQUENCER_ADDR + std::to_string(CORFU_SEQ_PORT));
	}

	// [[Issue 6]] Wait for all publisher threads to initialize with timeout
	constexpr auto THREAD_INIT_TIMEOUT = std::chrono::seconds(60);
	auto thread_wait_start = std::chrono::steady_clock::now();
	while (thread_count_.load(std::memory_order_acquire) != num_threads_.load(std::memory_order_acquire)) {
		auto elapsed = std::chrono::steady_clock::now() - thread_wait_start;
		if (elapsed >= THREAD_INIT_TIMEOUT) {
			LOG(ERROR) << "Publisher::Init() timed out after " << THREAD_INIT_TIMEOUT.count()
			           << "s waiting for thread_count_ (" << thread_count_.load(std::memory_order_relaxed)
			           << ") == num_threads_ (" << num_threads_.load(std::memory_order_relaxed) << ")";
			break;
		}
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

	size_t my_order = client_order_.fetch_add(1, std::memory_order_acq_rel);  // [[CRITICAL: Atomic RMW]]
#ifdef BATCH_OPTIMIZATION
	if (!pubQue_.Write(my_order, message, len, padded_total)) {
		LOG(ERROR) << "Failed to write message to queue (client_order=" << my_order << ")";
	}
#else
	const static size_t batch_size = BATCH_SIZE;
	static size_t i = 0;
	static size_t j = 0;
	size_t n = batch_size / (padded_total);
	if (n == 0) n = 1;
	if (!pubQue_.Write(i, my_order, message, len, padded_total)) {
		LOG(ERROR) << "Failed to write message to queue (client_order=" << my_order << ")";
	}
	j++;
	if (j == n) {
		i = (i + 1) % num_threads_.load();
		j = 0;
	}
#endif
}

bool Publisher::Poll(size_t n) {
	// [[LAST_PERCENT_ACK_FIX]] Seal and return reads before signaling finished.
	// If we set publish_finished_ first, threads that get nullptr from Read() may exit
	// before we've called SealAll(), dropping the last batches.
	WriteFinishedOrPuased();
	pubQue_.ReturnReads();
	publish_finished_.store(true, std::memory_order_release);

	// Wait for all messages to be queued
	// Use periodic spin-then-yield pattern for efficient polling
	// Spin for 1ms blocks, then yield once to reduce CPU waste while maintaining low latency
	constexpr auto SPIN_DURATION = std::chrono::milliseconds(1);
		while (client_order_.load(std::memory_order_acquire) < n) {
			auto spin_start = std::chrono::steady_clock::now();
			const auto spin_end = spin_start + SPIN_DURATION;
			while (std::chrono::steady_clock::now() < spin_end && client_order_.load(std::memory_order_acquire) < n) {
				Embarcadero::CXL::cpu_pause();
			}
			if (client_order_.load(std::memory_order_acquire) < n) {
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
		auto wait_start_time = std::chrono::steady_clock::now();
		auto last_log_time = wait_start_time;
		// [[CONFIG: Ack-wait spin]] 500µs spin when waiting for acks (burst-friendly); was 1ms.
		constexpr auto SPIN_DURATION = std::chrono::microseconds(500);
		
		// [[PHASE_4_BOUNDED_TIMEOUTS]] - Configurable timeout for ACK waits
		// Default 60s prevents infinite hang if broker fails; override via env var for tests
		const char* timeout_env = std::getenv("EMBARCADERO_ACK_TIMEOUT_SEC");
		int timeout_seconds = timeout_env ? std::atoi(timeout_env) : 60;  // 60s default for safety
		const auto timeout_duration = std::chrono::seconds(timeout_seconds);
		// [[FIX: ACK Race Condition]] Capture target ONCE - never reload inside loop
		// Reloading allowed concurrent Publish() calls to move the target, causing potential infinite wait
		const size_t target_acks = client_order_.load(std::memory_order_acquire);

		while (ack_received_.load(std::memory_order_acquire) < target_acks) {
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - wait_start_time);

			// Check timeout
			if (timeout_seconds > 0 && elapsed >= timeout_duration) {
				LOG(ERROR) << "[Publisher ACK Timeout]: Waited " << elapsed.count()
					<< " seconds for ACKs, received " << ack_received_ << " out of " << target_acks
					<< " (timeout=" << timeout_seconds << "s)";
				LOG(ERROR) << "[Publisher ACK Diagnostics]: ack_level=" << ack_level_
					<< ", last_ack_received=" << ack_received_ << ", client_order=" << target_acks;
				// Per-broker counts to pinpoint which broker(s) are short
				std::string per_broker;
				for (size_t i = 0; i < acked_messages_per_broker_.size(); i++) {
					if (i) per_broker += " ";
					per_broker += "B" + std::to_string(i) + "=" + std::to_string(acked_messages_per_broker_[i].load(std::memory_order_relaxed));
				}
				LOG(ERROR) << "[Publisher ACK Per-Broker]: " << per_broker;
				// Return failure - caller should handle timeout appropriately
				return false;  // Exit early on timeout
			}

			if(kill_brokers_){
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count() >= 100) {
					break;
				}
			}
			// Only log every 3 seconds to avoid spam; include per-broker acks for stall diagnosis
			if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 3) {
				std::string per_broker;
				for (size_t i = 0; i < acked_messages_per_broker_.size(); i++) {
					if (i) per_broker += " ";
					per_broker += "B" + std::to_string(i) + "=" + std::to_string(acked_messages_per_broker_[i].load(std::memory_order_relaxed));
				}
				LOG(INFO) << "Waiting for acknowledgments, received " << ack_received_.load(std::memory_order_relaxed) << " out of " << target_acks
					<< " (elapsed: " << elapsed.count() << "s"
					<< (timeout_seconds > 0 ? ", timeout: " + std::to_string(timeout_seconds) + "s" : "") << ") [" << per_broker << "]";
				last_log_time = now;
			}

			// [[REMOVED: co = client_order_.load()]] - This caused the race condition!
			auto spin_start = std::chrono::steady_clock::now();
			const auto spin_end = spin_start + SPIN_DURATION;
			while (std::chrono::steady_clock::now() < spin_end && ack_received_.load(std::memory_order_acquire) < target_acks) {
				Embarcadero::CXL::cpu_pause();
			}
			if (ack_received_.load(std::memory_order_acquire) < target_acks) {
				std::this_thread::yield();
			}
		}
	}

	// IMPROVED: Graceful disconnect - keep gRPC context alive for subscriber
	// Only set publish_finished flag, don't shutdown entire system
	// The gRPC context remains active to support subscriber cluster management
	// Publisher data connections are already closed by joined threads
	
	LOG(INFO) << "Publisher finished sending " << client_order_.load(std::memory_order_relaxed) << " messages, keeping cluster context alive for subscriber";
	
	// NOTE: We do NOT set shutdown_=true or cancel context here
	// This allows the subscriber to continue using the cluster management infrastructure
	// The context will be cleaned up when the Publisher object is destroyed
	return true;
}

void Publisher::DEBUG_check_send_finish() {
	WriteFinishedOrPuased();
	publish_finished_.store(true, std::memory_order_release);  // [[CRITICAL: Atomic store]]
	pubQue_.ReturnReads();

	// CRITICAL FIX: Don't join threads here as Poll() will handle thread cleanup
	// This prevents double-join issues and race conditions
	// DEBUG_check_send_finish: Signaled publishing completion, threads will be joined in Poll()
}

void Publisher::FailBrokers(size_t total_message_size, size_t message_size,
		double failure_percentage, 
		std::function<bool()> killbrokers) {
	kill_brokers_.store(true, std::memory_order_release);

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

		while (!shutdown_.load(std::memory_order_acquire) && total_sent_bytes_.load(std::memory_order_acquire) < bytes_to_kill_brokers) {
			std::this_thread::yield();
		}

		if (!shutdown_.load(std::memory_order_acquire)) {
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

		while (!shutdown_.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(measurement_interval_ms));
			size_t sum = 0;
			auto now = std::chrono::steady_clock::now();
			auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
			throughputFile << timestamp_ms; // Write timestamp

			for (size_t i = 0; i < num_brokers; i++) {
				size_t bytes = acked_messages_per_broker_[i].load(std::memory_order_acquire) * message_size;
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
			ack_port_ = kAckPortMin + (GenerateRandomNum() % kAckPortRange);
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
	// [[PERF: 1ms epoll timeout]] - Wake often to process incoming acks; 10ms added latency.
	constexpr int EPOLL_TIMEOUT_MS = 1;
	std::map<int, int> client_sockets; // Map: client_fd -> broker_id (value is broker_id)

	// Map to track the last received cumulative ACK per socket for calculating increments
	// Initializing with -1 assumes ACK IDs (logical_offset) start >= 0.
	// The first calculation becomes ack - (size_t)-1 which equals ack + 1.
	absl::flat_hash_map<int, size_t> prev_ack_per_sock;

	// Track state for reading initial broker ID
	enum class ConnState { WAITING_FOR_ID, READING_ACKS };
	std::map<int, ConnState> socket_state;
	std::map<int, std::pair<int, size_t>> partial_id_reads; // fd -> {partial_id, bytes_read}
	// Buffer partial ACK reads (size_t) so we don't discard bytes when recv returns < 8 bytes
	std::map<int, std::pair<size_t, size_t>> partial_ack_reads; // fd -> {ack_buffer, bytes_read}

thread_count_.fetch_add(1, std::memory_order_release);  // Signal that epoll loop is ready; Init loads with acquire

// Main epoll loop
while (!shutdown_.load(std::memory_order_acquire)) {
	int num_events = epoll_wait(epoll_fd, events.data(), max_events, EPOLL_TIMEOUT_MS);

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
			// [[DEFENSIVE]] Stale event for fd we already closed (e.g. EPOLL_CTL_DEL race).
			if (client_sockets.find(client_sock) == client_sockets.end()) {
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
				continue;
			}
			ConnState current_state = socket_state[client_sock];
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
						partial_ack_reads[client_sock] = {0, 0}; // Init ACK read buffer for this connection
						// Continue reading potential ACK data in the same loop iteration
					}
					// If ID still not complete, loop will try recv() again if more data indicated by epoll
				}else if(current_state == ConnState::READING_ACKS){
					// [[CRITICAL_FIX: Buffer partial ACK reads]] - Don't discard bytes when recv returns < sizeof(size_t).
					// Otherwise we can lose ACK data and ack_received_ never reaches client_order_ (test hangs).
					auto& partial = partial_ack_reads[client_sock];
					size_t needed = sizeof(size_t) - partial.second;
					ssize_t recv_ret = recv(client_sock,
							reinterpret_cast<char*>(&partial.first) + partial.second,
							needed, 0);
					if (recv_ret == 0) { connection_error_or_closed = true; break; }
					if (recv_ret < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) break; // No more data now
						if (errno == EINTR) continue; // Retry read
						LOG(ERROR) << "AckThread: recv error reading ACK bytes on fd " << client_sock << ": " << strerror(errno);
						connection_error_or_closed = true; break;
					}
					partial.second += static_cast<size_t>(recv_ret);
					if (partial.second != sizeof(size_t)) {
						// Partial ACK still in progress, wait for more data
						break;
					}

					// --- Process Full ACK Value ---
					size_t acked_msg = partial.first;
					partial_ack_reads[client_sock] = {0, 0}; // Reset for next ACK
					int broker_id = client_sockets[client_sock]; // Get broker ID

					// Check if broker_id is valid (should be if state is READING_ACKS)
					if (broker_id < 0) {
						LOG(ERROR) << "AckThread: Invalid broker_id (-1) for fd " << client_sock << " in READING_ACKS state.";
						connection_error_or_closed = true; break;
					}

					size_t prev_acked = prev_ack_per_sock[client_sock]; // Assumes key exists

					if (acked_msg >= prev_acked || prev_acked == (size_t)-1) { // Check for valid cumulative value
						// [[CRITICAL_FIX: Handle first ACK correctly to avoid unsigned underflow]]
						// If prev_acked == (size_t)-1, this is the first ACK from this broker
						// Direct subtraction would underflow: acked_msg - (size_t)-1 = huge number
						// We must handle first ACK specially: new_acked_msgs = acked_msg (not acked_msg - (-1))
						size_t new_acked_msgs;
						if (prev_acked == (size_t)-1) {
							// First ACK from this broker - use value directly (no previous to subtract)
							new_acked_msgs = acked_msg;
						} else {
							// Subsequent ACK - calculate increment from previous
							new_acked_msgs = acked_msg - prev_acked;
						}
						if (new_acked_msgs > 0) {
							// [[DIAGNOSTIC: Log ACK processing]]
							static thread_local size_t ack_process_count = 0;
							size_t prev_total = ack_received_.load(std::memory_order_relaxed);
							if (++ack_process_count % 100 == 0 || prev_total % 10000 == 0) {
								VLOG(2) << "EpollAckThread: B" << broker_id << " acked_msg=" << acked_msg 
								        << " prev_acked=" << (prev_acked == (size_t)-1 ? -1 : (int64_t)prev_acked)
								        << " new_acked=" << new_acked_msgs 
								        << " total_ack_received=" << (prev_total + new_acked_msgs)
								        << " (count=" << ack_process_count << ")";
							}
							acked_messages_per_broker_[broker_id].fetch_add(new_acked_msgs, std::memory_order_release);
							ack_received_.fetch_add(new_acked_msgs, std::memory_order_release);
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
				partial_ack_reads.erase(client_sock); // Clean up partial ACK state too
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
		// [[FIX: Throughput]] Increased event array, reduced timeout for high-throughput
		struct epoll_event events[64];
		bool running = true;
		size_t sent_bytes = 0;

		while (!shutdown_.load(std::memory_order_acquire) && running) {
			// [[FIX: Throughput]] 1ms timeout instead of 1000ms for fast response
			int n = epoll_wait(efd, events, 64, 1);
			if (n == 0) {
				// Timeout - check if we should continue
				if (shutdown_.load(std::memory_order_acquire) || publish_finished_.load(std::memory_order_acquire)) {
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
	while (!shutdown_.load(std::memory_order_acquire)) {
		size_t len;
		int bytesSent = 0;

#ifdef BATCH_OPTIMIZATION
		// Read a batch from the queue
		Embarcadero::BatchHeader* batch_header = 
			static_cast<Embarcadero::BatchHeader*>(pubQue_.Read(pubQuesIdx));

		// Skip if no batch is available
		if (batch_header == nullptr || batch_header->total_size == 0) {
			if (publish_finished_.load(std::memory_order_acquire) || shutdown_.load(std::memory_order_acquire)) {
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
				         << publish_finished_.load(std::memory_order_relaxed) << ", shutdown=" << shutdown_.load(std::memory_order_relaxed) 
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
						// [[FIX: Throughput]] 1ms timeout, larger event array
						struct epoll_event events[64];
						int n = epoll_wait(efd, events, 64, 1);

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
			if (shutdown_.load(std::memory_order_acquire) && !publish_finished_.load(std::memory_order_acquire)) {
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
				// [[FIX: Throughput]] 1ms timeout, larger event array
				struct epoll_event events[64];
				int n = epoll_wait(efd, events, 64, 1);

				if (n == -1) {
					LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
					break;
				}

				// [[FIX: Exponential Backoff]] Halve on congestion (was 75% linear reduction)
				// Recovers faster after transient network pressure clears
				zero_copy_send_limit = std::max(zero_copy_send_limit / 2, 1UL << 16); // Halve, min 64KB
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
	         << " kept open for ACKs. publish_finished=" << publish_finished_.load(std::memory_order_relaxed) 
	         << ", shutdown=" << shutdown_.load(std::memory_order_relaxed);
}

void Publisher::SubscribeToClusterStatus() {
	heartbeat_system::ClusterStatus cluster_status;
	read_fail_count_ = 0;  // [[Issue 7]] Reset on entry

	// [[Issue 4]] Outer loop: re-establish reader when Read() fails or stream ends
	while (!shutdown_.load(std::memory_order_acquire)) {
		heartbeat_system::ClientInfo client_info;
		{
			absl::MutexLock lock(&mutex_);
			for (const auto& it : nodes_) {
				client_info.add_nodes_info(it.first);
			}
		}

		LOG(INFO) << "SubscribeToCluster: Creating gRPC reader for cluster status subscription...";
		std::unique_ptr<grpc::ClientReader<ClusterStatus>> reader(
				stub_->SubscribeToCluster(&context_, client_info));
		
		if (!reader) {
			LOG(ERROR) << "SubscribeToCluster: Failed to create gRPC reader. Check broker gRPC service availability.";
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}
		
		LOG(INFO) << "SubscribeToCluster: gRPC reader created successfully, waiting for cluster status...";

		// Inner loop: process reads until Read() fails or shutdown
		while (!shutdown_.load(std::memory_order_acquire)) {
			if (reader->Read(&cluster_status)) {
			LOG(INFO) << "SubscribeToCluster: Received cluster status update with " 
			         << cluster_status.new_nodes_size() << " new nodes";
			const auto& new_nodes = cluster_status.new_nodes();

			if (!new_nodes.empty()) {
				absl::MutexLock lock(&mutex_);

				// Adjust queue size based on number of brokers on first connection
				if (!connected_.load(std::memory_order_acquire)) {  // [[CRITICAL_FIX: Atomic load]]
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
			if (!connected_.load(std::memory_order_acquire)) {  // [[CRITICAL_FIX: Atomic load]]
				LOG(INFO) << "SubscribeToCluster: Initial connection - connecting to " 
				         << brokers_.size() << " brokers";
				// [[Issue 3]] Read queueSize_ under mutex before calling AddPublisherThreads
				size_t qsize;
				{ absl::MutexLock lock(&mutex_); qsize = queueSize_; }
				bool all_connected = true;
				for (int broker_id : brokers_) {
					LOG(INFO) << "SubscribeToCluster: Adding publisher threads for broker " << broker_id;
					if (!AddPublisherThreads(num_threads_per_broker_, broker_id, qsize)) {
						LOG(ERROR) << "Failed to add publisher threads for broker " << broker_id;
						all_connected = false;
						break;
					}
				}
				if (brokers_.empty()) {
					LOG(WARNING) << "SubscribeToCluster: No brokers discovered, using head broker (0) as fallback";
					if (!AddPublisherThreads(num_threads_per_broker_, 0, qsize)) {
						LOG(ERROR) << "Failed to add publisher threads for head broker";
						all_connected = false;
					} else {
						brokers_.push_back(0);
					}
				}

				// Signal that we're connected (CRITICAL: Set even if reader->Read() fails later)
				if (all_connected) {
					connected_.store(true, std::memory_order_release);  // [[CRITICAL_FIX: Atomic store with release semantics]]
					LOG(INFO) << "SubscribeToCluster: Connection established successfully. connected_=true";
				}
			}
			} else {
				// [[Issue 4]] Read failed – break inner loop, Finish(), then outer loop re-establishes reader
				auto now = std::chrono::steady_clock::now();
				if (read_fail_count_ == 0) last_read_warning_ = now;
				read_fail_count_++;
				if (now - last_read_warning_ > std::chrono::seconds(5)) {
					LOG(WARNING) << "SubscribeToCluster: reader->Read() returned false. Failure count: " << read_fail_count_
					            << ". Re-establishing gRPC reader.";
					if (!connected_.load(std::memory_order_acquire)) {
						LOG(ERROR) << "SubscribeToCluster: Initial connection not established after " << read_fail_count_ << " read attempts.";
					}
					last_read_warning_ = now;
				}
				break;  // Exit inner loop → Finish() → outer loop creates new reader
			}
		}

		// [[Issue 4]] Finish current reader before re-establishing
		grpc::Status status = reader->Finish();
		if (!status.ok() && !shutdown_.load(std::memory_order_acquire)) {
			LOG(ERROR) << "SubscribeToCluster stream ended: " << status.error_message() << ". Re-establishing.";
		}
		if (shutdown_.load(std::memory_order_acquire)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Back off before re-connect
	}
}

bool Publisher::AddPublisherThreads(size_t num_threads, int broker_id, size_t queue_size) {
	// [[Issue 3]] Use queue_size parameter (caller reads under mutex)
	if (!pubQue_.AddBuffers(queue_size)) {
		LOG(ERROR) << "Failed to add buffers for broker " << broker_id;
		return false;
	}

	// [[Issue 5]] Create threads with cleanup on partial failure
	size_t created = 0;
	try {
		for (size_t i = 0; i < num_threads; i++) {
			int thread_idx = num_threads_.fetch_add(1);
			threads_.emplace_back(&Publisher::PublishThread, this, broker_id, thread_idx);
			created++;
		}
	} catch (const std::exception& e) {
		LOG(ERROR) << "AddPublisherThreads: failed after " << created << " threads: " << e.what();
		// Rollback: join created threads and revert num_threads_
		for (size_t j = 0; j < created; j++) {
			if (threads_.back().joinable()) threads_.back().join();
			threads_.pop_back();
			num_threads_.fetch_sub(1);
		}
		return false;
	}
	return true;
}
