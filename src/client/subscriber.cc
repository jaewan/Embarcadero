#include "subscriber.h"
#include "latency_stats.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "../common/wire_formats.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

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
	// Release any buffer held by Consume() for order < 2 (per-instance state)
	if (consume_acquired_buffer_ && consume_acquired_connection_) {
		consume_acquired_connection_->release_read_buffer(consume_acquired_buffer_);
		consume_acquired_buffer_ = nullptr;
		consume_acquired_connection_.reset();
	}
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
	if (shutdown_) { // Prevent double shutdown
		return;
	}
	shutdown_ = true;

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

// Helper struct to store header data for validation (supports both V1 and V2)
struct HeaderValidationData {
	size_t total_order;
	int client_id;
	size_t client_order;  // For V1: client_order, for V2: derived from batch_seq
	bool is_v2;
	size_t size;          // For V2: payload size, for V1: paddedSize
	uint64_t batch_seq;   // For V2: batch_seq, for V1: 0
};

bool Subscriber::DEBUG_check_order(int order) {
	// 1. Aggregate all message headers from all connection buffers (V1 and V2)
	std::vector<HeaderValidationData> all_headers;
	size_t total_bytes_parsed = 0;
	size_t v1_count = 0, v2_count = 0;
	
	// Aggregating message headers from all connections...
	{ // Scope for locking the connection map
		absl::ReaderMutexLock map_lock(&connection_map_mutex_);

		for (auto const& [fd, conn_ptr] : connections_) {
			if (!conn_ptr) continue;

			// Lock connection state to access buffers safely
			// NOTE: This assumes no receiver thread is actively writing during the check.
			absl::MutexLock state_lock(&conn_ptr->state_mutex);

			for (int buf_idx = 0; buf_idx < 2; ++buf_idx) {
				const auto& buffer_state = conn_ptr->buffers[buf_idx];
				size_t buffer_data_size = buffer_state.write_offset.load(std::memory_order_relaxed);
				uint8_t* buffer_start_ptr = static_cast<uint8_t*>(buffer_state.buffer);

				if (buffer_data_size == 0) continue;

				VLOG(5) << "DEBUG: Parsing FD=" << fd << ", Buffer=" << buf_idx << ", Size=" << buffer_data_size;
				size_t parse_offset = 0;
				
				// Track batch metadata state for V2 header detection
				bool has_batch_metadata = false;
				uint16_t current_header_version = 1; // Default to V1
				size_t messages_in_batch = 0;
				size_t messages_processed = 0;
				size_t current_batch_total_order = 0;
				
				while (parse_offset < buffer_data_size) {
					uint8_t* current_parse_ptr = buffer_start_ptr + parse_offset;
					size_t remaining_in_buffer = buffer_data_size - parse_offset;

					// Check for BatchMetadata (16 bytes) before message headers
					if (!has_batch_metadata && 
					    remaining_in_buffer >= sizeof(Embarcadero::wire::BatchMetadata)) {
						Embarcadero::wire::BatchMetadata* metadata = 
							reinterpret_cast<Embarcadero::wire::BatchMetadata*>(current_parse_ptr);
						
					if (Embarcadero::wire::IsValidHeaderVersion(metadata->header_version) &&
					    metadata->num_messages > 0 && 
					    metadata->num_messages <= Embarcadero::wire::MAX_BATCH_MESSAGES) {
							current_header_version = metadata->header_version;
							messages_in_batch = metadata->num_messages;
							messages_processed = 0;
							current_batch_total_order = metadata->batch_total_order;
							has_batch_metadata = true;
							parse_offset += sizeof(Embarcadero::wire::BatchMetadata);
							VLOG(5) << "DEBUG: Found batch metadata, header_version=" << current_header_version
							        << ", num_messages=" << messages_in_batch;
							continue;
						}
					}

					// Need at least 64 bytes for either header type
					if (remaining_in_buffer < sizeof(Embarcadero::MessageHeader)) {
						VLOG(5) << "DEBUG: Incomplete header at offset " << parse_offset << ", stopping parse for this buffer.";
						break;
					}

					HeaderValidationData header_data;
					size_t total_message_size = 0;
					bool valid_header = false;

					if (current_header_version == 2) {
						// [[BLOG_HEADER: Parse V2 BlogMessageHeader]]
						Embarcadero::BlogMessageHeader* v2_hdr = 
							reinterpret_cast<Embarcadero::BlogMessageHeader*>(current_parse_ptr);
						
						// Validate payload size
						if (Embarcadero::wire::ValidateV2Payload(v2_hdr->size, remaining_in_buffer)) {
							total_message_size = Embarcadero::wire::ComputeStrideV2(v2_hdr->size);
							
							if (remaining_in_buffer >= total_message_size) {
								header_data.is_v2 = true;
								if (has_batch_metadata) {
									header_data.total_order = current_batch_total_order + messages_processed;
								} else {
									header_data.total_order = v2_hdr->total_order;
								}
								header_data.client_id = static_cast<int>(v2_hdr->client_id);
								header_data.client_order = v2_hdr->batch_seq; // Use batch_seq as client_order equivalent
								header_data.size = v2_hdr->size;
								header_data.batch_seq = v2_hdr->batch_seq;
								valid_header = true;
								v2_count++;
								
								if (has_batch_metadata) {
									messages_processed++;
									if (messages_processed >= messages_in_batch) {
										has_batch_metadata = false; // Reset for next batch
									}
								}
							}
						}
					} else {
						// [[LEGACY: Parse V1 MessageHeader]]
						Embarcadero::MessageHeader* v1_hdr = 
							reinterpret_cast<Embarcadero::MessageHeader*>(current_parse_ptr);
						
					// Validate paddedSize
					if (Embarcadero::wire::ValidateV1PaddedSize(v1_hdr->paddedSize, remaining_in_buffer)) {
							total_message_size = v1_hdr->paddedSize;
							
							if (remaining_in_buffer >= total_message_size) {
								header_data.is_v2 = false;
								header_data.total_order = v1_hdr->total_order;
								header_data.client_id = v1_hdr->client_id;
								header_data.client_order = v1_hdr->client_order;
								header_data.size = total_message_size;
								header_data.batch_seq = 0;
								valid_header = true;
								v1_count++;
							}
						}
					}

					if (!valid_header || total_message_size == 0) {
						VLOG(5) << "DEBUG: Invalid or incomplete message at offset " << parse_offset 
						        << ", stopping parse for this buffer.";
						break;
					}

					// Store header data
					all_headers.push_back(header_data);
					total_bytes_parsed += total_message_size;

					// Advance parse_offset
					parse_offset += total_message_size;
				} // End while(parse_offset < buffer_data_size)
			} // End for buf_idx
		} // End for connections
	} // Release connection map lock
	
	LOG(INFO) << "DEBUG_check_order: Parsed " << all_headers.size() << " messages "
	          << "(V1=" << v1_count << ", V2=" << v2_count << "), "
	          << total_bytes_parsed << " total bytes";

	if (all_headers.empty()) {
		LOG(WARNING) << "DEBUG_check_order: No message headers found to validate";
		return true; // No messages to check
	}

	// Deduplicate headers to avoid reprocessing duplicates across buffers
	VLOG(3) << "DEBUG: Deduplicating headers before validation...";
	std::set<std::tuple<int, size_t, uint64_t>> seen_messages;
	auto dedup_it = std::remove_if(all_headers.begin(), all_headers.end(),
		[&seen_messages](const auto& hdr) {
			auto key = std::make_tuple(hdr.client_id, hdr.total_order, hdr.batch_seq);
			if (seen_messages.find(key) != seen_messages.end()) {
				VLOG(5) << "DEBUG: Removing duplicate message: client_id=" << hdr.client_id
				        << ", total_order=" << hdr.total_order << ", batch_seq=" << hdr.batch_seq;
				return true;
			}
			seen_messages.insert(key);
			return false;
		});
	all_headers.erase(dedup_it, all_headers.end());
	VLOG(3) << "DEBUG: After deduplication: " << all_headers.size() << " unique messages";

	bool overall_status = true;

	// 2. Order Level 0 Check: Basic header validity
	VLOG(3) << "DEBUG: --- Checking Order Level 0 (Header Validity) ---";
	for (const auto& hdr : all_headers) {
		if (hdr.is_v2) {
			// V2 validation: size should be within bounds
			if (hdr.size == 0 || hdr.size > Embarcadero::wire::MAX_MESSAGE_PAYLOAD_SIZE) {
				LOG(ERROR) << "DEBUG Check Failed (Level 0): Invalid V2 payload size=" << hdr.size;
				overall_status = false;
			}
		} else {
			// V1 validation: size should be reasonable
			if (hdr.size == 0 || hdr.size > Embarcadero::wire::MaxV1PaddedSize()) {
				LOG(ERROR) << "DEBUG Check Failed (Level 0): Invalid V1 paddedSize=" << hdr.size;
				overall_status = false;
			}
		}
	}
	if (order == 0) {
		LOG(INFO) << "DEBUG: Order Level 0 check " << (overall_status ? "PASSED" : "FAILED");
		return overall_status;
	}

	// 3. Sort by Total Order for subsequent checks
	VLOG(3) << "DEBUG: Sorting headers by total_order...";
	std::sort(all_headers.begin(), all_headers.end(), [](const auto& a, const auto& b) {
		return a.total_order < b.total_order;
	});
	VLOG(3) << "DEBUG: Sorting complete.";

	// 5. Order Level 1 Check: Total Order assignment, uniqueness, contiguity
	VLOG(3) << "DEBUG: --- Checking Order Level 1 (Total Order Assignment, Uniqueness, Contiguity) ---";
	std::set<size_t> total_orders_seen;
	bool contiguity_ok = true;
	bool uniqueness_ok = true;

	if (all_headers.empty()) {
		return overall_status;
	}

	// Check first element
	if (all_headers[0].total_order != 0) {
		VLOG(3) << "DEBUG Check (Level 1): First total_order is " << all_headers[0].total_order 
		        << " (expected 0 if sequence starts at 0).";
	}

	for (size_t i = 0; i < all_headers.size(); ++i) {
		const auto& hdr = all_headers[i];
		size_t current_total_order = hdr.total_order;

		// Check Uniqueness
		if (!total_orders_seen.insert(current_total_order).second) {
			LOG(ERROR) << "DEBUG Check Failed (Level 1): Duplicate total_order=" << current_total_order
			           << " found (client_id=" << hdr.client_id 
			           << ", client_order=" << hdr.client_order
			           << ", is_v2=" << hdr.is_v2 << ")";
			uniqueness_ok = false;
			overall_status = false;
		}

		// Check Contiguity (for non-batch ordering)
		if (i > 0 && order < 5) { // Skip contiguity check for ORDER=5 (batch-level)
			size_t prev_total_order = all_headers[i-1].total_order;
			if (current_total_order > prev_total_order + 1) {
				LOG(ERROR) << "DEBUG Check Failed (Level 1): Hole detected in total_order sequence. "
				           << "Current=" << current_total_order << ", Previous=" << prev_total_order
				           << " (client_id=" << hdr.client_id << ", client_order=" << hdr.client_order << ")";
				contiguity_ok = false;
				overall_status = false;
			}
		}
	}
	
	if (!uniqueness_ok || !contiguity_ok) {
		VLOG(3) << "DEBUG: Order Level 1 check FAILED (Uniqueness=" << uniqueness_ok
		        << ", Contiguity=" << contiguity_ok << ")";
	} else {
		VLOG(3) << "DEBUG: Order Level 1 check passed.";
	}

	if (order == 1) {
		LOG(INFO) << "DEBUG: Order Level 1 check " << (overall_status ? "PASSED" : "FAILED");
		return overall_status;
	}

	// 6. Order Level >= 2 Check: Client Order Preservation
	// [[ORDER=5: Batch-level ordering]] For ORDER=5, skip client_order checks (batch-level, total_order only).
	// [[ORDER=2: Total order only]] For ORDER=2, skip client_order checks (design: no per-client guarantee).
	// For ORDER 0,1,3,4: Check per-message client_order preservation
	if (order < 5 && order != 2) {
		VLOG(3) << "DEBUG: --- Checking Order Level >= 2 (Client Order Preservation) ---";
		std::map<int, size_t> last_client_order_for_client;
		bool client_order_preserved = true;

		for (const auto& hdr : all_headers) {
			int client_id = hdr.client_id;
			size_t client_order = hdr.client_order;

			auto it = last_client_order_for_client.find(client_id);
			if (it != last_client_order_for_client.end()) {
				// Client seen before, check order
				if (client_order < it->second) {
					LOG(ERROR) << "DEBUG Check Failed (Level >=2): Client order violation for client_id=" << client_id
					           << ". Current msg (total_order=" << hdr.total_order 
					           << ", client_order=" << client_order
					           << ", is_v2=" << hdr.is_v2
					           << ") has smaller client_order than previous msg (client_order=" << it->second << ").";
					client_order_preserved = false;
					overall_status = false;
				}
				it->second = client_order;
			} else {
				// First time seeing this client
				last_client_order_for_client[client_id] = client_order;
			}
		}
		if (!client_order_preserved) {
			VLOG(3) << "DEBUG: Order Level >= 2 check FAILED.";
		} else {
			VLOG(3) << "DEBUG: Order Level >= 2 check passed.";
		}
	} else {
		// ORDER=5: Batch-level ordering - skip per-message client_order checks
		VLOG(3) << "DEBUG: --- Skipping Order Level >= 2 (Client Order Preservation) for ORDER=5 (batch-level ordering) ---";
		VLOG(3) << "DEBUG: ORDER=5 uses batch-level ordering; total_order uniqueness already validated in Level 1.";
	}

	LOG(INFO) << "DEBUG: Order Level " << order << " check " << (overall_status ? "PASSED" : "FAILED");
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

					// 4. Find exact receive time from recv_log for this buffer generation.
					// We parse the final resident bytes in this buffer, so use its current generation.
					const uint64_t target_generation = conn_ptr->buffer_generation[buf_idx];
					const RecvLogEntry* matched_entry = nullptr;
					for (const auto& log_entry : recv_log) {
						if (log_entry.buffer_idx != buf_idx) continue;
						if (log_entry.buffer_generation != target_generation) continue;
						if (log_entry.end_offset >= message_end_offset_in_buffer) {
							matched_entry = &log_entry;
							break;
						}
					}

					// 5. Calculate Latency
					if (matched_entry != nullptr) {
						auto latency_duration = matched_entry->receive_time - send_time;
						long long latency_micros = std::chrono::duration_cast<std::chrono::microseconds>(latency_duration).count();
						all_latencies_us.push_back(latency_micros);
					}

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
	if (all_latencies_us.size() != total_messages_parsed) {
		LOG(WARNING) << "Latency sample mismatch: parsed=" << total_messages_parsed
		             << " mapped_samples=" << all_latencies_us.size()
		             << ". Some messages could not be correlated to recv-log entries.";
	}

	// --- Calculate Statistics ---
	// Sort once and use a shared percentile policy with publisher stats.
	std::sort(all_latencies_us.begin(), all_latencies_us.end());
	const auto summary = Embarcadero::LatencyStats::ComputeSummary(all_latencies_us);

	// --- Log Results ---
	LOG(INFO) << "Publish->Deliver Latency Statistics (us):";
	LOG(INFO) << "  Count:   " << summary.count;
	LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << summary.average_us;
	LOG(INFO) << "  Min:     " << summary.min_us;
	LOG(INFO) << "  P50:     " << summary.p50_us;
	LOG(INFO) << "  P99:     " << summary.p99_us;
	LOG(INFO) << "  P99.9:   " << summary.p999_us;
	LOG(INFO) << "  Max:     " << summary.max_us;

	std::string latency_filename = "latency_stats.csv";
	std::ofstream latency_file(latency_filename);
	if (!latency_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << latency_filename;
	} else {
		latency_file << "Average,Min,Median,p90,p95,p99,p999,Max,Count,Metric,Unit,PercentileMethod,Granularity\n";
		latency_file << std::fixed << std::setprecision(3) << summary.average_us
			<< "," << summary.min_us
			<< "," << summary.p50_us
			<< "," << summary.p90_us
			<< "," << summary.p95_us
			<< "," << summary.p99_us
			<< "," << summary.p999_us
			<< "," << summary.max_us
			<< "," << summary.count
			<< ",publish_to_deliver_latency"
			<< ",us"
			<< "," << Embarcadero::LatencyStats::kPercentileMethod
			<< ",message\n";
		latency_file.close();
	}

	// --- Generate and Write CDF Data Points ---
	std::string cdf_filename = "cdf_latency_us.csv"; // Use .csv for easy import
	VLOG(3) << "Writing CDF data points to " << cdf_filename;
	std::ofstream cdf_file(cdf_filename);
	if (!cdf_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << cdf_filename;
	} else {
		cdf_file << "Latency_us,CumulativeProbability,Metric\n"; // CSV Header

		// Iterate through the SORTED latencies
		for (size_t i = 0; i < summary.count; ++i) {
			long long current_latency = all_latencies_us[i];
			// Cumulative probability = (number of points <= current_latency) / total_points
			// Since it's sorted, this is (index + 1) / count
			double cumulative_probability = static_cast<double>(i + 1) / summary.count;

			cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability
			         << ",publish_to_deliver_latency\n";
		}
		cdf_file.close();
	}
}

void Subscriber::Poll(size_t total_msg_size, size_t msg_size) {
	VLOG(5) << "Waiting to receive " << total_msg_size << " bytes of data with message size " << msg_size;

	// Calculate expected total data size based on padded message size
	const size_t num_msg = total_msg_size / msg_size;
	const size_t padded_msg_size = ((msg_size + 64 - 1) / 64) * 64;
	size_t total_data_size = num_msg * (sizeof(Embarcadero::MessageHeader) + padded_msg_size);

	VLOG(5) << "Subscriber::Poll - Expected: " << total_data_size << " bytes (" << num_msg << " messages), "
	          << "padded_msg_size=" << padded_msg_size << ", header_size=" << sizeof(Embarcadero::MessageHeader);

	constexpr int POLL_TIMEOUT_SEC = 15;  // 15s to allow full drain for Order 0 subscribe (10GB+)
	constexpr int PROGRESS_LOG_INTERVAL_SEC = 5;  // Progress log interval during Poll
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(POLL_TIMEOUT_SEC);
	auto last_progress_log = std::chrono::steady_clock::now();

	// Reduce busy-wait overhead with adaptive sleeping
	while (DEBUG_count_ < total_data_size) {
		size_t current_count = DEBUG_count_.load(std::memory_order_relaxed);
		if (current_count >= total_data_size) break;

		auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			double pct = total_data_size > 0 ? (100.0 * static_cast<double>(current_count)) / static_cast<double>(total_data_size) : 0;
			LOG(ERROR) << "Subscriber::Poll timeout after " << POLL_TIMEOUT_SEC << "s: received "
			           << current_count << " / " << total_data_size << " bytes (" << std::fixed << std::setprecision(1) << pct << "%)";
			// Log per-broker bytes to identify which broker(s) stopped sending (no broker logs needed)
			std::ostringstream broker_breakdown;
			broker_breakdown << "Per-broker bytes at timeout: [";
			for (size_t i = 0; i < per_broker_bytes_.size(); ++i) {
				size_t b = per_broker_bytes_[i].load(std::memory_order_relaxed);
				if (i > 0) broker_breakdown << " ";
				broker_breakdown << "B" << i << "=" << b;
			}
			broker_breakdown << "]";
			LOG(ERROR) << broker_breakdown.str();
			throw std::runtime_error("Subscriber poll timeout");
		}
		// Progress logging during long Poll (e.g. large E2E test)
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_log).count() >= PROGRESS_LOG_INTERVAL_SEC) {
			last_progress_log = now;
			double pct = total_data_size > 0 ? (100.0 * static_cast<double>(current_count)) / static_cast<double>(total_data_size) : 0;
			int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count());
			std::ostringstream broker_stats;
			broker_stats << "Per-broker bytes: [";
			for (size_t i = 0; i < per_broker_bytes_.size(); ++i) {
				size_t b = per_broker_bytes_[i].load(std::memory_order_relaxed);
				if (b > 0 || i < 4) {
					broker_stats << "B" << i << "=" << b << " ";
				}
			}
			broker_stats << "]";
			VLOG(1) << "Subscriber::Poll progress: " << current_count << " / " << total_data_size << " bytes ("
			        << std::fixed << std::setprecision(1) << pct << "%), " << elapsed << "s until timeout. " << broker_stats.str();
		}

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
					if (shutdown_) break;
					while (! (shutdown_ ||
								!conn_buffers->read_buffer_in_use_by_consumer.load(std::memory_order_acquire)) )
					{
						conn_buffers->receiver_can_write_cv.Wait(&conn_buffers->state_mutex);
					}
				}
				VLOG(4) << "Worker (fd=" << conn_buffers->fd << "): Wait loop finished.";
				if (shutdown_) break;
				VLOG(4) << "Worker (fd=" << conn_buffers->fd << "): Consumer released buffer, continuing loop.";
				continue;
			}
		}

		// Use 1MB receive chunks for optimal performance
		size_t recv_chunk_size = std::min(available_space, static_cast<size_t>(1UL << 20));
		ssize_t bytes_received = recv(conn_buffers->fd, write_ptr, recv_chunk_size, 0);

		if (bytes_received > 0) {
			// [[BLOG_HEADER: Process ORDER=2/5 batch metadata in receiver thread]]
			// Order 2 (total order) and Order 5 (strong order) both send batch metadata; assign total_order per message
			if (order_level_ == 5 || order_level_ == 2) {
				ProcessSequencer5Data(static_cast<uint8_t*>(write_ptr), bytes_received, conn_buffers);
			}
			
			// 4. Advance write offset (BEFORE getting timestamp)
			conn_buffers->advance_write_offset(bytes_received);
			// 5. Record Timestamp and NEW Offset
			if (measure_latency_) {
				absl::MutexLock lock(&conn_buffers->state_mutex);
				auto recv_complete_time = std::chrono::steady_clock::now();
				const int write_idx = conn_buffers->current_write_idx.load(std::memory_order_relaxed);
				size_t current_end_offset = conn_buffers->buffers[write_idx].write_offset.load(std::memory_order_relaxed);
				const uint64_t generation = conn_buffers->buffer_generation[write_idx];
				conn_buffers->recv_log.push_back(RecvLogEntry{
					.receive_time = recv_complete_time,
					.buffer_idx = write_idx,
					.buffer_generation = generation,
					.end_offset = current_end_offset
				});
			}

			DEBUG_count_.fetch_add(bytes_received, std::memory_order_relaxed);
			if (broker_id >= 0 && static_cast<size_t>(broker_id) < per_broker_bytes_.size()) {
				per_broker_bytes_[broker_id].fetch_add(static_cast<size_t>(bytes_received), std::memory_order_relaxed);
			}
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
			if (shutdown_) { /* log shutdown */ }
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
	// Wait for cluster info before connecting to brokers
	// to determine which brokers accept subscriptions (same as publishers)

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

			// Use broker_info if available (includes accepts_publishes)
			// Skip brokers that do not accept subscriptions
			if (cluster_status.broker_info_size() > 0) {
				std::vector<std::pair<int, std::string>> brokers_to_add;
				size_t data_brokers = 0;  // Count data brokers for WaitUntilAllConnected
				{ // Lock scope
					absl::MutexLock lock(&node_mutex_);
					for (const auto& bi : cluster_status.broker_info()) {
						int broker_id = bi.broker_id();
						std::string addr = bi.network_mgr_addr();
						if (!bi.accepts_publishes()) {
							LOG(INFO) << "SubscribeToCluster: Skipping broker " << broker_id
							          << " (no data)";
							continue;
						}
						data_brokers++;
						nodes_[broker_id] = addr;  // Always update (e.g. broker 0 pre-set in ctor)
						// Spawn ManageBrokerConnections for every data broker we haven't started yet (including broker 0)
						if (brokers_connection_started_.insert(broker_id).second) {
							brokers_to_add.push_back({broker_id, addr});
							LOG(INFO) << "SubscribeToCluster: Added data broker " << broker_id;
						}
					}
				} // Lock released
				// Update data broker count for WaitUntilAllConnected
				data_broker_count_.store(data_brokers);
				for(const auto& pair : brokers_to_add) {
					std::thread manager_thread(&Subscriber::ManageBrokerConnections, this, pair.first, pair.second);
					manager_thread.detach();
				}
			} else {
				// Fallback to legacy new_nodes API (no filtering)
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
		// Offset resets to 0 on a reused buffer; advance generation so recv-log
		// correlation can distinguish this reuse epoch.
		buffer_generation[read_idx] += 1;
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
	// Copy batch state in/out so we hold state_mutex only at start and end (reduces contention with consumer/swap).
	ConnectionBuffers::BatchMetadataState batch_state;
	{
		absl::MutexLock batch_lock(&conn_buffers->state_mutex);
		batch_state = conn_buffers->batch_metadata;
	}

	size_t current_pos = 0;
	int fd = conn_buffers->fd;

	VLOG(5) << "ProcessSequencer5Data: Processing " << data_size << " bytes for fd=" << fd;

	while (current_pos < data_size) {
		if (!batch_state.has_pending_metadata &&
		    current_pos + sizeof(Embarcadero::wire::BatchMetadata) <= data_size) {

			Embarcadero::wire::BatchMetadata* potential_metadata =
				reinterpret_cast<Embarcadero::wire::BatchMetadata*>(data + current_pos);

			if (Embarcadero::wire::IsValidHeaderVersion(potential_metadata->header_version) &&
			    potential_metadata->num_messages > 0 &&
			    potential_metadata->num_messages <= Embarcadero::wire::MAX_BATCH_MESSAGES &&
			    potential_metadata->batch_total_order < Embarcadero::wire::MAX_BATCH_TOTAL_ORDER) {

				batch_state.pending_metadata = *potential_metadata;
				batch_state.has_pending_metadata = true;
				batch_state.current_batch_messages_processed = 0;
				batch_state.next_message_order_in_batch = potential_metadata->batch_total_order;

				VLOG(3) << "ProcessSequencer5Data: Found batch metadata, total_order="
				        << potential_metadata->batch_total_order << ", num_messages="
				        << potential_metadata->num_messages << ", fd=" << fd;

				current_pos += sizeof(Embarcadero::wire::BatchMetadata);
				continue;
			}
		}

		if (current_pos + sizeof(Embarcadero::MessageHeader) <= data_size) {
			void* msg_ptr = data + current_pos;
			bool is_v2_header = (batch_state.has_pending_metadata &&
			                    batch_state.pending_metadata.header_version == 2);

			if (is_v2_header) {
				Embarcadero::BlogMessageHeader* v2_hdr =
					reinterpret_cast<Embarcadero::BlogMessageHeader*>(msg_ptr);
				size_t payload_size = v2_hdr->size;
				size_t total_msg_size = Embarcadero::wire::ComputeStrideV2(payload_size);

				if (Embarcadero::wire::ValidateV2Payload(payload_size, data_size - current_pos)) {
					if (batch_state.has_pending_metadata) {
						if (v2_hdr->total_order == 0) {
							v2_hdr->total_order = batch_state.next_message_order_in_batch++;
							batch_state.current_batch_messages_processed++;
						} else {
							batch_state.current_batch_messages_processed++;
						}
						if (batch_state.current_batch_messages_processed >=
						    batch_state.pending_metadata.num_messages) {
							batch_state.has_pending_metadata = false;
						}
					}
					current_pos += total_msg_size;
				} else {
					current_pos += 64;
				}
			} else {
				Embarcadero::MessageHeader* v1_hdr =
					reinterpret_cast<Embarcadero::MessageHeader*>(msg_ptr);

				if (Embarcadero::wire::ValidateV1PaddedSize(v1_hdr->paddedSize, data_size - current_pos)) {
					if (batch_state.has_pending_metadata) {
						if (v1_hdr->total_order == 0) {
							v1_hdr->total_order = batch_state.next_message_order_in_batch++;
							batch_state.current_batch_messages_processed++;
						} else if (batch_state.current_batch_messages_processed <
						           batch_state.pending_metadata.num_messages) {
							batch_state.current_batch_messages_processed++;
						}
						if (batch_state.current_batch_messages_processed >=
						    batch_state.pending_metadata.num_messages) {
							batch_state.has_pending_metadata = false;
						}
					}
					current_pos += v1_hdr->paddedSize;
				} else {
					current_pos += 64;
				}
			}
		} else {
			break;
		}
	}

	{
		absl::MutexLock batch_lock(&conn_buffers->state_mutex);
		conn_buffers->batch_metadata = batch_state;
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
                        const size_t remaining_bytes = write_offset - parse_offset;
                        if (!Embarcadero::wire::ValidateV2Payload(v2_hdr->size, remaining_bytes)) {
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
                        msg_total_size = Embarcadero::wire::ComputeStrideV2(v2_hdr->size);
                    } else {
                        // [[LEGACY: Parse v1 header]]
                        if (parse_offset + sizeof(Embarcadero::MessageHeader) > write_offset) {
                            break;  // Incomplete message header
                        }
                        
                        Embarcadero::MessageHeader* v1_hdr = 
                            reinterpret_cast<Embarcadero::MessageHeader*>(
                                static_cast<uint8_t*>(buffer_start) + parse_offset);
                        
                        // Validate v1 paddedSize
                        const size_t remaining_bytes = write_offset - parse_offset;
                        if (!Embarcadero::wire::ValidateV1PaddedSize(v1_hdr->paddedSize, remaining_bytes)) {
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
// [[FIX: Multi-subscriber]] Uses per-instance state (next_expected_order_weak_, consume_acquired_*)
// so multiple Subscriber instances or threads don't share one ordering state or buffer.
void* Subscriber::Consume(int timeout_ms) {
    if (order_level_ >= 2) {
        return ConsumeOrdered(timeout_ms);
    }

    // Release previously acquired buffer if any (per-instance state)
    if (consume_acquired_buffer_ && consume_acquired_connection_) {
        consume_acquired_connection_->release_read_buffer(consume_acquired_buffer_);
        consume_acquired_buffer_ = nullptr;
        consume_acquired_connection_.reset();
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
                const size_t remaining_bytes = buffer_write_offset - current_pos;
                if (!Embarcadero::wire::ValidateV1PaddedSize(header->paddedSize, remaining_bytes)) {
                    // This might be batch metadata or corrupted data
                    if ((order_level_ == 5 || order_level_ == 2) && current_pos + sizeof(Embarcadero::wire::BatchMetadata) <= buffer_write_offset) {
                        // Try skipping 16 bytes (batch metadata size)
                        current_pos += sizeof(Embarcadero::wire::BatchMetadata);
							} else {
                        // Skip to next aligned position
                        current_pos += 8;
                    }
                    continue;
                }
                
                // Check if we have the complete message
                if (header->paddedSize > remaining_bytes) {
                    VLOG(4) << "Consume: Incomplete message at pos " << current_pos 
                            << ", need " << header->paddedSize << " bytes, have " 
                            << remaining_bytes << ", fd=" << fd;
                    break; // Wait for more data
                }
                
                // For Sequencer 5: total_order is already assigned by receiver threads
                // No need to re-assign here
                
                // Check if this is a message we should consume
                bool should_consume = false;
                
                // All order levels (including 5) now enforce strict sequential ordering
                // since receiver threads already assigned correct total_order values
                should_consume = (header->total_order == next_expected_order_weak_);
                if (should_consume) {
                    next_expected_order_weak_++;
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
            
            // If we found a message to return, keep the buffer acquired (per-instance state)
            if (message_to_return != nullptr) {
                consume_acquired_buffer_ = buffer;
                consume_acquired_connection_ = conn_ptr;
                return message_to_return;
            }
            // No message found in this buffer, release it (single release only)
            conn_ptr->release_read_buffer(buffer);
		}

        // No data available from any connection, wait a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    VLOG(3) << "Consume: Timeout reached after " << timeout_ms << "ms";
    return nullptr;
}

void* Subscriber::ConsumeOrdered(int timeout_ms) {
	auto start_time = std::chrono::steady_clock::now();
	auto timeout = std::chrono::milliseconds(timeout_ms);

	// Release previously returned owned message (valid until next Consume call)
	last_returned_.reset();

	auto parse_stream = [&](StreamParseState& state) {
		constexpr size_t kMaxStreamBufferBytes = 64UL << 20; // 64 MB safety cap
		if (state.buffer.size() > kMaxStreamBufferBytes) {
			LOG(WARNING) << "ConsumeOrdered: stream buffer exceeded cap, dropping "
			             << state.buffer.size() << " bytes";
			state.buffer.clear();
			state.has_pending_metadata = false;
			state.current_batch_messages_processed = 0;
			state.next_message_order_in_batch = 0;
			return;
		}

		size_t pos = 0;
		const size_t buf_size = state.buffer.size();
		while (pos < buf_size) {
			if (!state.has_pending_metadata) {
				if (buf_size - pos < sizeof(Embarcadero::wire::BatchMetadata)) {
					break;
				}
				const auto* meta = reinterpret_cast<const Embarcadero::wire::BatchMetadata*>(
					state.buffer.data() + pos);
				if (Embarcadero::wire::IsValidHeaderVersion(meta->header_version) &&
				    meta->num_messages > 0 &&
				    meta->num_messages <= Embarcadero::wire::MAX_BATCH_MESSAGES &&
				    meta->batch_total_order < Embarcadero::wire::MAX_BATCH_TOTAL_ORDER) {
					state.pending_metadata = *meta;
					state.has_pending_metadata = true;
					state.current_batch_messages_processed = 0;
					state.next_message_order_in_batch = meta->batch_total_order;
					pos += sizeof(Embarcadero::wire::BatchMetadata);
					continue;
				}
				// Resync: advance by 8 bytes to find next metadata boundary
				pos += 8;
				continue;
			}

			const uint16_t header_version = state.pending_metadata.header_version;
			if (header_version == Embarcadero::wire::HEADER_VERSION_V2) {
				if (buf_size - pos < sizeof(Embarcadero::BlogMessageHeader)) {
					break;
				}
				const auto* hdr = reinterpret_cast<const Embarcadero::BlogMessageHeader*>(
					state.buffer.data() + pos);
				const size_t payload_size = hdr->size;
				if (!Embarcadero::wire::ValidateV2Payload(payload_size, buf_size - pos)) {
					// Resync on invalid header
					pos += 8;
					continue;
				}
				const size_t stride = Embarcadero::wire::ComputeStrideV2(payload_size);
				if (buf_size - pos < stride) {
					break;
				}

				auto msg = std::make_unique<OwnedMessage>();
				msg->header_version = header_version;
				msg->data.assign(state.buffer.begin() + pos, state.buffer.begin() + pos + stride);
				auto* msg_hdr = reinterpret_cast<Embarcadero::BlogMessageHeader*>(msg->data.data());
				if (msg_hdr->total_order == 0) {
					msg_hdr->total_order = state.next_message_order_in_batch;
				}
				size_t total_order = msg_hdr->total_order;
				state.next_message_order_in_batch = total_order + 1;
				state.current_batch_messages_processed++;
				pending_messages_.try_emplace(total_order, std::move(msg));

				if (state.current_batch_messages_processed >= state.pending_metadata.num_messages) {
					state.has_pending_metadata = false;
				}
				pos += stride;
				continue;
			}

			// Header version v1 (MessageHeader)
			if (buf_size - pos < sizeof(Embarcadero::MessageHeader)) {
				break;
			}
			const auto* hdr = reinterpret_cast<const Embarcadero::MessageHeader*>(
				state.buffer.data() + pos);
			const size_t padded_size = hdr->paddedSize;
			if (!Embarcadero::wire::ValidateV1PaddedSize(padded_size, buf_size - pos)) {
				pos += 8;
				continue;
			}
			if (buf_size - pos < padded_size) {
				break;
			}

			auto msg = std::make_unique<OwnedMessage>();
			msg->header_version = header_version;
			msg->data.assign(state.buffer.begin() + pos, state.buffer.begin() + pos + padded_size);
			auto* msg_hdr = reinterpret_cast<Embarcadero::MessageHeader*>(msg->data.data());
			if (msg_hdr->total_order == 0) {
				msg_hdr->total_order = state.next_message_order_in_batch;
			}
			size_t total_order = msg_hdr->total_order;
			state.next_message_order_in_batch = total_order + 1;
			state.current_batch_messages_processed++;
			pending_messages_.try_emplace(total_order, std::move(msg));

			if (state.current_batch_messages_processed >= state.pending_metadata.num_messages) {
				state.has_pending_metadata = false;
			}
			pos += padded_size;
		}

		if (pos > 0) {
			state.buffer.erase(state.buffer.begin(), state.buffer.begin() + pos);
		}
	};

	while (std::chrono::steady_clock::now() - start_time < timeout) {
		// Pull any available buffers into per-connection stream buffers
		{
			absl::ReaderMutexLock map_lock(&connection_map_mutex_);
			for (auto const& [fd, conn_ptr] : connections_) {
				if (!conn_ptr) continue;
				BufferState* buffer = conn_ptr->acquire_read_buffer();
				if (!buffer) continue;
				size_t buffer_write_offset = buffer->write_offset.load(std::memory_order_acquire);
				if (buffer_write_offset > 0) {
					auto& state = parse_states_[fd];
					uint8_t* data = static_cast<uint8_t*>(buffer->buffer);
					state.buffer.insert(state.buffer.end(), data, data + buffer_write_offset);
				}
				conn_ptr->release_read_buffer(buffer);
			}
		}

		// Parse available data
		for (auto& kv : parse_states_) {
			parse_stream(kv.second);
		}

		// Return next expected message if ready
		auto it = pending_messages_.find(next_expected_order_);
		if (it != pending_messages_.end()) {
			last_returned_ = std::move(it->second);
			pending_messages_.erase(it);
			void* ret = last_returned_ ? static_cast<void*>(last_returned_->data.data()) : nullptr;
			if (ret) {
				next_expected_order_++;
				return ret;
			}
		}

		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}

	VLOG(3) << "ConsumeOrdered: Timeout reached after " << timeout_ms << "ms";
	return nullptr;
}
