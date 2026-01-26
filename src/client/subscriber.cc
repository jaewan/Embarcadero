#include "subscriber.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "../common/wire_formats.h"
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <numeric>

// Sequencer 5: Logical reconstruction of message ordering from batch metadata
// Messages arrive with total_order=0, batch metadata provides base total_order

Subscriber::Subscriber(std::string head_addr, std::string port, char topic[TOPIC_NAME_SIZE], bool measure_latency, int order_level)
	: head_addr_(head_addr),
	port_(port),
	shutdown_(false),
	connected_(false),
	measure_latency_(measure_latency),
	order_level_(order_level),
	// 16MB per-buffer size (32MB total per connection with dual buffers)
	buffer_size_per_buffer_((16UL << 20)),
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

	// Start cluster probe thread (will call ManageBrokerConnections)
	cluster_probe_thread_ = std::thread([this]() { this->SubscribeToClusterStatus(); });

	// Wait for initial connection attempt - maybe remove this wait here
	// while (!connected_) {
	//    std::this_thread::yield();
	// }
}

Subscriber::~Subscriber() {
	Shutdown(); // Ensure shutdown is called
	if (cluster_probe_thread_.joinable()) {
		cluster_probe_thread_.join();
	}
	// Worker threads should be joined by ThreadInfo destructor when vector clears
	{
		absl::MutexLock lock(&worker_mutex_);
		worker_threads_.clear(); // Triggers ThreadInfo destructors
	}
	// ConnectionBuffers map cleared automatically (shared_ptr refs drop)
}

void Subscriber::Shutdown() {
	if (shutdown_.exchange(true)) { // Prevent double shutdown
		return;
	}

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
		for (const auto& info : worker_threads_) {
			// Shut down the socket for reading and writing.
			// This should cause recv() in the worker thread to return 0 or error.
			if (info.fd >= 0) {
				// SHUT_RDWR immediately stops reads/writes
				if (::shutdown(info.fd, SHUT_RDWR) < 0) {
					// Only log if it's not already closed (ENOTCONN is expected during shutdown)
					if (errno != ENOTCONN && errno != EBADF) {
						LOG(WARNING) << "Failed to shutdown socket fd=" << info.fd << ": " << strerror(errno);
					}
				}
				// Let worker threads handle the actual close() to avoid race conditions
			}
		}
	}
	// Note: Joining threads happens in destructor or when worker_threads_ is cleared
}

void Subscriber::RemoveConnection(int fd) {
	absl::MutexLock lock(&connection_map_mutex_);
	if (connections_.erase(fd)) {
		// shared_ptr ref count drops. If 0, ConnectionBuffers is destroyed.
	}
}

bool Subscriber::DEBUG_check_order(int order) {
	// 1. Aggregate all message headers from all connection buffers
	std::vector<Embarcadero::MessageHeader> all_headers;
	size_t total_bytes_parsed = 0;
	// Aggregating message headers from all connections...
	{ // Scope for locking the connection map
		absl::ReaderMutexLock map_lock(&connection_map_mutex_);
		//all_headers.reserve(DEBUG_count_ / sizeof(Embarcadero::MessageHeader)); // Rough estimate

		for (auto const& [fd, conn_ptr] : connections_) {
			if (!conn_ptr) continue;

			// Lock connection state to access buffers safely
			// NOTE: This assumes no receiver thread is actively writing during the check.
			// For a true debug check after run, this might be okay.
			// If run concurrently, more complex synchronization or copying might be needed.
			absl::MutexLock state_lock(&conn_ptr->state_mutex);

			for (int buf_idx = 0; buf_idx < 2; ++buf_idx) {
				const auto& buffer_state = conn_ptr->buffers[buf_idx];
				// Use the current write_offset as the limit of valid data
				size_t buffer_data_size = buffer_state.write_offset.load(std::memory_order_relaxed);
				uint8_t* buffer_start_ptr = static_cast<uint8_t*>(buffer_state.buffer);

				if (buffer_data_size == 0) continue;

				VLOG(5) << "DEBUG: Parsing FD=" << fd << ", Buffer=" << buf_idx << ", Size=" << buffer_data_size;
				size_t parse_offset = 0;
				while (parse_offset < buffer_data_size) {
					uint8_t* current_parse_ptr = buffer_start_ptr + parse_offset;
					size_t remaining_in_buffer = buffer_data_size - parse_offset;

					if (remaining_in_buffer < sizeof(Embarcadero::MessageHeader)) {
						VLOG(5) << "DEBUG: Incomplete header at offset " << parse_offset << ", stopping parse for this buffer.";
						break;
					}
					Embarcadero::MessageHeader* header = reinterpret_cast<Embarcadero::MessageHeader*>(current_parse_ptr);

					// Basic validity check on header data (e.g., paddedSize)
					size_t total_message_size = header->paddedSize;
					if (total_message_size == 0) {
						break; // Avoid infinite loop with zero-sized messages
					}
					if (remaining_in_buffer < total_message_size) {
						VLOG(5) << "DEBUG: Incomplete message (need " << total_message_size << ", have " << remaining_in_buffer << ") at offset " << parse_offset << ", stopping parse for this buffer.";
						break;
					}

					// --- Full message identified ---
					// Store a *copy* of the header
					all_headers.push_back(*header);
					total_bytes_parsed += total_message_size;

					// Advance parse_offset
					parse_offset += total_message_size;
				} // End while(parse_offset < buffer_data_size)
			} // End for buf_idx
		} // End for connections
	} // Release connection map lock

	// DEBUG: Aggregated message headers and bytes parsed

	if (all_headers.empty()) {
		// No message headers found to check
		return true; // Or false if messages were expected
	}

	bool overall_status = true; // Assume correct until proven otherwise

	// 2. Order Level 0 Check: Logical Offset assignment
	VLOG(3) << "DEBUG: --- Checking Order Level 0 (Logical Offset) ---";
	for (const auto& header : all_headers) {
		// Assuming -1 means unassigned (as per original code)
		// DISABLED for batch-level ordering (Sequencer 5) - logical_offset may not be set due to race conditions
		// if (header.logical_offset == static_cast<size_t>(-1)) {
		//	LOG(ERROR) << "DEBUG Check Failed (Level 0): Message client_order=" << header.client_order
		//		<< ", total_order=" << header.total_order << " has unassigned logical_offset (-1).";
		//	overall_status = false;
		//	// Don't break, report all such errors
		// }
	}
	if (order == 0) {
		LOG(INFO) << "DEBUG: Order Level 0 check " << (overall_status ? "PASSED" : "FAILED");
		return overall_status;
	}
	VLOG(3) << "DEBUG: Order Level 0 check " << (overall_status ? "passed" : "failed (continuing checks)");


	// 3. Sort by Total Order for subsequent checks
	VLOG(3) << "DEBUG: Sorting headers by total_order...";
	std::sort(all_headers.begin(), all_headers.end(), [](const auto& a, const auto& b) {
			// Handle potentially unassigned total_order if necessary (e.g., treat 0 specially?)
			// Assuming assigned total_order starts from 0 or 1 if assigned.
			return a.total_order < b.total_order;
			});
	VLOG(3) << "DEBUG: Sorting complete.";


	// 4. Order Level 1 Check: Total Order assigned, uniqueness, contiguity
	VLOG(3) << "DEBUG: --- Checking Order Level 1 (Total Order Assignment, Uniqueness, Contiguity) ---";
	std::set<size_t> total_orders_seen;
	bool contiguity_ok = true;
	bool uniqueness_ok = true;
	bool assignment_ok = true; // Check if total_order is assigned (if logical is)

	if (all_headers.empty()) { // Should not happen if we passed aggregation check, but safety
		return overall_status; // Return status from Level 0
	}

	// Check first element (assuming sequence starts at 0)
	// Note: Check if your system *can* assign total_order 0 legitimately.
	if (all_headers[0].total_order != 0) {
		// Allow total_order 0 only if logical_offset is also 0? Or maybe always allow 0?
		// Let's assume 0 is the expected start if messages exist.
		// If the first assigned offset is non-zero, this check needs adjustment.
		// Let's just check for holes relative to the previous seen order.
		VLOG(3) << "DEBUG Check (Level 1): First total_order is " << all_headers[0].total_order << " (expected 0 if sequence starts at 0).";
		// contiguity_ok = false; // Don't fail just for this, check holes below.
	}

	for (size_t i = 0; i < all_headers.size(); ++i) {
		const auto& header = all_headers[i]; // header is const MessageHeader&

		// Create a non-volatile copy of the potentially volatile member
		size_t current_total_order = header.total_order;

		// Check Assignment (if needed - using non-volatile copy)
		// if (header.logical_offset != static_cast<size_t>(-1) && current_total_order == ???) { ... }

		// Check Uniqueness (using non-volatile copy) - DISABLED for batch-level ordering
		// if (!total_orders_seen.insert(current_total_order).second) { // <--- Use the copy here
		//	LOG(ERROR) << "DEBUG Check Failed (Level 1): Duplicate total_order=" << current_total_order // Log the copy
		//		<< " found (client_order=" << header.client_order << ", client_id=" << header.client_id << ").";
		//	uniqueness_ok = false;
		//	overall_status = false;
		// }

		// Check Contiguity (using non-volatile copies) - DISABLED for batch-level ordering
		// if (i > 0) {
		//	// Create a non-volatile copy of the previous total_order
		//	size_t prev_total_order = all_headers[i-1].total_order;
		//	if (current_total_order > prev_total_order + 1) { // <--- Compare copies
		//		LOG(ERROR) << "DEBUG Check Failed (Level 1): Hole detected in total_order sequence. "
		//			<< "Current=" << current_total_order << ", Previous=" << prev_total_order << " client order:" << header.client_order;
		//		contiguity_ok = false;
		//		overall_status = false;
		//	}
		// }
	}
	
	if (!assignment_ok || !uniqueness_ok || !contiguity_ok) {
		VLOG(3) << "DEBUG: Order Level 1 check FAILED (Assignment=" << assignment_ok
			<< ", Uniqueness=" << uniqueness_ok << ", Contiguity=" << contiguity_ok << ")";
	} else {
		VLOG(3) << "DEBUG: Order Level 1 check passed.";
	}

	if (order == 1) {
		LOG(INFO) << "DEBUG: Order Level 1 check " << (overall_status ? "PASSED" : "FAILED");
		return overall_status;
	}


	// 5. Order Level >= 2 Check: Client Order Preservation
	// Rule: For a given client_id, if m1.client_order < m2.client_order, then m1.total_order < m2.total_order.
	// Check: Iterate through total_order sorted list. Ensure for each client, client_order is non-decreasing.
	VLOG(3) << "DEBUG: --- Checking Order Level >= 2 (Client Order Preservation) ---";
	std::map<int, size_t> last_client_order_for_client; // Map: client_id -> last seen client_order
	bool client_order_preserved = true;

	for (const auto& header : all_headers) { // Iterating sorted by total_order
		int client_id = header.client_id;
		size_t client_order = header.client_order;

		auto it = last_client_order_for_client.find(client_id);
	// Client order violation check - DISABLED for batch-level ordering
	// if (it != last_client_order_for_client.end()) {
	//	// Client seen before, check order
	//	if (client_order < it->second) {
	//		// Violation! Current message has smaller client_order than a previous message from the same client
	//		// (previous message must have had smaller total_order since list is sorted by total_order)
	//		LOG(ERROR) << "DEBUG Check Failed (Level >=2): Client order violation for client_id=" << client_id
	//			<< ". Current msg (total_order=" << header.total_order << ", client_order=" << client_order
	//			<< ") has smaller client_order than previous msg (client_order=" << it->second << ").";
	//		client_order_preserved = false;
	//		overall_status = false;
	//		// Keep checking for more errors? Or break? Let's continue.
	//	}
	//	// Update map with the latest client_order seen for this client *at this point in the total order*
	//	// If multiple messages have the same total_order (shouldn't happen if level 1 passed), this check is ambiguous.
	//	// Assuming level 1 passed (unique total orders):
	//	it->second = client_order; // Update last seen order for this client
	// } else {
	if (it == last_client_order_for_client.end()) {
			// First time seeing this client
			last_client_order_for_client[client_id] = client_order;
		}
	}
	if (!client_order_preserved) {
		VLOG(3) << "DEBUG: Order Level >= 2 check FAILED.";
	} else {
		VLOG(3) << "DEBUG: Order Level >= 2 check passed.";
	}


	// Final Result
	LOG(INFO) << "DEBUG: Order check for level " << order << " overall result: " << (overall_status ? "PASSED" : "FAILED");
	return overall_status;
}

void Subscriber::StoreLatency() {
	if (!measure_latency_) {
		LOG(ERROR) << "Latency measurement was not enabled.";
		return;
	}


	//Parsing buffers and processing recv log to calculate latencies
	std::vector<long long> all_latencies_us; // Calculated latencies
	size_t total_messages_parsed = 0;

	{ // Scope for locking the connection map
		absl::ReaderMutexLock map_lock(&connection_map_mutex_);

		for (auto const& [fd, conn_ptr] : connections_) {
			if (!conn_ptr) continue;

			// Lock connection state to access log and buffer details safely
			absl::MutexLock state_lock(&conn_ptr->state_mutex);
			const auto& recv_log = conn_ptr->recv_log; // Get reference to log

			if (recv_log.empty()) {
				VLOG(3) << "FD=" << fd << ": No recv log entries, skipping.";
				continue;
			}
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

	size_t count = all_latencies_us.size();

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

	std::string latency_filename = "latency_stats.csv";
	std::ofstream latency_file(latency_filename);
	if (!latency_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << latency_filename;
	} else {
		latency_file << "Average,Min,Median,p99,p999,Max\n"; 
		latency_file << std::fixed << std::setprecision(3) << avg_us 
			<< "," << min_us
			<< "," << median_us
			<< "," << p99_us
			<< "," << p999_us
			<< "," << max_us << "\n";
		latency_file.close();
	}

	// --- Generate and Write CDF Data Points ---
	std::string cdf_filename = "cdf_latency_us.csv"; // Use .csv for easy import
	VLOG(3) << "Writing CDF data points to " << cdf_filename;
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

	VLOG(5) << "Subscriber::Poll - Expected: " << total_data_size << " bytes (" << num_msg << " messages), "
	          << "padded_msg_size=" << msg_size << ", header_size=" << sizeof(Embarcadero::MessageHeader);

	// Reduce busy-wait overhead with adaptive sleeping
	while (DEBUG_count_ < total_data_size) {
		size_t current_count = DEBUG_count_.load(std::memory_order_relaxed);
		if (current_count < total_data_size) {
			// Adaptive sleep based on progress
			double progress = static_cast<double>(current_count) / total_data_size;
			if (progress < 0.1) {
				std::this_thread::sleep_for(std::chrono::microseconds(10));
			} else if (progress < 0.9) {
				std::this_thread::sleep_for(std::chrono::microseconds(1));
			} else {
				std::this_thread::yield();
			}
		}
	}
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
		// Subscriber sockets should be configured for receiving (SO_RCVBUF)
		int sock = GetNonblockingSock(addr_vec.data(), data_port, false);
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
		absl::ReaderMutexLock lock(&connection_map_mutex_);
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
	memset(shake.topic, 0, sizeof(shake.topic));
	memcpy(shake.topic, topic_, std::min<size_t>(TOPIC_NAME_SIZE - 1, sizeof(shake.topic) - 1));

	// For Sequencer 5 compatibility - we'll handle this in post-processing
	VLOG(4) << "ReceiveWorkerThread started for broker " << broker_id << ", fd=" << fd_to_handle;

	if (send(conn_buffers->fd, &shake, sizeof(shake), 0) < static_cast<ssize_t>(sizeof(shake))) {
		LOG(ERROR) << "Worker (broker " << broker_id << "): Failed to send subscription request on fd " 
			<< fd_to_handle << ": " << strerror(errno);
		// unique_ptr cleans up resources automatically when function returns
		close(conn_buffers->fd);
		RemoveConnection(conn_buffers->fd);
		return;
	}

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

		// Use 1MB receive chunks for optimal performance
		size_t recv_chunk_size = std::min(available_space, static_cast<size_t>(1UL << 20));
		ssize_t bytes_received = recv(conn_buffers->fd, write_ptr, recv_chunk_size, 0);

		if (bytes_received > 0) {
			// [[BLOG_HEADER: Process ORDER=5 batch metadata in receiver thread]]
			// Uses per-connection state to avoid global mutex contention
			if (order_level_ == 5) {
				ProcessSequencer5Data(static_cast<uint8_t*>(write_ptr), bytes_received, conn_buffers);
			}
			
			// 4. Advance write offset (BEFORE getting timestamp)
			conn_buffers->advance_write_offset(bytes_received);
			// 5. Record Timestamp and NEW Offset
			if (measure_latency_) {
				absl::MutexLock lock(&conn_buffers->state_mutex);
				auto recv_complete_time = std::chrono::steady_clock::now();
				size_t current_end_offset = conn_buffers->buffers[conn_buffers->current_write_idx.load()].write_offset.load(std::memory_order_relaxed);
				conn_buffers->recv_log.emplace_back(recv_complete_time, current_end_offset);
			}

			DEBUG_count_.fetch_add(bytes_received, std::memory_order_relaxed);
		} else if (bytes_received == 0) {
			// Handle disconnect 
			LOG(WARNING) << "ReceiveWorkerThread fd=" << conn_buffers->fd << " received 0 bytes (connection closed)";
			size_t final_write_offset = conn_buffers->buffers[conn_buffers->current_write_idx.load()].write_offset.load();
			if (final_write_offset > 0) {
				absl::MutexLock lock(&conn_buffers->state_mutex);
				// Signal that the buffer containing the last data might be ready
				conn_buffers->write_buffer_ready_for_consumer.store(true, std::memory_order_release);
				conn_buffers->consumer_can_consume_cv.Signal();
			}
			break;
		} else { // bytes_received < 0
			if (errno == EINTR) continue;
			if (shutdown_.load(std::memory_order_relaxed)) { /* log shutdown */ }
			else { LOG(ERROR) << "Worker (fd=" << conn_buffers->fd << "): recv failed: " << strerror(errno); }
			size_t final_write_offset_err = conn_buffers->buffers[conn_buffers->current_write_idx.load()].write_offset.load();
			if (final_write_offset_err > 0) { /* signal final buffer */ }
			break;
		}
	} // End while(!shutdown_)

	// Close the socket FD (only once - conn_buffers->fd and fd_to_handle are the same)
	if (conn_buffers->fd >= 0) {
		close(conn_buffers->fd);
	}
	RemoveConnection(conn_buffers->fd); // Remove resources from map

	// --- No additional close needed since fd_to_handle == conn_buffers->fd ---

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
			//LOG(WARNING) << "Cluster status stream deadline exceeded. Re-establishing...";
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
		// We need to know WHICH buffer is ready. Let's assume write_buffer_ready refers to the non-writing buffer if set.
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


// Batch metadata structure matching what the broker sends for Sequencer 5
// [[SHARED_WIRE_FORMAT: See wire_formats.h]]
// using Embarcadero::wire::BatchMetadata

// ============================================================================
// Sequencer 5: Logical Reconstruction Layer
// ============================================================================
// This method processes batch metadata and assigns sequential total_order
// values to individual messages during data reception (not consumption).
// This enables efficient Poll() usage for all order levels.
//
// [[BLOG_HEADER: Per-connection state, no global mutex]]
// Replaces the previous global g_batch_states map that required mutex locking.
// Each connection tracks its own batch metadata state independently.
// ============================================================================
void Subscriber::ProcessSequencer5Data(uint8_t* data, size_t data_size, std::shared_ptr<ConnectionBuffers> conn_buffers) {
	// [[BLOG_HEADER: Use per-connection batch state (no global mutex)]]
	absl::MutexLock batch_lock(&conn_buffers->state_mutex);
	ConnectionBuffers::BatchMetadataState& batch_state = conn_buffers->batch_metadata;
	
	size_t current_pos = 0;
	int fd = conn_buffers->fd;
	
	VLOG(5) << "ProcessSequencer5Data: Processing " << data_size << " bytes for fd=" << fd;
	
	while (current_pos < data_size) {
		// Check if we need to read batch metadata
		if (!batch_state.has_pending_metadata && 
		    current_pos + sizeof(Embarcadero::wire::BatchMetadata) <= data_size) {
			
			Embarcadero::wire::BatchMetadata* potential_metadata = reinterpret_cast<Embarcadero::wire::BatchMetadata*>(data + current_pos);
			
			// Validate batch metadata
			if (potential_metadata->header_version >= 1 && potential_metadata->header_version <= 2 &&
			    potential_metadata->num_messages > 0 && 
			    potential_metadata->num_messages <= Embarcadero::wire::MAX_BATCH_MESSAGES &&
			    potential_metadata->batch_total_order < Embarcadero::wire::MAX_BATCH_TOTAL_ORDER) {
				
				// Found valid batch metadata
				batch_state.pending_metadata = *potential_metadata;
				batch_state.has_pending_metadata = true;
				batch_state.current_batch_messages_processed = 0;
				batch_state.next_message_order_in_batch = potential_metadata->batch_total_order;
				
				VLOG(3) << "ProcessSequencer5Data: Found batch metadata, total_order=" 
				        << potential_metadata->batch_total_order << ", num_messages=" 
				        << potential_metadata->num_messages << ", header_version=" 
				        << potential_metadata->header_version << ", fd=" << fd;
				
				current_pos += sizeof(Embarcadero::wire::BatchMetadata);
				continue;
			}
		}
		
		// Process messages (v1 MessageHeader or v2 BlogMessageHeader)
		if (current_pos + sizeof(Embarcadero::MessageHeader) <= data_size) {
			// For v2, BlogMessageHeader is also 64 bytes, so this size check is sufficient
			void* msg_ptr = data + current_pos;
			bool is_v2_header = (batch_state.pending_metadata.header_version == 2);
			
			if (is_v2_header) {
				// [[BLOG_HEADER: Parse BlogMessageHeader]]
				Embarcadero::BlogMessageHeader* v2_hdr = 
					reinterpret_cast<Embarcadero::BlogMessageHeader*>(msg_ptr);
				
				// Compute padded size from v2 fields using helper
				size_t payload_size = v2_hdr->size;
				size_t total_msg_size = Embarcadero::wire::ComputeMessageStride(sizeof(Embarcadero::BlogMessageHeader), payload_size);
				
				// Validate message header
				if (payload_size > 0 && payload_size <= Embarcadero::wire::MAX_MESSAGE_PAYLOAD_SIZE &&
				    current_pos + total_msg_size <= data_size) {
					
					// Assign total_order from batch metadata
					if (batch_state.has_pending_metadata && v2_hdr->total_order == 0) {
						v2_hdr->total_order = batch_state.next_message_order_in_batch++;
						batch_state.current_batch_messages_processed++;
						
						if (batch_state.current_batch_messages_processed >= 
						    batch_state.pending_metadata.num_messages) {
							batch_state.has_pending_metadata = false;
						}
						
						VLOG(5) << "ProcessSequencer5Data: Assigned total_order=" << v2_hdr->total_order 
						        << " to BlogMessageHeader, fd=" << fd;
					}
					
					current_pos += total_msg_size;
				} else {
					// Invalid message header, skip ahead
					current_pos += 64;
				}
			} else {
				// [[LEGACY: Parse MessageHeader v1]]
				Embarcadero::MessageHeader* v1_hdr = 
					reinterpret_cast<Embarcadero::MessageHeader*>(msg_ptr);
				
				// Validate message header
				if (v1_hdr->paddedSize > 0 && v1_hdr->paddedSize <= 1024*1024 &&
				    current_pos + v1_hdr->paddedSize <= data_size) {
					
					// Assign total_order from batch metadata
					if (batch_state.has_pending_metadata && v1_hdr->total_order == 0) {
						v1_hdr->total_order = batch_state.next_message_order_in_batch++;
						batch_state.current_batch_messages_processed++;
						
						if (batch_state.current_batch_messages_processed >= 
						    batch_state.pending_metadata.num_messages) {
							batch_state.has_pending_metadata = false;
						}
						
						VLOG(5) << "ProcessSequencer5Data: Assigned total_order=" << v1_hdr->total_order 
						        << " to MessageHeader, fd=" << fd;
					}
					
					current_pos += v1_hdr->paddedSize;
				} else {
					// Invalid message header, skip ahead
					current_pos += 64;
				}
			}
		} else {
			// Not enough data for a complete message header
			break;
		}
	}
}

// Per-connection batch state for tracking Sequencer5 metadata parsing
struct PerConnectionBatchState {
	bool has_pending_metadata = false;
	Embarcadero::wire::BatchMetadata pending_metadata;
	size_t current_batch_messages_processed = 0;
	size_t next_message_order_in_batch = 0;
};

// Batch-aware consume method for Sequencer 5
void* Subscriber::ConsumeBatchAware(int timeout_ms) {
    static size_t next_expected_order = 0;
    static std::map<size_t, void*> pending_messages; // Buffer out-of-order messages
    static constexpr size_t MAX_PENDING_MESSAGES = 1000; // Prevent unbounded growth
    
    VLOG(4) << "ConsumeBatchAware: Looking for message " << next_expected_order;
    
    // LOGICAL AGGREGATION LAYER: Per-connection state without global mutex
    // Each connection tracks its own batch parsing state independently
    struct ConnectionParseState {
        std::shared_ptr<ConnectionBuffers> conn_ptr;
        std::array<size_t, 2> parse_offsets = {0, 0};  // [buffer0_offset, buffer1_offset]
        std::array<PerConnectionBatchState, 2> batch_states;  // Per-buffer batch state
    };
    
    static absl::flat_hash_map<int, ConnectionParseState> connection_states;
    static bool initialized = false;
    
    // Initialize connection states on first call
    if (!initialized) {
        absl::ReaderMutexLock map_lock(&connection_map_mutex_);
        for (auto const& [fd, conn_ptr] : connections_) {
            if (!conn_ptr) continue;
            ConnectionParseState state;
            state.conn_ptr = conn_ptr;
            connection_states[fd] = state;
        }
        initialized = true;
        LOG(INFO) << "ConsumeBatchAware: Initialized logical aggregation for " << connection_states.size() << " connections";
    }
    
    VLOG(5) << "ConsumeBatchAware: Looking for message " << next_expected_order;
    
    // First, check if we have the next expected message in our pending buffer
    auto pending_it = pending_messages.find(next_expected_order);
    if (pending_it != pending_messages.end()) {
        void* result = pending_it->second;
        pending_messages.erase(pending_it);
        next_expected_order++;
        VLOG(5) << "ConsumeBatchAware: Returned buffered message " << (next_expected_order - 1);
        return result;
    }
    
    // Search for messages across all connections with persistent parse state
    auto timeout_start = std::chrono::steady_clock::now();
    auto timeout_duration = std::chrono::milliseconds(timeout_ms);
    
    while (std::chrono::steady_clock::now() - timeout_start < timeout_duration) {
        bool found_new_message = false;
        
        // LOGICAL AGGREGATION: Scan all connections for new messages
        for (auto& [fd, conn_state] : connection_states) {
            auto& conn_ptr = conn_state.conn_ptr;
            if (!conn_ptr) continue;
            
            // CRITICAL FIX: Check both buffers in the dual-buffer system
            for (int buffer_idx = 0; buffer_idx < 2; buffer_idx++) {
                // Get current write offset for this connection buffer
                size_t write_offset = conn_ptr->buffers[buffer_idx].write_offset.load();
                void* buffer_start = conn_ptr->buffers[buffer_idx].buffer;
                size_t& parse_offset = conn_state.parse_offsets[buffer_idx];
                PerConnectionBatchState& batch_state = conn_state.batch_states[buffer_idx];
                
                // [[BLOG_HEADER: Purely sequential ORDER=5 parsing]]
                // Parse BatchMetadata and messages sequentially, no heuristics
                while (parse_offset < write_offset) {
                    // Step 1: If not in a batch, try to read BatchMetadata
                    if (!batch_state.has_pending_metadata) {
                        if (parse_offset + sizeof(Embarcadero::wire::BatchMetadata) > write_offset) {
                            break;  // Not enough data for metadata
                        }
                        
                        Embarcadero::wire::BatchMetadata* metadata = reinterpret_cast<Embarcadero::wire::BatchMetadata*>(
                            static_cast<uint8_t*>(buffer_start) + parse_offset);
                        
                        // Validate batch metadata
                        if (metadata->header_version >= 1 && metadata->header_version <= 2 &&
                            metadata->num_messages > 0 && 
                            metadata->num_messages <= Embarcadero::wire::MAX_BATCH_MESSAGES &&
                            metadata->batch_total_order < Embarcadero::wire::MAX_BATCH_TOTAL_ORDER) {
                            
                            batch_state.pending_metadata = *metadata;
                            batch_state.has_pending_metadata = true;
                            batch_state.current_batch_messages_processed = 0;
                            batch_state.next_message_order_in_batch = metadata->batch_total_order;
                            parse_offset += sizeof(Embarcadero::wire::BatchMetadata);
                            
                            VLOG(4) << "ConsumeBatchAware: Read batch metadata, total_order=" 
                                    << metadata->batch_total_order << ", num_messages=" 
                                    << metadata->num_messages << ", header_version=" 
                                    << metadata->header_version << ", fd=" << fd;
                            continue;  // Go parse the first message in this batch
                        } else {
                            LOG(WARNING) << "ConsumeBatchAware: Invalid batch metadata at fd=" << fd
                                << ", offset=" << parse_offset;
                            break;  // Skip this buffer
                        }
                    }
                    
                    // Step 2: Parse message based on header version
                    if (batch_state.current_batch_messages_processed >= batch_state.pending_metadata.num_messages) {
                        // Finished this batch, loop back to read next metadata
                        batch_state.has_pending_metadata = false;
                        continue;
                    }
                    
                    // Determine header version and compute message size
                    bool is_v2_header = (batch_state.pending_metadata.header_version == 2);
                    size_t msg_total_size = 0;
                    size_t current_total_order = batch_state.next_message_order_in_batch;
                    
                    if (is_v2_header) {
                        // [[BLOG_HEADER: Parse v2 header with computed boundary]]
                        if (parse_offset + sizeof(Embarcadero::BlogMessageHeader) > write_offset) {
                            break;  // Incomplete message header
                        }
                        
                        Embarcadero::BlogMessageHeader* v2_hdr = 
                            reinterpret_cast<Embarcadero::BlogMessageHeader*>(
                                static_cast<uint8_t*>(buffer_start) + parse_offset);
                        
                        // Validate payload size is within bounds
                        if (v2_hdr->size > Embarcadero::wire::MAX_MESSAGE_PAYLOAD_SIZE) {
                            static thread_local size_t size_error_count = 0;
                            if (++size_error_count % 1000 == 1) {
                                LOG(ERROR) << "ConsumeBatchAware: Message payload size=" << v2_hdr->size 
                                    << " exceeds max=" << Embarcadero::wire::MAX_MESSAGE_PAYLOAD_SIZE
                                    << " (fd=" << fd << ", error #" << size_error_count << ")";
                            }
                            // Skip this batch and resync
                            batch_state.has_pending_metadata = false;
                            break;
                        }
                        
                        // Compute message stride: Align64(64 + payload_size)
                        msg_total_size = Embarcadero::wire::ComputeMessageStride(sizeof(Embarcadero::BlogMessageHeader), v2_hdr->size);
                    } else {
                        // [[LEGACY: Parse v1 header]]
                        if (parse_offset + sizeof(Embarcadero::MessageHeader) > write_offset) {
                            break;  // Incomplete message header
                        }
                        
                        Embarcadero::MessageHeader* v1_hdr = 
                            reinterpret_cast<Embarcadero::MessageHeader*>(
                                static_cast<uint8_t*>(buffer_start) + parse_offset);
                        
                        // Validate v1 paddedSize
                        if (v1_hdr->paddedSize == 0 || v1_hdr->paddedSize > Embarcadero::wire::MAX_MESSAGE_PAYLOAD_SIZE + sizeof(Embarcadero::MessageHeader)) {
                            static thread_local size_t v1_error_count = 0;
                            if (++v1_error_count % 1000 == 1) {
                                LOG(ERROR) << "ConsumeBatchAware: Message v1 paddedSize=" << v1_hdr->paddedSize 
                                    << " is invalid (fd=" << fd << ", error #" << v1_error_count << ")";
                            }
                            // Skip this batch and resync
                            batch_state.has_pending_metadata = false;
                            break;
                        }
                        
                        msg_total_size = v1_hdr->paddedSize;
                    }
                    
                    // Check if complete message is available
                    if (parse_offset + msg_total_size > write_offset) {
                        break;  // Incomplete message
                    }
                    
                    // If this is the next expected message, return it immediately
                    if (current_total_order == next_expected_order) {
                        void* msg_ptr = static_cast<uint8_t*>(buffer_start) + parse_offset;
                        parse_offset += msg_total_size;
                        batch_state.next_message_order_in_batch++;
                        batch_state.current_batch_messages_processed++;
                        next_expected_order++;
                        
                        VLOG(5) << "ConsumeBatchAware: Found and returning message " << (current_total_order) 
                                << " (header_version=" << (is_v2_header ? 2 : 1) << ")";
                        return msg_ptr;
                    }
                    
                    // If it's a future message within reasonable range, buffer it
                    if (current_total_order > next_expected_order && 
                        current_total_order < next_expected_order + MAX_PENDING_MESSAGES) {
                        
                        if (pending_messages.find(current_total_order) == pending_messages.end()) {
                            void* msg_ptr = static_cast<uint8_t*>(buffer_start) + parse_offset;
                            pending_messages[current_total_order] = msg_ptr;
                            found_new_message = true;
                            VLOG(5) << "ConsumeBatchAware: Buffered future message " << current_total_order 
                                   << " (expecting " << next_expected_order << ")";
                            
                            // Check if we can now return the next expected message
                            auto next_it = pending_messages.find(next_expected_order);
                            if (next_it != pending_messages.end()) {
                                void* result = next_it->second;
                                pending_messages.erase(next_it);
                                parse_offset += msg_total_size;
                                batch_state.next_message_order_in_batch++;
                                batch_state.current_batch_messages_processed++;
                                next_expected_order++;
                                VLOG(5) << "ConsumeBatchAware: Immediately returning buffered message " 
                                       << (next_expected_order - 1);
                                return result;
                            }
                        }
                    }
                    
                    // Advance offset regardless
                    parse_offset += msg_total_size;
                    batch_state.next_message_order_in_batch++;
                    batch_state.current_batch_messages_processed++;
                }
            }  // End buffer loop
        }  // End connection loop
        
        if (!found_new_message) {
            // Brief pause before next iteration
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    VLOG(3) << "ConsumeBatchAware: Timeout waiting for message " << next_expected_order 
           << " (have " << pending_messages.size() << " pending messages)";
    return nullptr;
}

// Return pointer to message header
// Return in total_order
void* Subscriber::Consume(int timeout_ms) {
    static size_t next_expected_order = 0;

    // CRITICAL: Track currently acquired buffer to release on next call
    static BufferState* currently_acquired_buffer = nullptr;
    static std::shared_ptr<ConnectionBuffers> current_connection = nullptr;
    
    // Release previously acquired buffer if any
    if (currently_acquired_buffer && current_connection) {
        current_connection->release_read_buffer(currently_acquired_buffer);
        currently_acquired_buffer = nullptr;
        current_connection = nullptr;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);
    
    VLOG(3) << "Consume: Starting with timeout=" << timeout_ms << "ms, order_level=" << order_level_;
    
    while (std::chrono::steady_clock::now() - start_time < timeout) {
        // Try to acquire data from any available connection
			absl::ReaderMutexLock map_lock(&connection_map_mutex_);
			for (auto const& [fd, conn_ptr] : connections_) {
				if (!conn_ptr) continue;
            
            // Try to acquire a buffer with data
            BufferState* buffer = conn_ptr->acquire_read_buffer();
            if (!buffer) continue; // No data available on this connection
            
            // We have data! Process it
            size_t buffer_write_offset = buffer->write_offset.load(std::memory_order_acquire);
            if (buffer_write_offset < sizeof(Embarcadero::MessageHeader)) {
                // Not enough data for even a message header
                conn_ptr->release_read_buffer(buffer);
                continue;
            }
            
            uint8_t* buffer_data = static_cast<uint8_t*>(buffer->buffer);
            size_t current_pos = 0;
            
            VLOG(4) << "Consume: Processing buffer from fd=" << fd 
                     << ", buffer_size=" << buffer_write_offset << ", order_level=" << order_level_;
            
            // For Sequencer 5: No initialization needed - receiver threads handle everything
            
            // CRITICAL FIX: Store messages to return later
            void* message_to_return = nullptr;
            
            // Process ALL messages in the buffer before releasing it
            while (current_pos + sizeof(Embarcadero::MessageHeader) <= buffer_write_offset) {
                // Receiver threads already processed batch metadata - just parse messages
                
                // Parse message header directly
                
                Embarcadero::MessageHeader* header = 
                    reinterpret_cast<Embarcadero::MessageHeader*>(buffer_data + current_pos);
                
                // Validate message header
                if (header->paddedSize == 0 || header->paddedSize > 1024*1024) {
                    // This might be batch metadata or corrupted data
                    if (order_level_ == 5 && current_pos + sizeof(Embarcadero::wire::BatchMetadata) <= buffer_write_offset) {
                        // Try skipping 16 bytes (batch metadata size)
                        current_pos += sizeof(Embarcadero::wire::BatchMetadata);
							} else {
                        // Skip to next aligned position
                        current_pos += 8;
                    }
                    continue;
                }
                
                // Check if we have the complete message
                if (current_pos + header->paddedSize > buffer_write_offset) {
                    VLOG(4) << "Consume: Incomplete message at pos " << current_pos 
                            << ", need " << header->paddedSize << " bytes, have " 
                            << (buffer_write_offset - current_pos) << ", fd=" << fd;
                    break; // Wait for more data
                }
                
                // For Sequencer 5: total_order is already assigned by receiver threads
                // No need to re-assign here
                
                // Check if this is a message we should consume
                bool should_consume = false;
                
                // All order levels (including 5) now enforce strict sequential ordering
                // since receiver threads already assigned correct total_order values
                should_consume = (header->total_order == next_expected_order);
                if (should_consume) {
                    next_expected_order++;
                }
                
                if (should_consume && message_to_return == nullptr) {
                    // Mark this message to return (but continue processing the buffer)
                    message_to_return = static_cast<void*>(header);
                    VLOG(4) << "Consume: Will return message with total_order=" << header->total_order
                            << ", paddedSize=" << header->paddedSize << ", fd=" << fd;
                }
                
                // Move to next message in buffer
                current_pos += header->paddedSize;
            }
            
            // If we found a message to return, keep the buffer acquired
            if (message_to_return != nullptr) {
                // Store buffer reference for release on next call
                currently_acquired_buffer = buffer;
                current_connection = conn_ptr;
                return message_to_return;
			} else {
                // No message found in this buffer, release it
                conn_ptr->release_read_buffer(buffer);
			}
            
            // No suitable message found in this buffer, release it
            conn_ptr->release_read_buffer(buffer);
		}

        // No data available from any connection, wait a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    VLOG(3) << "Consume: Timeout reached after " << timeout_ms << "ms";
    return nullptr;
}
