#pragma once

#include "common.h"

class Subscriber;

// State for a single buffer within the dual-buffer setup
struct BufferState {
	void* buffer = nullptr;
	size_t capacity = 0;
	std::atomic<size_t> write_offset{0}; // Receiver updates this
																			 // Add other metadata if needed per buffer, e.g., message count start/end

	BufferState(size_t cap) : capacity(cap) {
		// Add MAP_POPULATE for potential performance benefit if system supports it well
		buffer = mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS /*| MAP_POPULATE*/, -1, 0);
		if (buffer == MAP_FAILED) {
			LOG(ERROR) << "BufferState: Failed to mmap buffer of size " << capacity << ": " << strerror(errno);
			buffer = nullptr;
			throw std::runtime_error("Failed to mmap buffer");
		}
	}

	~BufferState() {
		if (buffer != nullptr && buffer != MAP_FAILED) {
			munmap(buffer, capacity);
			buffer = nullptr;
		}
	}

	// Delete copy/move constructors/assignments
	BufferState(const BufferState&) = delete;
	BufferState& operator=(const BufferState&) = delete;
	BufferState(BufferState&&) = delete;
	BufferState& operator=(BufferState&&) = delete;
};

struct TimestampPair {
	long long send_time_nanos; // From message payload
	std::chrono::steady_clock::time_point receive_time; // Captured on receive
};

// Manages the dual buffers and state for a single connection (FD)
struct ConnectionBuffers : public std::enable_shared_from_this<ConnectionBuffers> {
	const int fd; // The socket FD this corresponds to
	const int broker_id;
	const size_t buffer_capacity;

	BufferState buffers[2]; // The two buffers

	std::atomic<int> current_write_idx{0}; // Index (0 or 1) of the buffer receiver is writing to
	std::atomic<bool> write_buffer_ready_for_consumer{false}; // Flag set by receiver when write buffer is full/ready
	std::atomic<bool> read_buffer_in_use_by_consumer{false};  // Flag set by consumer when it acquires read buffer

	absl::Mutex state_mutex; // Protects swapping, flag coordination, and waiting
	absl::CondVar consumer_can_consume_cv; // Notifies consumer a buffer *might* be ready
	absl::CondVar receiver_can_write_cv; // Notifies receiver the *other* buffer is free

	std::vector<std::pair<std::chrono::steady_clock::time_point, size_t>> recv_log ABSL_GUARDED_BY(state_mutex);


	ConnectionBuffers(int f, int b_id, size_t cap_per_buffer) :
		fd(f),
		broker_id(b_id),
		buffer_capacity(cap_per_buffer),
		buffers{BufferState(cap_per_buffer), BufferState(cap_per_buffer)} // Initialize buffers
	{
		//VLOG(2) << "ConnectionBuffers created for fd=" << fd << ", broker=" << broker_id;
	}

	~ConnectionBuffers() {
		//VLOG(2) << "ConnectionBuffers destroyed for fd=" << fd;
		// Buffers get unmapped by BufferState destructor
	}

	// Delete copy/move constructors/assignments
	ConnectionBuffers(const ConnectionBuffers&) = delete;
	ConnectionBuffers& operator=(const ConnectionBuffers&) = delete;
	ConnectionBuffers(ConnectionBuffers&&) = delete;
	ConnectionBuffers& operator=(ConnectionBuffers&&) = delete;

	// --- Helper methods ---

	// Called by Receiver: Get pointer and available space in the CURRENT write buffer
	std::pair<void*, size_t> get_write_location() {
		int write_idx = current_write_idx.load(std::memory_order_acquire);
		size_t current_offset = buffers[write_idx].write_offset.load(std::memory_order_relaxed);
		if (current_offset >= buffers[write_idx].capacity) {
			return {nullptr, 0}; // Buffer is full
		}
		return {static_cast<uint8_t*>(buffers[write_idx].buffer) + current_offset,
			buffers[write_idx].capacity - current_offset};
	}

	// Called by Receiver: Update write offset after successful recv()
	void advance_write_offset(size_t bytes_written) {
		//int write_idx = current_write_idx.load(std::memory_order_relaxed);
		int write_idx = current_write_idx;
		buffers[write_idx].write_offset.fetch_add(bytes_written, std::memory_order_relaxed);
	}

	// Called by Receiver: Signal that the current write buffer is ready and try to swap
	// Returns true if swap was successful, false otherwise (consumer still busy)
	bool signal_and_attempt_swap(Subscriber* subscriber_instance); // Implementation in .cc

	// Called by Consumer: Try to acquire the buffer ready for reading
	// Returns pointer to buffer state if acquired, nullptr otherwise.
	// Sets read_buffer_in_use_by_consumer = true on success.
	BufferState* acquire_read_buffer(); // Implementation in .cc

	// Called by Consumer: Release the buffer after processing
	// Resets read_buffer_in_use_by_consumer = false.
	// Notifies receiver thread.
	void release_read_buffer(BufferState* acquired_buffer); // Implementation in .cc

};

// Represents the data handed off to the consumer
struct ConsumedData {
	std::shared_ptr<ConnectionBuffers> connection; // Keep connection alive
	BufferState* buffer_state = nullptr; // The specific buffer being consumed
	size_t data_size = 0; // How much data is available in this buffer

	// Pointer to the start of consumable data
	const void* data() const {
		return buffer_state ? buffer_state->buffer : nullptr;
	}

	// Must be called when consumer is finished with this data block
	void release() {
		if (connection && buffer_state) {
			connection->release_read_buffer(buffer_state);
		}
		// Reset self
		connection = nullptr;
		buffer_state = nullptr;
		data_size = 0;
	}

	// Check if this holds valid data
	explicit operator bool() const {
		return connection && buffer_state && data_size > 0;
	}
};

/**
 * Subscriber class for receiving messages from the messaging system
 */
class Subscriber {
	public:
		// ... Constructor, destructor, other methods ...
		Subscriber(std::string head_addr, std::string port, char topic[TOPIC_NAME_SIZE], bool measure_latency=false, int order_level=0);
		~Subscriber(); // Important to manage shutdown and cleanup

		// The method the application calls to get data
		void* Consume(int timeout_ms = 1000);
		void* ConsumeBatchAware(int timeout_ms = 1000);

		// Called by client code after test is finished
		void StoreLatency();
		void DEBUG_wait(size_t total_msg_size, size_t msg_size);
		bool DEBUG_check_order(int order);
		/**
		 * Debug method to wait for a certain amount of data
		 * @param total_msg_size Total size of all messages
		 * @param msg_size Size of each message
		 */
		void Poll(size_t total_msg_size, size_t msg_size);


		// Initiate shutdown
		void Shutdown();

		// --- Buffer Management (part 1/2) ---
		// It is here for DKVS
		absl::Mutex connection_map_mutex_; // Protects the map itself
		absl::flat_hash_map<int, std::shared_ptr<ConnectionBuffers>> connections_ ABSL_GUARDED_BY(connection_map_mutex_);

		void WaitUntilAllConnected(){
			size_t num_connections = 0;
			// Use runtime-configured broker count to avoid hanging when fewer than NUM_MAX_BROKERS are used
			size_t expected = NUM_MAX_BROKERS_CONFIG * NUM_SUB_CONNECTIONS;
			LOG(INFO) << "Waiting for " << expected << " connections (brokers=" << NUM_MAX_BROKERS_CONFIG 
				<< ", sub_connections=" << NUM_SUB_CONNECTIONS << ")";
			
			auto start_time = std::chrono::steady_clock::now();
			while (num_connections < expected) {
				{
					absl::ReaderMutexLock map_lock(&connection_map_mutex_);
					num_connections = connections_.size();
				}
				if(num_connections < expected){
					auto now = std::chrono::steady_clock::now();
					auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
					if (elapsed.count() > 30) { // 30 second timeout
						LOG(WARNING) << "Timeout waiting for connections. Got " << num_connections 
							<< "/" << expected << " after " << elapsed.count() << " seconds";
						break;
					}
					if (elapsed.count() % 5 == 0 && elapsed.count() > 0) {
						LOG(INFO) << "Still waiting for connections: " << num_connections << "/" << expected;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}else{
					break;
				}
			}
			LOG(INFO) << "Connection wait complete: " << num_connections << "/" << expected;
		}

	private:
		friend class ConnectionBuffers; // Allow access to members if needed

		// --- Connection & Thread Management ---
		std::string head_addr_;
		std::string port_;
		std::unique_ptr<heartbeat_system::HeartBeat::Stub> stub_;
		char topic_[TOPIC_NAME_SIZE];
		std::atomic<bool> shutdown_{false};
		std::atomic<bool> connected_{false}; // Maybe more granular connection state needed

		// --- Latency / Debug ---
		bool measure_latency_;
		int order_level_; // Store the order level for batch-aware processing
		std::atomic<size_t> DEBUG_count_{0}; // Total bytes received across all connections

		// --- Buffer Management (part 2/2)---
		const size_t buffer_size_per_buffer_; // Size for *each* of the two buffers per connection
		absl::CondVar consume_cv_; // Global CV for consumer to wait on

		int client_id_;

		// Cluster state
		absl::Mutex node_mutex_;
		absl::flat_hash_map<int, std::string> nodes_ ABSL_GUARDED_BY(node_mutex_);
		std::thread cluster_probe_thread_;

		// Worker thread management
		struct ThreadInfo {
			std::thread thread;
			int fd; // Associated FD for cleanup

			// Need custom move constructor/assignment if std::thread is directly included
			ThreadInfo(std::thread t, int f) : thread(std::move(t)), fd(f) {}
			ThreadInfo(ThreadInfo&& other) noexcept : thread(std::move(other.thread)), fd(other.fd) {}
			ThreadInfo& operator=(ThreadInfo&& other) noexcept {
				if (this != &other) {
					if(thread.joinable()) thread.join(); // Join old thread if assigned over
					thread = std::move(other.thread);
					fd = other.fd;
				}
				return *this;
			}
			// Ensure thread is joined on destruction
			~ThreadInfo() {
				if (thread.joinable()) {
					// Avoid logging from destructor if possible or make it thread-safe
					// VLOG(5) << "Joining thread for fd " << fd;
					thread.join();
				}
			}
			// Delete copy operations
			ThreadInfo(const ThreadInfo&) = delete;
			ThreadInfo& operator=(const ThreadInfo&) = delete;
		};
		absl::Mutex worker_mutex_; // Protects worker_threads_ vector
		std::vector<ThreadInfo> worker_threads_ ABSL_GUARDED_BY(worker_mutex_);




		// --- Private Methods ---
		void SubscribeToClusterStatus(); // Runs in cluster_probe_thread_
		void ManageBrokerConnections(int broker_id, const std::string& address); // Launches workers
																																						 // Worker thread function (needs access to Subscriber instance)
		void ReceiveWorkerThread(int broker_id, int fd_to_handle);

		// Helper to remove connection resources
		void RemoveConnection(int fd);
};
