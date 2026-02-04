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
#include "staging_pool.h"
#include "../disk_manager/disk_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "../embarlet/topic_manager.h"
#include "../common/performance_utils.h"
#include "../common/wire_formats.h"

namespace Embarcadero {

// [[STALL_DIAG]] Threshold (µs) above which we log recv() as blocking stall (50 ms)
static constexpr int64_t kRecvStallThresholdUs = 50 * 1000;

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
	// [[DIAGNOSTIC]] Verify kernel actually applied the buffer sizes
	int actual_rcv = 0, actual_snd = 0;
	socklen_t len = sizeof(actual_rcv);
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_rcv, &len);
	getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual_snd, &len);
	// Kernel doubles the value; we expect actual >= buffer_size
	if (actual_rcv < buffer_size) {
		LOG(WARNING) << "SO_RCVBUF capped: requested=" << buffer_size << " actual=" << actual_rcv
		             << " (fd=" << fd << "). Check net.core.rmem_max";
	}
	LOG(INFO) << "SetAcceptedSocketBuffers fd=" << fd << " SO_RCVBUF=" << actual_rcv
	          << " SO_SNDBUF=" << actual_snd;
}

// [[PERF]] Named constants for recovery loop (avoid magic numbers)
static constexpr int kRecoveryCheckInterval = 100;  // Check every N epoll iterations
static constexpr int kMaxPBRRetries = 20;           // WAIT_PBR_SLOT: recovery attempts before fallback handoff

// [[KNOWN_DEVIATION]] offset_entry.written is volatile size_t in CXL; casting to (size_t*) for
// __atomic_* is undefined in C++ (object is not an atomic type). GCC/clang support it in practice.
// A standards-clean fix would require C++20 std::atomic_ref or changing CXL layout.
void NetworkManager::UpdateWrittenForOrder0(TInode* tinode, size_t logical_offset, uint32_t num_msg) {
	if (!tinode) return;
	const size_t new_written = logical_offset + num_msg;
	volatile size_t* written_ptr = &tinode->offsets[broker_id_].written;
	size_t current;
	do {
		current = __atomic_load_n((size_t*)written_ptr, __ATOMIC_ACQUIRE);
		if (new_written <= current) break;
	} while (!__atomic_compare_exchange_n(
		(size_t*)written_ptr, &current, new_written,
		false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
	CXL::flush_cacheline(CXL::ToFlushable(written_ptr));
}

void NetworkManager::CompleteBatchInCXL(BatchHeader* batch_header_location, uint32_t num_msg, TInode* tinode, size_t logical_offset) {
	// [[PERF]] Removed std::chrono profiling from hot path - adds ~100-200ns overhead per batch
	if (!batch_header_location) return;
	__atomic_store_n(&batch_header_location->num_msg, num_msg, __ATOMIC_RELEASE);
	__atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE);
	CXL::flush_cacheline(batch_header_location);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(batch_header_location) + 64);
	if (tinode && tinode->order == 0) {
		UpdateWrittenForOrder0(tinode, logical_offset, num_msg);
	}
	CXL::store_fence();
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

NetworkManager::NetworkManager(int broker_id, int num_reqReceive_threads, bool skip_networking)
	: request_queue_(64),
	large_msg_queue_(10000),
	broker_id_(broker_id),
	num_reqReceive_threads_(num_reqReceive_threads) {

		// [[SEQUENCER_ONLY_HEAD_NODE]] Skip all networking threads when skip_networking is true
		// Sequencer-only head node does not accept client connections
		if (skip_networking) {
			LOG(INFO) << "[SEQUENCER_ONLY] NetworkManager: skip_networking=true, no listener/receiver threads";
			return;
		}

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

			bool recv_direct = config.network.recv_direct_to_cxl.get();
			LOG(INFO) << "NetworkManager: Non-blocking mode enabled "
			          << "(staging_pool=" << num_buffers << "×" << buffer_size_mb << "MB, recv_direct_to_cxl="
			          << (recv_direct ? "true" : "false") << ")";
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
	// Log publish pipeline profile first (before joining threads) so it is emitted even if process is killed during shutdown
	LogPublishPipelineProfile();
	google::FlushLogFiles(google::GLOG_INFO);  // Ensure profile is on disk before join (SIGTERM during join can abort before flush)

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

void NetworkManager::RecordProfile(PublishPipelineComponent c, uint64_t ns) {
	if (c < 0 || c >= kNumPipelineComponents) return;
	profile_total_ns_[c].fetch_add(ns, std::memory_order_relaxed);
	profile_count_[c].fetch_add(1, std::memory_order_relaxed);
}

void NetworkManager::LogPublishPipelineProfile() {
	static const char* kComponentNames[] = {
		"DrainHeader",
		"ReserveBLogSpace",
		"DrainPayloadToBuffer",
		"ReservePBRSlotAndWriteEntry",
		"CompleteBatchInCXL",
		"GetCXLBuffer",
		"CopyAndFlushPayload",
	};
	uint64_t total_ns = 0;
	for (int c = 0; c < kNumPipelineComponents; ++c) {
		total_ns += profile_total_ns_[c].load(std::memory_order_relaxed);
	}
	if (total_ns == 0) {
		LOG(INFO) << "[PublishPipelineProfile] No samples yet.";
		// Still log key metrics for bandwidth diagnostics
		uint64_t rf = metric_ring_full_.load(std::memory_order_relaxed);
		uint64_t bd = metric_batches_dropped_.load(std::memory_order_relaxed);
		if (rf > 0 || bd > 0) {
			LOG(INFO) << "[BrokerMetrics] ring_full=" << rf << " batches_dropped=" << bd;
		}
		return;
	}
	LOG(INFO) << "[PublishPipelineProfile] === Aggregated time per component ===";
	for (int c = 0; c < kNumPipelineComponents; ++c) {
		uint64_t ns = profile_total_ns_[c].load(std::memory_order_relaxed);
		uint64_t cnt = profile_count_[c].load(std::memory_order_relaxed);
		uint64_t avg_ns = cnt ? (ns / cnt) : 0;
		double pct = (100.0 * static_cast<double>(ns)) / static_cast<double>(total_ns);
		LOG(INFO) << "[PublishPipelineProfile]   " << kComponentNames[c]
			<< ": total=" << (ns / 1000) << " us, count=" << cnt
			<< ", avg=" << avg_ns << " ns, " << pct << "% of pipeline time";
	}
	LOG(INFO) << "[PublishPipelineProfile]   TOTAL: " << (total_ns / 1000) << " us ("
		<< (total_ns / 1e9) << " s) across all components";
	LOG(INFO) << "[PublishPipelineProfile] ========================================";

	// Bandwidth diagnostics: key counters for regression analysis (EAGAIN is client-side)
	uint64_t ring_full = metric_ring_full_.load(std::memory_order_relaxed);
	uint64_t batches_dropped = metric_batches_dropped_.load(std::memory_order_relaxed);
	uint64_t cxl_retries = metric_cxl_retries_.load(std::memory_order_relaxed);
	uint64_t staging_exhausted = metric_staging_exhausted_.load(std::memory_order_relaxed);
	if (ring_full > 0 || batches_dropped > 0 || cxl_retries > 0 || staging_exhausted > 0) {
		LOG(INFO) << "[BrokerMetrics] ring_full=" << ring_full
			<< " batches_dropped=" << batches_dropped
			<< " cxl_retries=" << cxl_retries
			<< " staging_exhausted=" << staging_exhausted;
	}
}

void NetworkManager::EnsureCachedTopicValid(ConnectionState* state) {
	if (!state || !cxl_manager_) return;
	state->batch_count_since_validate++;
	if (state->batch_count_since_validate >= kCachedTopicValidateInterval) {
		state->batch_count_since_validate = 0;
		Topic* fresh = cxl_manager_->GetTopicPtr(state->handshake.topic);
		if (fresh != state->cached_topic)
			state->cached_topic = fresh;
	}
}

bool NetworkManager::CheckPBRWatermark(ConnectionState* state, int pct) {
	if (!cxl_manager_ || !state) return false;
	return state->cached_topic
		? cxl_manager_->IsPBRAboveHighWatermark(static_cast<Topic*>(state->cached_topic), pct)
		: cxl_manager_->IsPBRAboveHighWatermark(state->handshake.topic, pct);
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
			threads_.emplace_back(&NetworkManager::AckThread, this, std::string(handshake.topic), handshake.ack, ack_fd, local_ack_efd);
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

		auto t_recv_hdr_start = std::chrono::steady_clock::now();
		ssize_t bytes_read = recv(client_socket, &batch_header, sizeof(BatchHeader), 0);
		auto t_recv_hdr_end = std::chrono::steady_clock::now();
		auto hdr_block_us = std::chrono::duration_cast<std::chrono::microseconds>(t_recv_hdr_end - t_recv_hdr_start).count();
		if (hdr_block_us >= kRecvStallThresholdUs) {
			LOG(WARNING) << "[STALL] recv batch header blocked " << (hdr_block_us / 1000) << "ms client_id=" << handshake.client_id << " bytes_read=" << bytes_read;
		}
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
			auto t_partial = std::chrono::steady_clock::now();
			ssize_t recv_ret = recv(client_socket,
					((uint8_t*)&batch_header) + bytes_read,
					sizeof(BatchHeader) - bytes_read,
					0);
			auto partial_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_partial).count();
			if (partial_us >= kRecvStallThresholdUs) {
				LOG(WARNING) << "[STALL] recv batch header (partial) blocked " << (partial_us / 1000) << "ms client_id=" << handshake.client_id << " recv_ret=" << recv_ret;
			}
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
		// BLOCKING MODE: Wait indefinitely for ring space - NEVER close connection
		static std::atomic<size_t> blocking_ring_full_count{0};
		size_t wait_iterations = 0;
		static std::atomic<size_t> perf_sample_counter{0};

		auto t0 = std::chrono::high_resolution_clock::now();

		while (!buf && !stop_threads_) {
			non_emb_seq_callback = cxl_manager_->GetCXLBuffer(batch_header, handshake.topic, buf,
					segment_header, logical_offset, seq_type, batch_header_location);

			if (buf != nullptr) {
				break;  // Success
			}

			// Ring full - wait for consumer to make space
			wait_iterations++;
			size_t total_ring_full = blocking_ring_full_count.fetch_add(1, std::memory_order_relaxed) + 1;

			// Log first few waits and every 1000th wait
			if (total_ring_full <= 10 || total_ring_full % 1000 == 0) {
				LOG(WARNING) << "NetworkManager (blocking): Ring full, waiting for space "
				             << "(client_id=" << handshake.client_id
				             << ", wait_iterations=" << wait_iterations
				             << ", total_ring_full=" << total_ring_full << ")";
			}

			// Small sleep to avoid busy-wait (100µs = 0.1ms)
			// This allows consumer threads to make progress
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}

		if (!buf) {
			// Only happens if stop_threads_ is set (shutdown)
			LOG(WARNING) << "NetworkManager (blocking): Shutting down, closing connection to client_id="
			             << handshake.client_id;
			break;
		}
		
		auto t1 = std::chrono::high_resolution_clock::now();

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
		auto t2 = std::chrono::high_resolution_clock::now();
		size_t read = 0;
		bool batch_data_complete = false;
		static std::atomic<size_t> recv_payload_stall_count{0};
		while (running && !stop_threads_) {
			// [[REVERT]] Removed MSG_WAITALL - blocking for full batch may reduce parallelism
			auto t_recv_payload = std::chrono::steady_clock::now();
			bytes_read = recv(client_socket, (uint8_t*)buf + read, to_read, 0);
			auto recv_payload_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_recv_payload).count();
			if (recv_payload_us >= kRecvStallThresholdUs) {
				size_t cnt = recv_payload_stall_count.fetch_add(1, std::memory_order_relaxed) + 1;
				LOG(WARNING) << "[STALL] recv payload blocked " << (recv_payload_us / 1000) << "ms batch_seq=" << batch_header.batch_seq
				             << " remaining=" << to_read << " total_stall_count=" << cnt;
			}
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
		auto t3 = std::chrono::high_resolution_clock::now();

		// [[PERF INSTRUMENTATION]] Log timing breakdown every 1000th batch
		size_t sample_count = perf_sample_counter.fetch_add(1, std::memory_order_relaxed) + 1;
		if (sample_count % 1000 == 0) {
			auto getcxl_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
			auto prep_us = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
			auto recv_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
			LOG(INFO) << "[PERF] Batch " << sample_count << ": GetCXLBuffer=" << getcxl_us << "us, Prep=" << prep_us << "us, Recv=" << recv_us << "us";
		}
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

	// Whether we're using BlogMessageHeader (Order 5 + env): used only to skip v1 validation when publisher sent v2.
	TInode* tinode = nullptr;
	bool is_blog_header_enabled = false;
	if (seq_type == EMBARCADERO) {
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
		// For Sequencer 5, batch is complete once all data is received (to_read == 0)
		// Now it's safe to mark batch as complete for ALL order levels
		__atomic_store_n(&batch_header_location->num_msg, batch_header.num_msg, __ATOMIC_RELEASE);
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
		// [[CRITICAL: ORDER=0 + ACK=1]] Blocking path must update written so GetOffsetToAck/AckThread can send ACKs.
		// Non-blocking path uses CompleteBatchInCXL which calls UpdateWrittenForOrder0; blocking path does not,
		// so written was never updated → GetOffsetToAck always returned 0 → client ACK timeout.
		if (tinode && tinode->order == 0) {
			UpdateWrittenForOrder0(tinode, logical_offset, batch_header.num_msg);
		}
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
			// Get new messages from CXL manager. Narrow mutex: copy-out, call without lock, copy-in.
			size_t local_offset;
			{
				absl::MutexLock lock(&(*cached_state_ptr)->mu);
				local_offset = (*cached_state_ptr)->last_offset;
			}
			if (order == 5) {
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
				batch_meta.header_version = (order == 5 && HeaderUtils::ShouldUseBlogHeader()) ? 2 : 1;
				batch_meta.flags = 0;
				VLOG(2) << "SubscribeNetworkThread: Sending batch metadata, total_order=" << batch_total_order
				        << ", num_messages=" << num_messages << ", topic=" << topic;
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
			{
				absl::MutexLock lock(&(*cached_state_ptr)->mu);
				(*cached_state_ptr)->last_offset = local_offset;
				(*cached_state_ptr)->last_addr = local_addr;
			}
			// [[BLOG_HEADER: Split large messages with version-aware boundary]]
			bool using_blog_header = (order == 5 && HeaderUtils::ShouldUseBlogHeader());
			while (messages_size > zero_copy_send_limit) {
				struct LargeMsgRequest r;
				r.msg = msg;
				size_t padded_size = 0;
				if (using_blog_header) {
					Embarcadero::BlogMessageHeader* v2_hdr =
						reinterpret_cast<Embarcadero::BlogMessageHeader*>(msg);
					size_t payload_size = v2_hdr->size;
					if (!wire::ValidateV2Payload(payload_size, messages_size)) {
						LOG(ERROR) << "SubscribeNetworkThread: Invalid v2 payload_size=" << payload_size
						           << " for splitting, messages_size=" << messages_size;
						break;
					}
					padded_size = wire::ComputeStrideV2(payload_size);
				} else {
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
					break;
				}
				int mod = zero_copy_send_limit % padded_size;
				r.len = zero_copy_send_limit - mod;
				large_msg_queue_.blockingWrite(r);
				msg = (uint8_t*)msg + r.len;
				messages_size -= r.len;
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
				LOG(ERROR) << "Error sending data: " << strerror(errno) << ", to_send: " << to_send;
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

		// [[PHASE_2]] Use CompletionVector instead of replication_done array
		// CV is 8 bytes (single uint64_t) vs 256 bytes (32×8 array) = 32× bandwidth reduction
		if (seq_type == EMBARCADERO) {
			CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
				reinterpret_cast<uint8_t*>(cxl_manager_->GetCXLAddr()) + Embarcadero::kCompletionVectorOffset);
			CompletionVectorEntry* my_cv_entry = &cv[broker_id_];

			// Read CV (tail replica updates this after replication completes)
			CXL::flush_cacheline(my_cv_entry);
			CXL::load_fence();
			uint64_t completed_pbr_index = my_cv_entry->completed_pbr_head.load(std::memory_order_acquire);

			// Sentinel (uint64_t)-1 = no progress; 0 is valid (first batch completed)
			if (completed_pbr_index == static_cast<uint64_t>(-1)) {
				return (size_t)-1;  // No replication progress yet
			}

			// [[PHASE_2_FIX]] pbr_index is now absolute (never wraps), map to ring position
			BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(cxl_manager_->GetCXLAddr()) + tinode->offsets[broker_id_].batch_headers_offset);

			// Calculate ring size in number of BatchHeaders
			size_t ring_size_bytes = BATCHHEADERS_SIZE;
			size_t ring_size = ring_size_bytes / sizeof(BatchHeader);

			// Map absolute pbr_index to ring position using modulo
			size_t ring_position = completed_pbr_index % ring_size;
			BatchHeader* completed_batch = ring_start + ring_position;

			// Read batch header to get logical offset range
			CXL::flush_cacheline(completed_batch);
			CXL::load_fence();
			size_t start_offset = completed_batch->start_logical_offset;
			size_t num_msg = completed_batch->num_msg;
			size_t last_offset = start_offset + num_msg - 1;

			// [[ACK_LEVEL_2_COUNT_SEMANTICS]] - Client expects message COUNT, not last offset.
			return last_offset + 1;  // Convert last_offset (0-based) to message count
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
				// [[CRITICAL_FIX: Invalidate cache before reading written for ORDER=0]]
				// UpdateWrittenForOrder0 flushes from writer; AckThread must invalidate to see it
				volatile uint64_t* written_ptr = &tinode->offsets[broker_id_].written;
				CXL::flush_cacheline(const_cast<const void*>(
					reinterpret_cast<const volatile void*>(written_ptr)));
				CXL::load_fence();
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
			// [[CRITICAL_FIX: Invalidate cache before reading written for ORDER=0]]
			// UpdateWrittenForOrder0 flushes from writer; AckThread must invalidate to see it
			volatile uint64_t* written_ptr = &tinode->offsets[broker_id_].written;
			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(written_ptr)));
			CXL::load_fence();
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

void NetworkManager::AckThread(std::string topic, uint32_t ack_level, int ack_fd, int ack_efd) {
	struct epoll_event events[10];
	char buf[1];

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

	// [[DIAGNOSTIC: Confirm broker_id was sent to client]]
	LOG(INFO) << "AckThread: Broker " << broker_id_ << " sent broker_id to client for topic='" << topic << "'";

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
		TInode* tinode = (TInode*)cxl_manager_->GetTInode(topic.c_str());
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
					CXL::flush_cacheline(CXL::ToFlushable(ordered_ptr));
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
			cached_ack = GetOffsetToAck(topic.c_str(), ack_level);
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
			size_t ack = (cached_ack != (size_t)-1) ? cached_ack : GetOffsetToAck(topic.c_str(), ack_level);
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
				cached_ack = GetOffsetToAck(topic.c_str(), ack_level);
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
			VLOG(2) << "AckThread: Broker " << broker_id_ << " sending ack for " << ack << " messages (prev=" << next_to_ack_offset-1 << ")";
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
    // [[PERF]] Removed std::chrono profiling from hot path
    size_t remaining = sizeof(BatchHeader) - state->header_offset;

    ssize_t n = recv(fd, reinterpret_cast<uint8_t*>(&state->batch_header) + state->header_offset,
                     remaining, MSG_DONTWAIT);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
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
 * Drains payload data into an arbitrary buffer (e.g. CXL) using non-blocking recv.
 * [[RECV_DIRECT_TO_CXL]] Same logic as DrainPayload but destination is caller-provided.
 * [[PERF]] Drains in a loop until EAGAIN or complete: profile showed ~20 recv() calls per batch
 * (3749/184) because we previously did one recv per epoll wakeup; looping reduces syscalls and
 * epoll round-trips per batch.
 * @return true when payload is complete, false if more data needed or error
 */
bool NetworkManager::DrainPayloadToBuffer(int fd, ConnectionState* state, void* dest_buf) {
    // [[PERF]] One timestamp around whole call (not per-iteration) so we can measure wall-clock % without hot-path overhead.
    auto t0 = std::chrono::steady_clock::now();
    if (!dest_buf) {
        LOG(ERROR) << "DrainPayloadToBuffer: dest_buf is nullptr (fd=" << fd << ")";
        return false;
    }
    uint8_t* dest = reinterpret_cast<uint8_t*>(dest_buf);
    const size_t total_size = state->batch_header.total_size;

    static thread_local size_t recv_calls_this_thread = 0;
    static thread_local size_t batches_complete_this_thread = 0;
    static thread_local size_t total_bytes_drained_this_thread = 0;

    while (state->payload_offset < total_size) {
        size_t remaining = total_size - state->payload_offset;
        ssize_t n = recv(fd, dest + state->payload_offset, remaining, MSG_DONTWAIT);
        recv_calls_this_thread++;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            LOG(ERROR) << "DrainPayloadToBuffer: recv failed: " << strerror(errno) << " (fd=" << fd << ")";
            return false;
        }
        if (n == 0) {
            LOG(WARNING) << "DrainPayloadToBuffer: Connection closed, fd=" << fd;
            return false;
        }
        state->payload_offset += static_cast<size_t>(n);
    }

    bool complete = (state->payload_offset == total_size);
    if (complete) {
        batches_complete_this_thread++;
        total_bytes_drained_this_thread += total_size;
        if (batches_complete_this_thread % 100 == 0) {
            size_t avg_recv_per_batch = recv_calls_this_thread / batches_complete_this_thread;
            size_t avg_bytes_per_recv = (recv_calls_this_thread > 0)
                ? (total_bytes_drained_this_thread / recv_calls_this_thread) : 0;
            LOG(INFO) << "[DrainPayloadToBuffer] Avg recv calls per batch (this thread): "
                      << avg_recv_per_batch
                      << ", avg bytes per recv: " << avg_bytes_per_recv
                      << " (recv_calls=" << recv_calls_this_thread
                      << " batches=" << batches_complete_this_thread << ")";
        }
        auto t1 = std::chrono::steady_clock::now();
        RecordProfile(kDrainPayloadToBuffer, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        VLOG(3) << "DrainPayloadToBuffer: Complete. fd=" << fd << " batch_seq=" << state->batch_header.batch_seq;
    }
    return complete;
}

/**
 * Check if staging pool has recovered capacity and resume paused sockets
 */
void NetworkManager::CheckStagingPoolRecovery(
        int epoll_fd,
        absl::flat_hash_map<int, std::shared_ptr<ConnectionState>>& connections) {

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
                                         std::string(conn.handshake.topic), conn.handshake.ack,
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
    absl::flat_hash_map<int, std::shared_ptr<ConnectionState>> connections;

    LOG(INFO) << "PublishReceiveThread started (epoll_fd=" << epoll_fd << ")";

    // Cache config once per thread (avoid hot-loop config access)
    const bool recv_direct_cached = GetConfig().config().network.recv_direct_to_cxl.get();
    const size_t max_batch_size_cached = GetConfig().config().storage.batch_size.get();
    const int pbr_high_watermark_pct = GetConfig().config().network.pbr_high_watermark_pct.get();

    // Phase 3: Batch recovery check counter for optimization (kRecoveryCheckInterval, kMaxPBRRetries at file scope)
    int recovery_check_counter = 0;

    // Periodic profile log (every 10s) so profile is visible even when process is killed with SIGTERM (destructor may not run)
    static std::atomic<std::chrono::steady_clock::time_point::rep> last_profile_log_epoch{0};
    constexpr auto kProfileLogInterval = std::chrono::seconds(10);

    // Main event loop
    while (!stop_threads_) {
        auto now = std::chrono::steady_clock::now();
        auto now_rep = now.time_since_epoch().count();
        auto last_rep = last_profile_log_epoch.load(std::memory_order_relaxed);
        if (last_rep == 0 || (now - std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last_rep))) >= kProfileLogInterval) {
            if (last_profile_log_epoch.compare_exchange_strong(last_rep, now_rep, std::memory_order_relaxed)) {
                LogPublishPipelineProfile();
            }
        }

        // Check for new publish connections to register
        NewPublishConnection new_conn;
        while (publish_connection_queue_ && publish_connection_queue_->read(new_conn)) {
            // Create connection state (shared_ptr so PendingBatch keeps it alive after connection teardown)
            auto state = std::make_shared<ConnectionState>();
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

            // Add to connections map (shared_ptr so PendingBatch keeps state alive after teardown)
            int fd = new_conn.fd;
            connections[fd] = state;

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
                if (state->cxl_buf) {
                    metric_direct_cxl_connection_drop_with_buf_.fetch_add(1, std::memory_order_relaxed);
                    size_t leaked = static_cast<size_t>(state->batch_header.total_size);
                    metric_blog_leaked_bytes_.fetch_add(leaked, std::memory_order_relaxed);
                    uint64_t total_leaked = metric_blog_leaked_bytes_.load(std::memory_order_relaxed);
                    VLOG(1) << "PublishReceiveThread: fd=" << fd << " closed with cxl_buf set (BLog leak " << leaked << " bytes, total " << total_leaked << ")";
                    if (total_leaked >= (1ULL << 20) && total_leaked - leaked < (1ULL << 20)) {
                        LOG(WARNING) << "PublishReceiveThread: BLog leaked bytes above 1 MiB (total=" << total_leaked << ")";
                    }
                }
                state->cxl_buf = nullptr;
                if (state->staging_buf) staging_pool_->Release(state->staging_buf);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                connections.erase(it);
                continue;
            }

            size_t max_batch_size = max_batch_size_cached;
            if (staging_pool_) {
                size_t staging_cap = staging_pool_->GetBufferSize();
                if (staging_cap < max_batch_size) max_batch_size = staging_cap;
            }

            if (events[i].events & EPOLLIN) {
                switch (state->phase) {  // recv_direct_cached, max_batch_size from above
                    case WAIT_HEADER:
                        if (DrainHeader(fd, state)) {
                            // Validate total_size
                            if (state->batch_header.total_size == 0 ||
                                (staging_pool_ && state->batch_header.total_size > staging_pool_->GetBufferSize())) {
                                LOG(ERROR) << "PublishReceiveThread: Invalid batch total_size="
                                          << state->batch_header.total_size;
                                if (state->staging_buf) staging_pool_->Release(state->staging_buf);
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                                close(fd);
                                connections.erase(it);
                                continue;
                            }

                            if (recv_direct_cached) {
                                EnsureCachedTopicValid(state);
                                if (CheckPBRWatermark(state, pbr_high_watermark_pct)) {
                                    state->phase = WAIT_CXL;
                                    state->epoll_registered = false;
                                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                                    VLOG(2) << "PublishReceiveThread: PBR above " << pbr_high_watermark_pct
                                           << "%, pausing fd=" << fd << " (direct-CXL backpressure)";
                                } else {
                                // [[Issue #3]] Single epoch check per batch: do once before ReserveBLogSpace when using cached_topic
                                Topic* topic_ptr = state->cached_topic ? static_cast<Topic*>(state->cached_topic) : nullptr;
                                if (topic_ptr && topic_ptr->CheckEpochOnce()) {
                                    state->phase = WAIT_CXL;
                                    state->epoll_registered = false;
                                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                                    VLOG(2) << "PublishReceiveThread: Epoch stale, pausing fd=" << fd << " (direct-CXL)";
                                } else {
                                if (topic_ptr) state->epoch_checked_for_batch = true;
                                void* log = nullptr;
                                bool got_log;
                                {
                                    auto t0 = std::chrono::steady_clock::now();
                                    got_log = topic_ptr
                                        ? cxl_manager_->ReserveBLogSpace(topic_ptr, state->batch_header.total_size, log, true)
                                        : cxl_manager_->ReserveBLogSpace(state->handshake.topic, state->batch_header.total_size, log);
                                    auto t1 = std::chrono::steady_clock::now();
                                    RecordProfile(kReserveBLogSpace, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                                }
                                if (!got_log || !log) {
                                    state->epoch_checked_for_batch = false;
                                    state->phase = WAIT_CXL;
                                    state->epoll_registered = false;
                                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                                    VLOG(2) << "PublishReceiveThread: Ring full, pausing fd=" << fd << " (direct-CXL)";
                                } else {
                                    state->cxl_buf = log;
                                    state->phase = WAIT_PAYLOAD;
                                }
                                }
                                }
                            } else {
                                state->phase = WAIT_PAYLOAD;
                                state->staging_buf = staging_pool_->Allocate(state->batch_header.total_size);
                                if (!state->staging_buf) {
                                    metric_staging_exhausted_.fetch_add(1, std::memory_order_relaxed);
                                    VLOG(2) << "PublishReceiveThread: Staging pool exhausted, pausing fd=" << fd;
                                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                                    state->epoll_registered = false;
                                }
                            }
                        }
                        break;

                    case WAIT_PAYLOAD:
                        if (recv_direct_cached && state->cxl_buf) {
                            if (DrainPayloadToBuffer(fd, state, state->cxl_buf)) {
                                metric_batches_recv_direct_cxl_.fetch_add(1, std::memory_order_relaxed);
                                void* cxl_buf = state->cxl_buf;
                                BatchHeader batch_header = state->batch_header;
                                EnsureCachedTopicValid(state);
                                const char* topic = state->handshake.topic;
                                void* segment_header = nullptr;
                                size_t logical_offset = 0;
                                BatchHeader* batch_header_location = nullptr;
                                // Try PBR once; do not block. On failure, transition to WAIT_PBR_SLOT for recovery.
                                bool pbr_ok;
                                {
                                    auto t0 = std::chrono::steady_clock::now();
                                    pbr_ok = state->cached_topic
                                        ? cxl_manager_->ReservePBRSlotAndWriteEntry(static_cast<Topic*>(state->cached_topic), batch_header, cxl_buf,
                                                segment_header, logical_offset, batch_header_location, state->epoch_checked_for_batch)
                                        : cxl_manager_->ReservePBRSlotAndWriteEntry(topic, batch_header, cxl_buf,
                                                segment_header, logical_offset, batch_header_location);
                                    state->epoch_checked_for_batch = false;
                                    auto t1 = std::chrono::steady_clock::now();
                                    RecordProfile(kReservePBRSlotAndWriteEntry, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                                }
                                state->epoch_checked_for_batch = false;
                                if (!pbr_ok) {
                                    state->phase = WAIT_PBR_SLOT;
                                    state->pbr_retry_count = 1;
                                    break;
                                }
                                state->logical_offset = logical_offset;
                                state->batch_header_location = batch_header_location;
                                TInode* tinode = (TInode*)cxl_manager_->GetTInode(topic);
                                CompleteBatchInCXL(state->batch_header_location, batch_header.num_msg, tinode, state->logical_offset);
                                state->cxl_buf = nullptr;
                                state->batch_header_location = nullptr;
                                state->payload_offset = 0;
                                state->header_offset = 0;
                                state->phase = WAIT_HEADER;
                            }
                        } else if (state->staging_buf && DrainPayload(fd, state)) {
                            // Payload complete, enqueue for CXL allocation
                            metric_batches_drained_.fetch_add(1, std::memory_order_relaxed);

                            PendingBatch batch(it->second, state->staging_buf, state->batch_header, state->handshake);

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

                    case WAIT_PBR_SLOT:
                        // No EPOLLIN action; recovery loop retries PBR
                        break;

                    default:
                        LOG(ERROR) << "PublishReceiveThread: Invalid phase=" << state->phase
                                  << " for fd=" << fd;
                        break;
                }
            }
        }

        // Phase 3: Batched staging pool recovery (every N iterations)
        if (++recovery_check_counter >= kRecoveryCheckInterval) {
            CheckStagingPoolRecovery(epoll_fd, connections);
            if (recv_direct_cached) {
                for (auto& [recovery_fd, recovery_state] : connections) {
                    if (recovery_state->phase == WAIT_CXL && !recovery_state->epoll_registered) {
                        const int backoff_exp = std::min(recovery_state->wait_cxl_retry_count, 4);
                        const int backoff_interval = kRecoveryCheckInterval * (1 << backoff_exp);
                        if ((recovery_check_counter % backoff_interval) != 0) continue;
                        if (!recovery_state->cached_topic) recovery_state->cached_topic = cxl_manager_->GetTopicPtr(recovery_state->handshake.topic);
                        bool above_wm = recovery_state->cached_topic
                            ? cxl_manager_->IsPBRAboveHighWatermark(static_cast<Topic*>(recovery_state->cached_topic), pbr_high_watermark_pct)
                            : cxl_manager_->IsPBRAboveHighWatermark(recovery_state->handshake.topic, pbr_high_watermark_pct);
                        if (above_wm) continue;
                        void* log = nullptr;
                        bool got_log = recovery_state->cached_topic
                            ? cxl_manager_->ReserveBLogSpace(static_cast<Topic*>(recovery_state->cached_topic), recovery_state->batch_header.total_size, log)
                            : cxl_manager_->ReserveBLogSpace(recovery_state->handshake.topic, recovery_state->batch_header.total_size, log);
                        if (got_log && log) {
                            recovery_state->wait_cxl_retry_count = 0;
                            struct epoll_event ev;
                            ev.events = EPOLLIN | EPOLLET;
                            ev.data.fd = recovery_fd;
                            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, recovery_fd, &ev) == 0) {
                                recovery_state->cxl_buf = log;
                                recovery_state->phase = WAIT_PAYLOAD;
                                recovery_state->epoll_registered = true;
                                VLOG(2) << "PublishReceiveThread: Resumed fd=" << recovery_fd << " (direct-CXL)";
                            } else {
                                LOG(ERROR) << "PublishReceiveThread: epoll_ctl ADD failed fd=" << recovery_fd
                                           << " " << strerror(errno) << " (BLog reserved, will retry next cycle)";
                            }
                        } else {
                            recovery_state->wait_cxl_retry_count++;
                        }
                    }
                    // WAIT_PBR_SLOT: retry PBR without blocking; after MAX_PBR_RETRIES hand off to worker
                    if (recovery_state->phase == WAIT_PBR_SLOT && recovery_state->cxl_buf) {
                        void* segment_header = nullptr;
                        size_t logical_offset = 0;
                        BatchHeader* batch_header_location = nullptr;
                        EnsureCachedTopicValid(recovery_state.get());
                        const char* topic = recovery_state->handshake.topic;
                        bool pbr_ok = recovery_state->cached_topic
                            ? cxl_manager_->ReservePBRSlotAndWriteEntry(static_cast<Topic*>(recovery_state->cached_topic), recovery_state->batch_header,
                                    recovery_state->cxl_buf, segment_header, logical_offset, batch_header_location)
                            : cxl_manager_->ReservePBRSlotAndWriteEntry(topic, recovery_state->batch_header,
                                    recovery_state->cxl_buf, segment_header, logical_offset, batch_header_location);
                        if (pbr_ok) {
                            recovery_state->logical_offset = logical_offset;
                            recovery_state->batch_header_location = batch_header_location;
                            metric_batches_recv_direct_cxl_.fetch_add(1, std::memory_order_relaxed);
                            TInode* tinode = (TInode*)cxl_manager_->GetTInode(topic);
                            CompleteBatchInCXL(batch_header_location, recovery_state->batch_header.num_msg, tinode, logical_offset);
                            recovery_state->cxl_buf = nullptr;
                            recovery_state->batch_header_location = nullptr;
                            recovery_state->payload_offset = 0;
                            recovery_state->header_offset = 0;
                            recovery_state->phase = WAIT_HEADER;
                            recovery_state->pbr_retry_count = 0;
                        } else {
                            recovery_state->pbr_retry_count++;
                            if (recovery_state->pbr_retry_count >= kMaxPBRRetries) {
                                PendingBatch fallback(recovery_state, nullptr, recovery_state->batch_header, recovery_state->handshake);
                                fallback.staging_buf = nullptr;
                                fallback.cxl_buf_payload_already = recovery_state->cxl_buf;
                                if (!cxl_allocation_queue_->write(fallback)) {
                                    LOG(ERROR) << "PublishReceiveThread: PBR retries exhausted and queue full, fd=" << recovery_fd;
                                    metric_direct_cxl_pbr_fallback_dropped_.fetch_add(1, std::memory_order_relaxed);
                                }
                                recovery_state->cxl_buf = nullptr;
                                recovery_state->payload_offset = 0;
                                recovery_state->header_offset = 0;
                                recovery_state->phase = WAIT_HEADER;
                                recovery_state->pbr_retry_count = 0;
                            }
                        }
                    }
                }
            }
            recovery_check_counter = 0;
        }
    }

    // Cleanup
    for (auto& [fd, state] : connections) {
        if (state->cxl_buf) {
            metric_direct_cxl_connection_drop_with_buf_.fetch_add(1, std::memory_order_relaxed);
            size_t leaked = static_cast<size_t>(state->batch_header.total_size);
            metric_blog_leaked_bytes_.fetch_add(leaked, std::memory_order_relaxed);
        }
        state->cxl_buf = nullptr;
        if (state->staging_buf) staging_pool_->Release(state->staging_buf);
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

        // Allocate CXL buffer (or use existing when fallback set cxl_buf_payload_already)
        void* cxl_buf = nullptr;
        void* segment_header = nullptr;
        size_t logical_offset = 0;
        SequencerType seq_type;
        BatchHeader* batch_header_location = nullptr;
        std::function<void(void*, size_t)> callback;

        if (batch.cxl_buf_payload_already) {
            // [[CRITICAL FIX]] Fallback path: payload already in CXL at this address; only reserve PBR slot and complete.
            // Do NOT call GetCXLBuffer (would allocate new BLog and leak the existing buffer / corrupt subscriber data).
            cxl_buf = batch.cxl_buf_payload_already;
            const char* topic = batch.handshake.topic;
            if (batch.conn_state) EnsureCachedTopicValid(batch.conn_state.get());
            bool pbr_ok = (batch.conn_state && batch.conn_state->cached_topic)
                ? cxl_manager_->ReservePBRSlotAndWriteEntry(static_cast<Topic*>(batch.conn_state->cached_topic), batch.batch_header, cxl_buf,
                        segment_header, logical_offset, batch_header_location)
                : cxl_manager_->ReservePBRSlotAndWriteEntry(topic, batch.batch_header, cxl_buf,
                        segment_header, logical_offset, batch_header_location);
            if (!pbr_ok) {
                if (!cxl_allocation_queue_->write(batch)) {
                    metric_batches_dropped_.fetch_add(1, std::memory_order_relaxed);
                    LOG(ERROR) << "CXLAllocationWorker: Fallback batch PBR failed and re-enqueue full (batch_seq="
                              << batch.batch_header.batch_seq << ")";
                }
                continue;
            }
            TInode* tinode = (TInode*)cxl_manager_->GetTInode(topic);
            CompleteBatchInCXL(batch_header_location, batch.batch_header.num_msg, tinode, logical_offset);
            batch.conn_state->cxl_allocation_attempts = 0;
            if (batch.staging_buf) staging_pool_->Release(batch.staging_buf);
            batches_processed.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        {
            auto t0_get = std::chrono::steady_clock::now();
            callback = cxl_manager_->GetCXLBuffer(
                batch.batch_header,
                batch.handshake.topic,
                cxl_buf,
                segment_header,
                logical_offset,
                seq_type,
                batch_header_location);
            auto t1_get = std::chrono::steady_clock::now();
            RecordProfile(kGetCXLBuffer, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1_get - t0_get).count()));
        }

        if (!cxl_buf) {
            // CXL ring full, retry with exponential backoff
            if (batch.conn_state) batch.conn_state->cxl_allocation_attempts++;
            metric_cxl_retries_.fetch_add(1, std::memory_order_relaxed);
            metric_ring_full_.fetch_add(1, std::memory_order_relaxed);

            size_t ring_full_total = metric_ring_full_.load(std::memory_order_relaxed);
            if (ring_full_total <= 10 || ring_full_total % 1000 == 0) {
                LOG(WARNING) << "CXLAllocationWorker: Ring full, retry attempt "
                             << (batch.conn_state ? batch.conn_state->cxl_allocation_attempts : 0)
                             << " (batch_seq=" << batch.batch_header.batch_seq
                             << ", total_ring_full=" << ring_full_total << ")";
            }

            if (batch.conn_state && batch.conn_state->cxl_allocation_attempts > 10) {
                metric_batches_dropped_.fetch_add(1, std::memory_order_relaxed);
                size_t dropped = metric_batches_dropped_.load(std::memory_order_relaxed);
                LOG(ERROR) << "CXLAllocationWorker: Dropping batch after 10 retries "
                          << "(batch_seq=" << batch.batch_header.batch_seq
                          << ", total_dropped=" << dropped << ")";
                if (batch.staging_buf) staging_pool_->Release(batch.staging_buf);
                continue;
            }

            // Exponential backoff: 100µs, 200µs, 400µs, 800µs, 1600µs
            int backoff_exp = batch.conn_state ? std::min(batch.conn_state->cxl_allocation_attempts, 4) : 0;
            std::this_thread::sleep_for(std::chrono::microseconds(100 << backoff_exp));

            // Re-enqueue for retry
            if (!cxl_allocation_queue_->write(batch)) {
                metric_batches_dropped_.fetch_add(1, std::memory_order_relaxed);
                size_t dropped = metric_batches_dropped_.load(std::memory_order_relaxed);
                LOG(ERROR) << "CXLAllocationWorker: Failed to re-enqueue batch for retry "
                          << "(total_dropped=" << dropped << ")";
                if (batch.staging_buf) staging_pool_->Release(batch.staging_buf);
            }
            continue;
        }

        // Phase 3 Optimization: Pipelined copy and flush (skip when payload already in CXL)
        if (batch.staging_buf != nullptr) {
        auto t0_copy = std::chrono::steady_clock::now();
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
        auto t1_copy = std::chrono::steady_clock::now();
        RecordProfile(kCopyAndFlushPayload, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1_copy - t0_copy).count()));

        // Phase 3: Track successful copy
        metric_batches_copied_.fetch_add(1, std::memory_order_relaxed);
        }

        // Mark batch complete (critical for sequencer visibility)
        // [[CRITICAL FIX: OUT_OF_ORDER_BATCH_COMPLETION]]
        // Set num_msg BEFORE batch_complete=1. The ring slot was allocated with num_msg=0
        // to prevent scanner from seeing "in-flight" batches. Now that data copy is done,
        // we atomically make the batch visible by setting both fields and flushing.
        if (batch_header_location != nullptr) {
            // [[DIAGNOSTIC]] Verify num_msg is 0 in ring before we set it
            uint32_t prev_num_msg = batch_header_location->num_msg;
            if (prev_num_msg != 0) {
                static std::atomic<int> diag_count{0};
                if (++diag_count <= 10) {
                    LOG(WARNING) << "CXLAllocationWorker: Ring slot had num_msg=" << prev_num_msg
                                << " before completion (expected 0). batch_seq=" << batch.batch_header.batch_seq;
                }
            }
            // First set num_msg (scanner checks num_msg>0 as prerequisite)
            __atomic_store_n(&batch_header_location->num_msg, batch.batch_header.num_msg, __ATOMIC_RELEASE);
            // Then set batch_complete=1 (authoritative readiness signal)
            __atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE);
            CXL::flush_cacheline(batch_header_location);

            // Flush second cache line too (BatchHeader is 128 bytes / 2 cache lines)
            const void* batch_header_next_line = reinterpret_cast<const void*>(
                reinterpret_cast<const uint8_t*>(batch_header_location) + 64);
            CXL::flush_cacheline(batch_header_next_line);

            CXL::store_fence();

            // [[CRITICAL: ORDER=0 + ACK=1]] Copy-from-staging path must update written so GetOffsetToAck/AckThread send ACKs.
            // Blocking path and direct-CXL path use CompleteBatchInCXL (which calls UpdateWrittenForOrder0); this path did not.
            // See docs/PUBLISH_PIPELINE_EXPERT_ASSESSMENT.md §2.3.
            TInode* tinode = (TInode*)cxl_manager_->GetTInode(batch.handshake.topic);
            if (tinode && tinode->order == 0) {
                UpdateWrittenForOrder0(tinode, logical_offset, batch.batch_header.num_msg);
            }

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

        // Release staging buffer (null when payload was already in CXL)
        if (batch.staging_buf) staging_pool_->Release(batch.staging_buf);

        // Reset retry counter (conn_state may be null if connection was torn down)
        if (batch.conn_state) batch.conn_state->cxl_allocation_attempts = 0;

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
