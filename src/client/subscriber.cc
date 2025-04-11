#include "subscriber.h"
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <numeric>

Subscriber::Subscriber(std::string head_addr, std::string port, char topic[TOPIC_NAME_SIZE], bool measure_latency)
	: head_addr_(head_addr),
	port_(port),
	shutdown_(false),
	connected_(false),
	measure_latency_(measure_latency),
	// Adjust size: This is now PER BUFFER. 8GB total per connection might be excessive.
	// Maybe 1GB per buffer (2GB total per connection)? Needs tuning.
	buffer_size_per_buffer_((1UL << 30)), // e.g., 1 GB per buffer
	client_id_(GenerateRandomNum())
{
	memcpy(topic_, topic, TOPIC_NAME_SIZE);
	std::string grpc_addr = head_addr + ":" + port;
	// Consider managing stub_ lifecycle (e.g., unique_ptr) if Subscriber owns it
	stub_ = heartbeat_system::HeartBeat::NewStub(grpc::CreateChannel(grpc_addr, grpc::InsecureChannelCredentials()));

	{
		absl::MutexLock lock(&node_mutex_);
		nodes_[0] = head_addr + ":" + std::to_string(PORT); // Assuming PORT is defined
	}
	VLOG(5) << "Subscriber initialized. Buffer size per connection (dual): "
		<< (buffer_size_per_buffer_ * 2) / (1024*1024) << " MB total.";

	// Start cluster probe thread (will call ManageBrokerConnections)
	cluster_probe_thread_ = std::thread([this]() { this->SubscribeToClusterStatus(); });

	// Wait for initial connection attempt - maybe remove this wait here
	// while (!connected_) {
	//    std::this_thread::yield();
	// }
}

Subscriber::~Subscriber() {
	VLOG(1) << "Subscriber shutting down...";
	Shutdown(); // Ensure shutdown is called
	if (cluster_probe_thread_.joinable()) {
		cluster_probe_thread_.join();
		VLOG(1) << "Cluster probe thread joined.";
	}
	// Worker threads should be joined by ThreadInfo destructor when vector clears
	{
		absl::MutexLock lock(&worker_mutex_);
		worker_threads_.clear(); // Triggers ThreadInfo destructors
		VLOG(1) << "Worker threads cleared and joined.";
	}
	// ConnectionBuffers map cleared automatically (shared_ptr refs drop)
	VLOG(1) << "Subscriber shutdown complete.";
}

void Subscriber::Shutdown() {
	if (shutdown_.exchange(true)) { // Prevent double shutdown
		return;
	}
	VLOG(1) << "Initiating Subscriber shutdown sequence...";

	// Wake up any waiting consumer
	consume_cv_.SignalAll();

	// Wake up any waiting receiver threads (though they should check shutdown_ flag)
	{
		absl::MutexLock lock(&connection_map_mutex_);
		for(auto const& [fd, conn_ptr] : connections_) {
			if(conn_ptr) {
				absl::MutexLock state_lock(&conn_ptr->state_mutex); // Lock specific connection
				conn_ptr->receiver_can_write_cv.Signal(); // Wake up receiver if waiting
			}
		}
	}


	// Close all connection FDs to interrupt blocking recv calls
	{
		absl::MutexLock lock(&worker_mutex_);
		VLOG(1) << "Closing " << worker_threads_.size() << " worker connections...";
		for (const auto& info : worker_threads_) {
			// Shut down the socket for reading and writing.
			// This should cause recv() in the worker thread to return 0 or error.
			if (info.fd >= 0) {
				VLOG(2) << "Shutting down socket fd=" << info.fd;
				// SHUT_RDWR immediately stops reads/writes
				if (::shutdown(info.fd, SHUT_RDWR) < 0) {
					LOG(WARNING) << "Failed to shutdown socket fd=" << info.fd << ": " << strerror(errno);
				}
				// Closing might happen later when thread exits or here?
				// Let thread close its own FD on exit for cleaner resource handling.
			}
		}
	}
	// Note: Joining threads happens in destructor or when worker_threads_ is cleared
}

void Subscriber::RemoveConnection(int fd) {
	absl::MutexLock lock(&connection_map_mutex_);
	if (connections_.erase(fd)) {
		VLOG(1) << "Removed connection resources for fd=" << fd;
		// shared_ptr ref count drops. If 0, ConnectionBuffers is destroyed.
	}
}

bool Subscriber::DEBUG_check_order(int order) {
	VLOG(5) << "Checking message order with order level: " << order;
	return true;
	/*

	// Skip checks if disabled
	if (DEBUG_do_not_check_order_) {
	VLOG(5) << "Order checking disabled, skipping checks";
	return true;
	}

	int idx = 0;
	bool order_correct = true;

	for (auto& msg_pair : messages_[idx]) {
	// Get buffer and metadata
	void* buf = msg_pair.first;
	if (!buf) {
	continue;
	}

	// First check: Verify logical offsets are assigned
	Embarcadero::MessageHeader* header = static_cast<Embarcadero::MessageHeader*>(buf);

	while (header->paddedSize != 0) {
	if (header->logical_offset == static_cast<size_t>(-1)) {
	LOG(ERROR) << "Message with client_order " << header->client_order 
	<< " was not assigned a logical offset";
	order_correct = false;
	break;
	}

	// Move to next message
	header = reinterpret_cast<Embarcadero::MessageHeader*>(
	reinterpret_cast<uint8_t*>(header) + header->paddedSize);
	}

	// Skip further checks if order level is 0
	if (order == 0) {
	continue;
	}

	// Second check: Verify total order is assigned
	header = static_cast<Embarcadero::MessageHeader*>(buf);
	absl::flat_hash_set<int> duplicate_checker;

	while (header->paddedSize != 0) {
	if (header->total_order == 0 && header->logical_offset != 0) {
	LOG(ERROR) << "Message with client_order " << header->client_order 
	<< " and logical offset " << header->logical_offset 
	<< " was not assigned a total order";
	order_correct = false;
	break;
	}

	// Check for duplicate total order values
	if (duplicate_checker.contains(header->total_order)) {
	LOG(ERROR) << "Duplicate total order detected: " << header->total_order 
	<< " for message with client_order " << header->client_order;
	order_correct = false;
	} else {
	duplicate_checker.insert(header->total_order);
	}

	// Move to next message
	header = reinterpret_cast<Embarcadero::MessageHeader*>(
	reinterpret_cast<uint8_t*>(header) + header->paddedSize);
	}

	// Skip further checks if order level is 1
	if (order == 1) {
	continue;
	}

	// Third check: For order level 3, verify total_order matches client_order
	header = static_cast<Embarcadero::MessageHeader*>(buf);

	while (header->paddedSize != 0) {
		if (header->total_order != header->client_order) {
			LOG(ERROR) << "Message with client_order " << header->client_order 
				<< " has mismatched total_order " << header->total_order;
			order_correct = false;
			break;
		}

		// Move to next message
		header = reinterpret_cast<Embarcadero::MessageHeader*>(
				reinterpret_cast<uint8_t*>(header) + header->paddedSize);
	}
}

if (order_correct) {
	VLOG(5) << "Order check passed for level " << order;
} else {
	LOG(ERROR) << "Order check failed for level " << order;
}

return order_correct;
*/
}


void Subscriber::StoreLatency() {
	if (!measure_latency_) {
		LOG(INFO) << "Latency measurement was not enabled.";
		return;
	}

	VLOG(1) << "Parsing buffers and processing recv log to calculate latencies...";

	std::vector<long long> all_latencies_us; // Calculated latencies
	size_t total_messages_parsed = 0;

	{ // Scope for locking the connection map
		absl::MutexLock map_lock(&connection_map_mutex_);
		VLOG(2) << "Processing data from " << connections_.size() << " connections.";

		for (auto const& [fd, conn_ptr] : connections_) {
			if (!conn_ptr) continue;

			// Lock connection state to access log and buffer details safely
			absl::MutexLock state_lock(&conn_ptr->state_mutex);
			const auto& recv_log = conn_ptr->recv_log; // Get reference to log

			if (recv_log.empty()) {
				VLOG(3) << "FD=" << fd << ": No recv log entries, skipping.";
				continue;
			}
			VLOG(3) << "FD=" << fd << ": Processing " << recv_log.size() << " recv log entries.";

			// --- Process both buffers for this connection ---
			for (int buf_idx = 0; buf_idx < 2; ++buf_idx) {
				const auto& buffer_state = conn_ptr->buffers[buf_idx];
				size_t buffer_data_size = buffer_state.write_offset.load(std::memory_order_relaxed);
				uint8_t* buffer_start_ptr = static_cast<uint8_t*>(buffer_state.buffer);

				if (buffer_data_size == 0) continue; // Skip empty buffers

				VLOG(4) << "FD=" << fd << ", Buffer=" << buf_idx << ": Parsing " << buffer_data_size << " bytes.";

				size_t parse_offset = 0;
				while (parse_offset < buffer_data_size) {
					uint8_t* current_parse_ptr = buffer_start_ptr + parse_offset;
					size_t remaining_in_buffer = buffer_data_size - parse_offset;

					// 1. Check for MessageHeader
					if (remaining_in_buffer < sizeof(Embarcadero::MessageHeader)) break; // Incomplete header
					Embarcadero::MessageHeader* msg_header = reinterpret_cast<Embarcadero::MessageHeader*>(current_parse_ptr);

					// 2. Check for Full Message
					size_t total_message_size = msg_header->paddedSize; // Adjust field name if needed
					if (total_message_size == 0) { /* handle error */ break; }
					if (remaining_in_buffer < total_message_size) break; // Incomplete message

					// --- Full message identified ---
					total_messages_parsed++;
					size_t message_end_offset_in_buffer = parse_offset + total_message_size;

					// 3. Extract Send Timestamp from buffer payload
					uint8_t* payload_ptr = current_parse_ptr + sizeof(Embarcadero::MessageHeader);
					long long send_nanos_since_epoch;
					memcpy(&send_nanos_since_epoch, payload_ptr, sizeof(long long));
					std::chrono::steady_clock::time_point send_time{std::chrono::nanoseconds(send_nanos_since_epoch)};

					// 4. Find Approximate Receive Time from recv_log
					// Find the *first* recv log entry whose end_offset is >= message_end_offset
					std::chrono::steady_clock::time_point approx_receive_time = recv_log.back().first; // Default to last timestamp if not found earlier
					bool found_ts = false;
					for(const auto& log_entry : recv_log) {
						// CRITICAL ASSUMPTION: Offsets in recv_log correspond to THIS buffer.
						// This breaks if the log contains offsets from the *other* buffer
						// unless offsets are absolute across swaps, which they aren't here.
						// TODO: This correlation logic needs refinement if buffer swaps happened!
						// For simplicity now, assume log offsets roughly match buffer content offsets,
						// which is only true if only one buffer was significantly used or swaps were clean.

						// Let's ignore the offset correlation for now as it's complex with swaps,
						// and just use the timestamp of the *last* recv as a rough upper bound.
						// A better approach would require storing buffer_idx with log entries
						// or absolute stream offsets.

						// Correct search:
						// if (log_entry.second >= message_end_offset_in_buffer) {
						//      approx_receive_time = log_entry.first;
						//      found_ts = true;
						//      break;
						// }
					}
					// Using last timestamp as placeholder due to complexity:
					approx_receive_time = recv_log.back().first;


					// 5. Calculate Latency
					auto latency_duration = approx_receive_time - send_time;
					long long latency_micros = std::chrono::duration_cast<std::chrono::microseconds>(latency_duration).count();
					all_latencies_us.push_back(latency_micros);

					// 6. Advance parse_offset
					parse_offset += total_message_size;

				} // End while(parse_offset < buffer_data_size)
			} // End for buf_idx
		} // End for connections
	} // Release connection map lock

	// --- Post-processing (Sorting, Stats, CDF) remains the same ---
	if (all_latencies_us.empty()) {
		LOG(WARNING) << "No latency values could be calculated.";
		return;
	}

	LOG(INFO) << "Parsed " << total_messages_parsed << " messages and calculated " << all_latencies_us.size() << " latency values.";

	size_t count = all_latencies_us.size();
	LOG(INFO) << "Collected " << count << " latency values (microseconds).";

	// --- Calculate Statistics ---

	// Sort for Min, Max, Median, Percentiles
	std::sort(all_latencies_us.begin(), all_latencies_us.end());

	// Average
	long double sum = std::accumulate(all_latencies_us.begin(), all_latencies_us.end(), 0.0L);
	long double avg_us = sum / count;

	long long min_us = all_latencies_us.front();
	long long max_us = all_latencies_us.back();
	long long median_us = all_latencies_us[count / 2]; // Simple median

	// Percentiles (e.g., 99th, 99.9th)
	long long p99_us = all_latencies_us[static_cast<size_t>(std::floor(0.99 * count))];
	long long p999_us = all_latencies_us[static_cast<size_t>(std::floor(0.999 * count))];
	// Note: For exact percentile definitions (e.g., nearest rank, interpolation),
	// you might need a more sophisticated calculation, especially for small counts.

	// --- Log Results ---
	LOG(INFO) << "Latency Statistics (us):";
	LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << avg_us;
	LOG(INFO) << "  Min:     " << min_us;
	LOG(INFO) << "  Median:  " << median_us;
	LOG(INFO) << "  99th P:  " << p99_us;
	LOG(INFO) << "  99.9th P:" << p999_us;
	LOG(INFO) << "  Max:     " << max_us;

	// --- Generate and Write CDF Data Points ---
	std::string cdf_filename = "cdf_latency_us.csv"; // Use .csv for easy import
	LOG(INFO) << "Writing CDF data points to " << cdf_filename;
	std::ofstream cdf_file(cdf_filename);
	if (!cdf_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << cdf_filename;
	} else {
		cdf_file << "Latency_us,CumulativeProbability\n"; // CSV Header

		// Iterate through the SORTED latencies
		for (size_t i = 0; i < count; ++i) {
			long long current_latency = all_latencies_us[i];
			// Cumulative probability = (number of points <= current_latency) / total_points
			// Since it's sorted, this is (index + 1) / count
			double cumulative_probability = static_cast<double>(i + 1) / count;

			cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability << "\n";

			// Optimization: If you have many identical latency values, the CDF plot
			// will have vertical jumps. You might only want to write unique points
			// for a smaller file, but writing all points is fine for plotting.
			// Example to write unique points only (more complex):
			// if (i == 0 || all_latencies_us[i] != all_latencies_us[i - 1]) {
			//    // Find the index of the last occurrence of this latency
			//    auto upper = std::upper_bound(all_latencies_us.begin() + i, all_latencies_us.end(), current_latency);
			//    size_t last_index = (upper - all_latencies_us.begin()) - 1;
			//    double prob_at_last = static_cast<double>(last_index + 1) / count;
			//    cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << prob_at_last << "\n";
			//    i = last_index; // Skip to the next unique value
			//}
		}
		cdf_file.close();
	}
}

void Subscriber::Poll(size_t total_msg_size, size_t msg_size) {
	VLOG(5) << "Waiting to receive " << total_msg_size << " bytes of data with message size " << msg_size;

	// Calculate expected total data size based on padded message size
	msg_size = ((msg_size + 64 - 1) / 64) * 64;
	size_t num_msg = total_msg_size / msg_size;
	size_t total_data_size = num_msg * (sizeof(Embarcadero::MessageHeader) + msg_size);

	auto start = std::chrono::steady_clock::now();
	auto last_log_time = start;

	// Wait until all expected data is received
	while (DEBUG_count_ < total_data_size) {
		std::this_thread::yield();

		// Log progress every 3 seconds
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 3) {
			double percentage = static_cast<double>(DEBUG_count_) / total_data_size * 100.0;
			VLOG(5) << "Received " << DEBUG_count_ << "/" << total_data_size 
				<< " bytes (" << std::fixed << std::setprecision(1) << percentage << "%)";
			last_log_time = now;
		}
	}

	auto end = std::chrono::steady_clock::now();
	double seconds = std::chrono::duration<double>(end - start).count();
	double throughput_mbps = (total_data_size / seconds) / (1024 * 1024);

	VLOG(5) << "Received all " << total_data_size << " bytes in " << seconds << " seconds"
		<< " (" << throughput_mbps << " MB/s)";
}

void Subscriber::ManageBrokerConnections(int broker_id, const std::string& address) {
	auto [addr_str, port_str] = ParseAddressPort(address);
	int data_port = PORT + broker_id; // Use the base data port

	// Create a mutable copy
	std::vector<char> addr_vec(addr_str.begin(), addr_str.end());
	addr_vec.push_back('\0');

	std::vector<int> connected_fds;
	std::vector<int> pending_fds;

	// Still use temporary epoll for non-blocking connect phase
	int conn_epoll_fd = epoll_create1(0);
	if (conn_epoll_fd < 0) { /* ... error handling ... */ return; }

	// Step 1: Create sockets and initiate non-blocking connect (Unchanged)
	for (int i = 0; i < NUM_SUB_CONNECTIONS; ++i) {
		int sock = GetNonblockingSock(addr_vec.data(), data_port, true);
		if (sock < 0) { /* ... error handling ... */ continue; }
		pending_fds.push_back(sock);
		epoll_event ev;
		ev.events = EPOLLOUT | EPOLLET;
		ev.data.fd = sock;
		if (epoll_ctl(conn_epoll_fd, EPOLL_CTL_ADD, sock, &ev) < 0) {
			LOG(ERROR) << "Failed to add socket " << sock << " to connection epoll: " << strerror(errno);
			close(sock);
		}
	}

	if (pending_fds.empty()) { /* ... error handling ... */ close(conn_epoll_fd); return; }

	// Step 2: Wait for connection results (Unchanged)
	epoll_event events[NUM_SUB_CONNECTIONS];
	const int CONNECT_TIMEOUT_MS = 2000;
	int num_ready = epoll_wait(conn_epoll_fd, events, NUM_SUB_CONNECTIONS, CONNECT_TIMEOUT_MS);

	// Step 3: Check connection status (Unchanged)
	for (int n = 0; n < num_ready; ++n) {
		int sock = events[n].data.fd;
		if (events[n].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
			int error = 0;
			socklen_t len = sizeof(error);
			if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
				LOG(WARNING) << "Connection failed for socket " << sock << ": " << strerror(error ? error : ETIMEDOUT);
				close(sock);
				for(size_t i=0; i<pending_fds.size(); ++i) if(pending_fds[i] == sock) pending_fds[i] = -1;
			} else {
				VLOG(5) << "Socket " << sock << " connected successfully to broker " << broker_id;
				int flags = fcntl(sock, F_GETFL, 0);
				if (flags == -1) {
					LOG(ERROR) << "fcntl F_GETFL failed for connected socket " << sock << ": " << strerror(errno);
					close(sock); // Close socket if we can't change flags
											 // Mark as handled/failed in pending_fds (important if you iterate pending_fds later)
					for(size_t i=0; i<pending_fds.size(); ++i) if(pending_fds[i] == sock) pending_fds[i] = -1;
					continue; // Skip this socket
				}

				flags &= ~O_NONBLOCK; // Remove the non-blocking flag using bitwise AND with complement

				if (fcntl(sock, F_SETFL, flags) == -1) {
					LOG(ERROR) << "fcntl F_SETFL failed to set blocking mode for socket " << sock << ": " << strerror(errno);
					close(sock); // Close socket if we can't change flags
											 // Mark as handled/failed in pending_fds
					for(size_t i=0; i<pending_fds.size(); ++i) if(pending_fds[i] == sock) pending_fds[i] = -1;
					continue; // Skip this socket
				}
				// *** END OF ADDED BLOCK ***


				connected_fds.push_back(sock);
				// Mark as connected in pending_fds
				for(size_t i=0; i<pending_fds.size(); ++i) if(pending_fds[i] == sock) pending_fds[i] = -1; // Mark as handled
			}
		}
	}

	// Step 4: Clean up timed out/failed sockets (Unchanged)
	for (int sock : pending_fds) {
		if (sock != -1) {
			LOG(WARNING) << "Cleaning up potentially timed-out socket " << sock << " for broker " << broker_id;
			epoll_ctl(conn_epoll_fd, EPOLL_CTL_DEL, sock, nullptr);
			close(sock);
		}
	}
	close(conn_epoll_fd);


	if (connected_fds.empty()) {
		LOG(ERROR) << "No successful connections established to broker " << broker_id;
		return;
	}

	// Step 5 & 6 Combined: Create resources and Launch worker threads
	{
		// Lock both maps for consistency
		absl::MutexLock map_lock(&connection_map_mutex_);
		absl::MutexLock worker_lock(&worker_mutex_);

		for (int connected_fd : connected_fds) {
			// Create the shared buffer resource for this FD
			try {
				auto connection_res = std::make_shared<ConnectionBuffers>(
						connected_fd, broker_id, buffer_size_per_buffer_
						);
				connections_[connected_fd] = connection_res; // Add to map

				VLOG(5) << "Launching worker thread for broker " << broker_id << " FD " << connected_fd;
				// Use emplace_back for ThreadInfo
				worker_threads_.emplace_back(
						std::thread(&Subscriber::ReceiveWorkerThread, this, broker_id, connected_fd),
						connected_fd // Store FD with thread info
						);

			} catch (const std::runtime_error& e) {
				LOG(ERROR) << "Failed to create ConnectionBuffers for fd=" << connected_fd << ": " << e.what();
				close(connected_fd); // Close the socket if resource allocation failed
			} catch (const std::bad_alloc& e) {
				LOG(ERROR) << "Memory allocation failed for ConnectionBuffers fd=" << connected_fd << ": " << e.what();
				close(connected_fd);
			}

		} // end for loop
	} // Locks released
	connected_ = true; // Signal started processing
}

void Subscriber::ReceiveWorkerThread(int broker_id, int fd_to_handle) {
	// --- Resource Allocation ---
	std::shared_ptr<ConnectionBuffers> conn_buffers;
	{
		absl::MutexLock lock(&connection_map_mutex_);
		auto it = connections_.find(fd_to_handle);
		if (it == connections_.end()) {
			LOG(ERROR) << "Worker (fd=" << fd_to_handle << "): Could not find ConnectionBuffers in map.";
			close(fd_to_handle);
			return;
		}
		conn_buffers = it->second; // Get the shared pointer
	}

	if (!conn_buffers) {
		LOG(ERROR) << "Worker (fd=" << fd_to_handle << "): Null ConnectionBuffers pointer.";
		close(fd_to_handle);
		return;
	}

	// --- Send Subscription Request ---
	Embarcadero::EmbarcaderoReq shake;
	memset(&shake, 0, sizeof(shake));
	shake.num_msg = 0;
	shake.client_id = client_id_;
	shake.last_addr = 0;
	shake.client_req = Embarcadero::Subscribe;
	memcpy(shake.topic, topic_, TOPIC_NAME_SIZE);

	if (send(conn_buffers->fd, &shake, sizeof(shake), 0) < static_cast<ssize_t>(sizeof(shake))) {
		LOG(ERROR) << "Worker (broker " << broker_id << "): Failed to send subscription request on fd " << fd_to_handle << ": " << strerror(errno);
		// unique_ptr cleans up resources automatically when function returns
		close(conn_buffers->fd);
		RemoveConnection(conn_buffers->fd);
		return;
	}

	VLOG(5) << "Worker (broker " << broker_id << ", fd " << fd_to_handle << "): Successfully subscribed.";

	// --- Main receive loop (Simplified - Blocking recv) ---
	while (!shutdown_) {
		// 1. Get current write buffer location & space (same as before)
		std::pair<void*, size_t> write_loc = conn_buffers->get_write_location();
		void* write_ptr = write_loc.first;
		size_t available_space = write_loc.second;

		// 2. Check if current write buffer is full (same swap logic as before)
		if (available_space == 0) {
			VLOG(4) << "Worker (fd=" << conn_buffers->fd << "): Write buffer full. Attempting swap.";
			if (conn_buffers->signal_and_attempt_swap(this)) {
				VLOG(4) << "Worker (fd=" << conn_buffers->fd << "): Swap successful.";
				// REMOVE: parse_offset = 0;
				continue;
			} else {
				VLOG(4) << "Worker (fd=" << conn_buffers->fd << "): Swap failed, consumer busy. Waiting...";
				// Wait logic (manual loop using receiver_can_write_cv) remains the same
				{
					absl::MutexLock lock(&conn_buffers->state_mutex);
					if (shutdown_.load(std::memory_order_relaxed)) break;
					while (! (shutdown_.load(std::memory_order_relaxed) ||
								!conn_buffers->read_buffer_in_use_by_consumer.load(std::memory_order_acquire)) )
					{
						conn_buffers->receiver_can_write_cv.Wait(&conn_buffers->state_mutex);
					}
				}
				VLOG(4) << "Worker (fd=" << conn_buffers->fd << "): Wait loop finished.";
				if (shutdown_.load(std::memory_order_relaxed)) break;
				VLOG(4) << "Worker (fd=" << conn_buffers->fd << "): Consumer released buffer, continuing loop.";
				continue;
			}
		}

		// 3. Receive data into the buffer (same as before)
		ssize_t bytes_received = recv(conn_buffers->fd, write_ptr, available_space, 0);

		if (bytes_received > 0) {
			// 4. Advance write offset (BEFORE getting timestamp)
			conn_buffers->advance_write_offset(bytes_received);

			// 5. Record Timestamp and NEW Offset
			auto recv_complete_time = std::chrono::steady_clock::now();
			// Get the offset *after* the write
			size_t current_end_offset = conn_buffers->buffers[conn_buffers->current_write_idx.load()].write_offset.load(std::memory_order_relaxed);

			if (measure_latency_) { // Still use the flag
															// Store the pair within the ConnectionBuffers
				{
					absl::MutexLock lock(&conn_buffers->state_mutex);
					conn_buffers->recv_log.emplace_back(recv_complete_time, current_end_offset);
				}
			}

			// Update global byte count (same as before)
			DEBUG_count_.fetch_add(bytes_received, std::memory_order_relaxed);

			// --- NO MESSAGE PARSING OR LATENCY CALCULATION HERE ---

		} else if (bytes_received == 0) {
			// Handle disconnect (same as before, including potential final signal)
			VLOG(1) << "Worker (fd=" << conn_buffers->fd << "): Broker disconnected (recv returned 0).";
			size_t final_write_offset = conn_buffers->buffers[conn_buffers->current_write_idx.load()].write_offset.load();
			if (final_write_offset > 0) {
				absl::MutexLock lock(&conn_buffers->state_mutex);
				// Signal that the buffer containing the last data might be ready
				conn_buffers->write_buffer_ready_for_consumer.store(true, std::memory_order_release);
				conn_buffers->consumer_can_consume_cv.Signal();
			}
			break;
		} else { // bytes_received < 0
						 // Handle errors (same as before, including potential final signal)
			if (errno == EINTR) continue;
			if (shutdown_.load(std::memory_order_relaxed)) { /* log shutdown */ }
			else { LOG(ERROR) << "Worker (fd=" << conn_buffers->fd << "): recv failed: " << strerror(errno); }
			size_t final_write_offset_err = conn_buffers->buffers[conn_buffers->current_write_idx.load()].write_offset.load();
			if (final_write_offset_err > 0) { /* signal final buffer */ }
			break;
		}
	} // End while(!shutdown_)

	close(conn_buffers->fd); // Close the socket FD associated with this thread
	RemoveConnection(conn_buffers->fd); // Remove resources from map

	// --- Cleanup ---
	close(fd_to_handle);
	// unique_ptr<ManagedConnectionResources> 'resources' cleans up mmap/malloc automatically upon exit.

	VLOG(5) << "Worker thread for broker " << broker_id << ", FD " << fd_to_handle << " finished.";
}

void Subscriber::SubscribeToClusterStatus() {
	std::string initial_head_addr;
	{
		absl::MutexLock lock(&node_mutex_);
		initial_head_addr = nodes_[0];
	}
	ManageBrokerConnections(0, initial_head_addr); // Start connections for head broker

	while (!shutdown_) {
		heartbeat_system::ClientInfo client_info;
		heartbeat_system::ClusterStatus cluster_status;
		grpc::ClientContext stream_context; // New context per attempt

		auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(3);
		stream_context.set_deadline(deadline);


		if (shutdown_) break;

		std::unique_ptr<grpc::ClientReader<heartbeat_system::ClusterStatus>> reader(
				stub_->SubscribeToCluster(&stream_context, client_info));

		if (!reader) {
			LOG(WARNING) << "Failed to create cluster status reader. Retrying...";
			std::this_thread::sleep_for(std::chrono::seconds(2));
			continue;
		}

		while (true) { // Loop until Read fails or shutdown is detected
			if (shutdown_) {
				// Need to explicitly cancel the context *before* Finish if shutting down mid-stream
				stream_context.TryCancel();
				break; // Exit inner loop
			}

			// Read() will now return false on error, stream end, OR deadline exceeded
			if (!reader->Read(&cluster_status)) {
				break; // Exit inner loop - Read failed or stream ended
			}

			// Process status if read succeeds
			connected_ = true;
			const auto& new_nodes_proto = cluster_status.new_nodes();
			if (!new_nodes_proto.empty()) {
				std::vector<std::pair<int, std::string>> brokers_to_add;
				{ // Lock scope
					absl::MutexLock lock(&node_mutex_);
					for (const auto& addr : new_nodes_proto) {
						int broker_id = GetBrokerId(addr);
						if (nodes_.find(broker_id) == nodes_.end()) {
							VLOG(5) << "Discovered new broker: ID=" << broker_id << ", Addr=" << addr;
							nodes_[broker_id] = addr;
							brokers_to_add.push_back({broker_id, addr});
						}
					}
				} // Lock released
				for(const auto& pair : brokers_to_add) {
					std::thread manager_thread(&Subscriber::ManageBrokerConnections, this, pair.first, pair.second);
					manager_thread.detach();
				}
			} // End processing status
		} // End inner loop


		// Finish the stream (will also respect the deadline)
		grpc::Status status = reader->Finish();


		// Check status and shutdown flag AFTER Finish()
		if (shutdown_) {
			VLOG(5) << "Cluster status loop exiting due to shutdown request.";
			break; // Exit outer loop
		}

		// Log reason for stream ending (optional but helpful)
		if (status.ok()) {
			VLOG(5) << "Cluster status stream finished cleanly. Re-establishing after delay...";
			std::this_thread::sleep_for(std::chrono::seconds(5));
		} else if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
			LOG(WARNING) << "Cluster status stream deadline exceeded. Re-establishing...";
			// No extra delay needed, loop will restart immediately
		} else if (status.error_code() == grpc::StatusCode::CANCELLED) {
			// This might happen if TryCancel was called due to shutdown flag
			LOG(INFO) << "Cluster status stream cancelled. Exiting loop.";
			break; // Exit outer loop
		} else {
			LOG(WARNING) << "Cluster status stream failed: (" << status.error_code() << ") "
				<< status.error_message() << ". Retrying after delay...";
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}

	} // End outer while(!shutdown_)

	VLOG(5) << "SubscribeToClusterStatus thread finished.";
}

bool ConnectionBuffers::signal_and_attempt_swap(Subscriber* subscriber_instance) {
	absl::MutexLock lock(&state_mutex); // Lock for state changes

	int write_idx = current_write_idx.load(std::memory_order_acquire);
	int read_idx = 1 - write_idx;

	// Mark the buffer we just filled as ready for the consumer
	if (buffers[write_idx].write_offset.load(std::memory_order_relaxed) > 0) { // Only if not empty
		write_buffer_ready_for_consumer.store(true, std::memory_order_release);
		VLOG(4) << "FD=" << fd << ": Marked buffer " << write_idx << " ready for consumer.";
		// Wake up potentially waiting consumer(s) - they need to check flags
		subscriber_instance->consume_cv_.SignalAll(); // Use the global CV from Subscriber
	} else {
		VLOG(4) << "FD=" << fd << ": Write buffer " << write_idx << " is empty, not marking ready.";
		// If the buffer is empty, we might still want to swap if the other is free
		// This prevents getting stuck if we fill buffer 0, swap, fill buffer 1,
		// then get 0 bytes on buffer 1 before consumer reads buffer 0.
	}

	// Check if the *other* buffer (read_idx) is free
	if (!read_buffer_in_use_by_consumer.load(std::memory_order_acquire)) {
		// Swap successful! Reset the new write buffer's state.
		current_write_idx.store(read_idx, std::memory_order_release);
		buffers[read_idx].write_offset.store(0, std::memory_order_relaxed);
		// We don't reset write_buffer_ready_for_consumer here; that happens
		// when the *consumer acquires* the buffer (now buffers[write_idx]).
		VLOG(4) << "FD=" << fd << ": Swapped to write buffer " << read_idx << ". Other buffer free.";
		return true;
	} else {
		// Swap failed, consumer is still using the other buffer
		VLOG(4) << "FD=" << fd << ": Cannot swap, consumer active on buffer " << read_idx;
		return false;
	}
}

BufferState* ConnectionBuffers::acquire_read_buffer() {
	absl::MutexLock lock(&state_mutex);

	// We want the buffer that is *not* current_write_idx, but *is* ready, and *not* in use.
	int potential_read_idx = 1 - current_write_idx.load(std::memory_order_acquire);

	if (write_buffer_ready_for_consumer.load(std::memory_order_acquire) &&
			!read_buffer_in_use_by_consumer.load(std::memory_order_acquire))
	{
		// Check if the ready buffer is indeed the one the consumer should read
		// This condition implies the receiver filled 'potential_read_idx' and marked it ready,
		// OR receiver filled 'current_write_idx', marked it ready, BUT hasn't swapped yet because consumer was busy.
		// We need to know WHICH buffer is ready. Let's assume write_buffer_ready_for_consumer
		// refers to the buffer INDEXED by `potential_read_idx`. This needs careful state design.

		// Let's simplify: Assume write_buffer_ready refers to the non-writing buffer if set.
		BufferState* ready_buffer = &buffers[potential_read_idx];
		if (ready_buffer->write_offset.load(std::memory_order_relaxed) > 0) { // Check if actually has data
			read_buffer_in_use_by_consumer.store(true, std::memory_order_release);
			write_buffer_ready_for_consumer.store(false, std::memory_order_relaxed); // Consume the 'ready' signal
			VLOG(3) << "FD=" << fd << ": Consumer acquired read buffer " << potential_read_idx;
			return ready_buffer;
		} else {
			// Marked ready but somehow empty? Reset flag.
			// write_buffer_ready_for_consumer.store(false, std::memory_order_relaxed); // Reset if empty? Maybe not here.
			VLOG(4) << "FD=" << fd << ": Buffer " << potential_read_idx << " marked ready but seems empty.";
			return nullptr;
		}
	}

	VLOG(5) << "FD=" << fd << ": No buffer ready for consumer or consumer already active.";
	return nullptr; // No buffer available right now
}

void ConnectionBuffers::release_read_buffer(BufferState* acquired_buffer) {
	// Find index matching acquired_buffer
	int released_idx = -1;
	if (acquired_buffer == &buffers[0]) released_idx = 0;
	else if (acquired_buffer == &buffers[1]) released_idx = 1;
	else {
		LOG(ERROR) << "FD=" << fd << ": release_read_buffer called with invalid buffer pointer.";
		return;
	}

	absl::MutexLock lock(&state_mutex);
	read_buffer_in_use_by_consumer.store(false, std::memory_order_release);
	VLOG(3) << "FD=" << fd << ": Consumer released read buffer " << released_idx;
	// Notify the receiver thread *for this connection* that might be waiting to swap
	receiver_can_write_cv.Signal();
}

ConsumedData Subscriber::Consume(std::chrono::milliseconds timeout) {
	absl::MutexLock lock(&connection_map_mutex_); // Lock map while iterating

	// Simple round-robin or just iterate start? Store last consumed FD?
	// Let's just iterate for now.

	auto start_time = std::chrono::steady_clock::now();
	do {
		for (auto const& [fd, conn_ptr] : connections_) {
			if (conn_ptr) {
				// Attempt to acquire a read buffer from this connection
				// acquire_read_buffer locks its own state mutex internally
				BufferState* acquired_buffer = conn_ptr->acquire_read_buffer();
				if (acquired_buffer) {
					// Found a buffer! Prepare ConsumedData
					ConsumedData result;
					result.connection = conn_ptr; // Store shared_ptr to keep it alive
					result.buffer_state = acquired_buffer;
					result.data_size = acquired_buffer->write_offset.load(std::memory_order_relaxed); // Get size when acquired

					VLOG(3) << "Consume: Returning buffer " << (acquired_buffer == &conn_ptr->buffers[0] ? 0 : 1)
						<< " from fd=" << fd << " with size=" << result.data_size;
					return result; // Return the consumable data block
				}
			}
		} // End for loop through connections

		// No buffer found in this pass. Wait?
		if (timeout == std::chrono::milliseconds(0)) {
			break; // Non-blocking: exit loop if nothing found first time
		}

		VLOG(5) << "Consume: No data ready, waiting...";
		// Wait for a signal that *some* buffer *might* be ready
		// Unlock map mutex while waiting on global CV
		consume_cv_.WaitWithTimeout(&connection_map_mutex_, absl::Milliseconds(timeout.count()));
		VLOG(5) << "Consume: Woke up from wait.";

		// Check timeout
		if (std::chrono::steady_clock::now() - start_time >= timeout) {
			VLOG(4) << "Consume: Timeout expired.";
			break; // Timeout expired
		}
		// If not timed out, loop again to check connections

	} while (timeout > std::chrono::milliseconds(0)); // Loop only if timeout > 0


	VLOG(4) << "Consume: Returning empty data block.";
	return ConsumedData{}; // Return empty object if no data found or timeout
}
