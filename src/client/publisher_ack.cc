
#include "common/env_flags.h"
#include "common/stage_trace.h"
#include <array>
#include <iomanip>
#include "publisher.h"
#include "publisher_profile.h"
#include "latency_stats.h"
#include "common/config.h"
#include "common/order_level.h"
#include "common/scoped_fd.h"
#include "common/fault_injection.h"
#include "network_manager/protocol.h"
#include "session.pb.h"
#include "absl/container/flat_hash_map.h"
#include <cstring>
#include <random>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <limits>
#include <mutex>
#include <condition_variable>
#include <netdb.h>
#include <numeric>
#include <set>
#include <stdexcept>



#include "publisher_internal.h"

using namespace Embarcadero::client::detail;

// ACK transport/normalization and optional ACK latency accounting.
namespace {
constexpr int kAckPortMin = 10000;
constexpr int kAckPortMax = 65535;
constexpr int kAckPortRange = kAckPortMax - kAckPortMin + 1;
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
	} else {
		int actual = 0;
		socklen_t len = sizeof(actual);
		if (getsockopt(server_sock, SOL_SOCKET, SO_SNDBUF, &actual, &len) == 0 &&
		    actual < buffer_size) {
			LOG(WARNING) << "SO_SNDBUF capped: requested " << buffer_size << " got " << actual
			             << ". Raise net.core.wmem_max (e.g. scripts/tune_kernel_buffers.sh)";
		}
	}
	if (setsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_RCVBUF) failed: " << strerror(errno);
		// Non-fatal, continue
	} else {
		int actual = 0;
		socklen_t len = sizeof(actual);
		if (getsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &actual, &len) == 0 &&
		    actual < buffer_size) {
			LOG(WARNING) << "SO_RCVBUF capped: requested " << buffer_size << " got " << actual
			             << ". Raise net.core.rmem_max (e.g. scripts/tune_kernel_buffers.sh)";
		}
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
	// [[P2.2]] flat_hash_map for O(1) lookup and better cache locality than std::map.
	absl::flat_hash_map<int, int> client_sockets; // fd -> broker_id

	// Map to track the last received cumulative ACK per socket for calculating increments
	// Initializing with -1 assumes ACK IDs (logical_offset) start >= 0.
	// The first calculation becomes ack - (size_t)-1 which equals ack + 1.
	absl::flat_hash_map<int, size_t> prev_ack_per_sock;

	// Track state for reading initial broker ID
	enum class ConnState { WAITING_FOR_ID, READING_ACKS };
	absl::flat_hash_map<int, ConnState> socket_state;
	absl::flat_hash_map<int, std::pair<int, size_t>> partial_id_reads; // fd -> {partial_id, bytes_read}
	// Buffer partial ACK reads (size_t) so we don't discard bytes when recv returns < 8 bytes
	absl::flat_hash_map<int, std::pair<size_t, size_t>> partial_ack_reads; // fd -> {ack_buffer, bytes_read}

thread_count_.fetch_add(1, std::memory_order_release);  // Signal that epoll loop is ready; Init loads with acquire

// Main epoll loop
while (!shutdown_.load(std::memory_order_relaxed)) {
	auto ack_epoll_start = std::chrono::steady_clock::now();
	int num_events = epoll_wait(epoll_fd, events.data(), max_events, EPOLL_TIMEOUT_MS);
	if (ShouldEnableNetworkPathProfile()) {
		auto& net_profile = GetClientNetworkPathProfile();
		net_profile.ack_epoll_calls.fetch_add(1, std::memory_order_relaxed);
		net_profile.ack_epoll_wait_ns.fetch_add(NsSince(ack_epoll_start), std::memory_order_relaxed);
		if (num_events == 0) {
			net_profile.ack_epoll_timeouts.fetch_add(1, std::memory_order_relaxed);
		}
	}

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

			// [[FIX: TCP_QUICKACK on accepted ACK socket]] Re-arm so broker’s TCP ACKs are sent
			// immediately after each send, not delayed 40ms by delayed-ACK timer.
			{
				int one = 1;
				if (setsockopt(client_sock, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one)) < 0) {
					LOG(WARNING) << "setsockopt(TCP_QUICKACK) on accepted ACK socket failed: " << strerror(errno);
				}
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
				// [[EPOLLET_RACE_FIX]] With EPOLLET, data that arrives *before* EPOLL_CTL_ADD never
				// triggers EPOLLIN. On loopback (B0, same machine) the broker can send broker_id
				// before we register the fd � socket stuck in WAITING_FOR_ID forever � B0=0 ACKs.
				// Drain any data already present by processing this fd immediately (same path as below).
				current_fd = client_sock;
				goto process_client_fd;
			}
		} else {
			// Handle data from existing connection (also reached via goto from EPOLLET race fix)
process_client_fd:;
			int client_sock = current_fd;
			// [[DEFENSIVE]] Stale event for fd we already closed (e.g. EPOLL_CTL_DEL race).
			if (!client_sockets.contains(client_sock)) {
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
						if (broker_id_buffer < 0 || broker_id_buffer >= (int)broker_stats_.size()) {
							LOG(ERROR) << "AckThread: Received invalid broker_id " << broker_id_buffer << " on fd " << client_sock;
							connection_error_or_closed = true; break; // Invalid ID, close connection
						}
						client_sockets[client_sock] = broker_id_buffer; // Update map value
						socket_state[client_sock] = ConnState::READING_ACKS; // Transition state
						current_state = ConnState::READING_ACKS; // Update local state for this loop
						// [[FIX: B3=0 ACKs]] Track that this broker has an ACK connection
						{
							absl::MutexLock lock(&mutex_);
							brokers_with_ack_connection_.insert(broker_id_buffer);
							VLOG(1) << "AckThread: Broker " << broker_id_buffer << " ACK connection tracked. "
							        << "Total ACK connections: " << brokers_with_ack_connection_.size()
							        << " / expected: " << expected_ack_brokers_.load(std::memory_order_relaxed);
						}
							// Clear partial read state for this FD
							partial_id_reads.erase(client_sock);
							partial_ack_reads[client_sock] = {0, 0}; // Init ACK read buffer for this connection
							if (IsOrder5SessionMode()) {
								prev_ack_per_sock[client_sock] =
									ack_message_base_.load(std::memory_order_acquire);
							}
							// Continue reading potential ACK data in the same loop iteration
						}
					// If ID still not complete, loop will try recv() again if more data indicated by epoll
				}else if(current_state == ConnState::READING_ACKS){
					// [[CRITICAL_FIX: Buffer partial ACK reads]] - Don't discard bytes when recv returns < sizeof(size_t).
					// Otherwise we can lose ACK data and ack_received_ never reaches client_order_ (test hangs).
					auto& partial = partial_ack_reads[client_sock];
					size_t needed = sizeof(size_t) - partial.second;
					auto ack_recv_start = std::chrono::steady_clock::now();
					ssize_t recv_ret = recv(client_sock,
							reinterpret_cast<char*>(&partial.first) + partial.second,
							needed, 0);
					if (ShouldEnableNetworkPathProfile()) {
						auto& net_profile = GetClientNetworkPathProfile();
						net_profile.ack_recv_calls.fetch_add(1, std::memory_order_relaxed);
						net_profile.ack_recv_syscall_ns.fetch_add(NsSince(ack_recv_start), std::memory_order_relaxed);
					}
					if (recv_ret == 0) { connection_error_or_closed = true; break; }
					if (recv_ret < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) break; // No more data now
						if (errno == EINTR) continue; // Retry read
						LOG(ERROR) << "AckThread: recv error reading ACK bytes on fd " << client_sock << ": " << strerror(errno);
						connection_error_or_closed = true; break;
					}
					partial.second += static_cast<size_t>(recv_ret);
					if (partial.second != sizeof(size_t)) {
						// EPOLLET requires draining socket data until EAGAIN.
						// If we break here, remaining bytes may never trigger a new edge,
						// stranding an ACK fragment and stalling cumulative progress.
						continue;
					}

					// --- Process Full ACK Value ---
					// Re-arm TCP_QUICKACK once per complete ACK (not per recv fragment).
					// Linux resets to delayed-ACK after each recv(); re-arming keeps the
					// broker's send window open without a syscall on every partial read.
					{
						int one = 1;
						setsockopt(client_sock, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
					}
					size_t acked_msg = partial.first;
					const uint32_t maybe_magic = static_cast<uint32_t>(acked_msg & 0xFFFFFFFFULL);
					const uint32_t control_len = static_cast<uint32_t>((acked_msg >> 32) & 0xFFFFFFFFULL);
					if (maybe_magic == kSessionControlMagic) {
						partial_ack_reads[client_sock] = {0, 0};
						if (control_len == 0 || control_len > kMaxSessionControlPayload) {
							LOG(ERROR) << "AckThread: invalid SessionFenced control length " << control_len;
							connection_error_or_closed = true;
							break;
						}
						std::string payload(control_len, '\0');
						const int old_flags = fcntl(client_sock, F_GETFL, 0);
						if (old_flags >= 0) {
							fcntl(client_sock, F_SETFL, old_flags & ~O_NONBLOCK);
						}
						struct timeval tv;
						tv.tv_sec = 1;
						tv.tv_usec = 0;
						setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
						size_t got = 0;
						while (got < control_len) {
							ssize_t n = recv(client_sock, payload.data() + got, control_len - got, 0);
							if (n <= 0) break;
							got += static_cast<size_t>(n);
						}
						if (old_flags >= 0) {
							fcntl(client_sock, F_SETFL, old_flags);
						}
						if (got != control_len) {
							LOG(ERROR) << "AckThread: short SessionFenced control payload got="
							           << got << " expected=" << control_len;
							connection_error_or_closed = true;
							break;
						}
						embarcadero::session::SessionFenced fenced;
						if (!fenced.ParseFromString(payload)) {
							LOG(ERROR) << "AckThread: failed to parse SessionFenced control payload";
							connection_error_or_closed = true;
							break;
						}
							LOG(WARNING) << "[SESSION_FENCED_OBSERVED]"
							             << " broker_id=" << client_sockets[client_sock]
							             << " committed_batch_seq=" << fenced.committed_batch_seq()
							             << " committed_msg_hwm=" << fenced.committed_msg_hwm()
							             << " control_epoch=" << fenced.control_epoch()
							             << " reason=" << fenced.reason();
							HandleSessionFenced(fenced, client_sockets[client_sock]);
							// Keep the ACK socket: SESSION_FENCED is recoverable via
							// reopen/resubmit. Closing here dropped ACK credit while
							// pool_pin still held every slot after a 1023-batch
							// resubmit, so Publish wedged forever in AcquireNextBatchFromPool.
							partial_ack_reads[client_sock] = {0, 0};
							continue;
						}
					if (ShouldEnableNetworkPathProfile()) {
						GetClientNetworkPathProfile().ack_values_processed.fetch_add(1, std::memory_order_relaxed);
					}
					partial_ack_reads[client_sock] = {0, 0}; // Reset for next ACK
					int broker_id = client_sockets[client_sock]; // Get broker ID

					// [[CRITICAL FIX: Validate broker_id bounds]] Check for FD reuse corruption
					if (broker_id < 0 || broker_id >= (int)broker_stats_.size()) {
						LOG(ERROR) << "AckThread: Invalid broker_id=" << broker_id << " for fd=" << client_sock
						           << " (FD reuse or corruption). Closing connection.";
						connection_error_or_closed = true; break;
					}

					const size_t session_global_acked =
						IsOrder5SessionMode()
							? SessionGlobalAckFromGeneration(
								ack_message_base_.load(std::memory_order_acquire),
								acked_msg)
							: acked_msg;
					size_t prev_acked = prev_ack_per_sock[client_sock]; // Assumes key exists

					if (session_global_acked >= prev_acked || prev_acked == (size_t)-1) { // Check for valid cumulative value
						// [[CRITICAL_FIX: Handle first ACK correctly to avoid unsigned underflow]]
						// If prev_acked == (size_t)-1, this is the first ACK from this broker
						// Direct subtraction would underflow: acked_msg - (size_t)-1 = huge number
						// We must handle first ACK specially: new_acked_msgs = acked_msg (not acked_msg - (-1))
						size_t new_acked_msgs;
						if (IsOrder5SessionMode()) {
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
                            if (!Embarcadero::fault::Pause("ack.before_authoritative_hwm",
                                    {static_cast<uint64_t>(client_id_), session_epoch_.load(), session_global_acked,
                                     order5_last_ack_hwm_.load(), ack_received_.load()}, &shutdown_)) return;
#endif
							size_t global_prev = order5_last_ack_hwm_.load(std::memory_order_acquire);
							while (session_global_acked > global_prev &&
							       !order5_last_ack_hwm_.compare_exchange_weak(
								       global_prev, session_global_acked,
								       std::memory_order_acq_rel,
								       std::memory_order_acquire)) {}
							new_acked_msgs = session_global_acked > global_prev ? session_global_acked - global_prev : 0;
						} else if (prev_acked == (size_t)-1) {
							// First ACK from this broker - use value directly (no previous to subtract)
							new_acked_msgs = acked_msg;
						} else {
							// Subsequent ACK - calculate increment from previous
							new_acked_msgs = session_global_acked - prev_acked;
						}
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
                        if (IsOrder5SessionMode() && new_acked_msgs > 0) {
                            size_t bytes;
                            { std::lock_guard<std::mutex> lock(unacked_mu_); bytes = unacked_bytes_; }
                            if (!Embarcadero::fault::Pause("ack.after_authoritative_hwm",
                                    {static_cast<uint64_t>(client_id_), session_epoch_.load(), session_global_acked,
                                     ack_received_.load(), bytes}, &shutdown_)) return;
                        }
#endif
						if (new_acked_msgs > 0) {
#ifdef COLLECT_LATENCY_STATS
								ProcessPublishAckLatency(broker_id, session_global_acked);
#endif
								broker_stats_[broker_id].acked_messages.fetch_add(new_acked_msgs, std::memory_order_relaxed);
								last_ack_progress_ns_.store(SteadyNowNs(), std::memory_order_release);
								prev_ack_per_sock[client_sock] = session_global_acked; // Update last value for this socket
								CompleteUnackedThrough(broker_id, session_global_acked);
								ack_received_.fetch_add(new_acked_msgs, std::memory_order_release);
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
                                if (IsOrder5SessionMode()) {
                                    size_t bytes, batches;
                                    { std::lock_guard<std::mutex> lock(unacked_mu_);
                                      bytes = unacked_bytes_; batches = unacked_batches_.size(); }
                                    if (!Embarcadero::fault::Pause("ack.after_retirement",
                                            {static_cast<uint64_t>(client_id_), session_epoch_.load(), session_global_acked,
                                             bytes, batches}, &shutdown_)) return;
                                }
#endif
							} else {
							// Duplicate cumulative value, ignore.
							VLOG(5) << "AckThread: fd=" << client_sock << " (Broker " << broker_id << 
								") Duplicate ACK messages received: " << session_global_acked;
						}
					} else {
						LOG(WARNING) << "AckThread: Received non-monotonic ACK bytes on fd " << client_sock
							<< " (Broker " << broker_id << "). Received: " << session_global_acked << ", Previous: " << prev_acked;
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

// [[CRITICAL FIX: Clean up all ACK state]] Prevent FD reuse corruption in future runs
for (auto const& [sock_fd, broker_id] : client_sockets) {
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sock_fd, nullptr);
	close(sock_fd);
}
client_sockets.clear();
prev_ack_per_sock.clear();
socket_state.clear();
partial_id_reads.clear();
partial_ack_reads.clear();

// Clean up epoll and server socket
close(epoll_fd);
close(server_sock);
}

#ifdef COLLECT_LATENCY_STATS
void Publisher::RecordPublishSend(
		int broker_id,
		size_t end_count,
		const std::chrono::steady_clock::time_point& submit_time,
		bool has_submit_time) {
	if (!record_results_ || ack_level_ < 1 || end_count == 0) {
		return;
	}
	BatchSendRecord record{end_count, std::chrono::steady_clock::now(), submit_time, has_submit_time};
	{
		std::lock_guard<std::mutex> lock(send_records_mutexes_[broker_id]);
		auto& records = send_records_per_broker_[broker_id];
		// Keep records sorted by end_count so ACK processing can pop_front in O(k).
		if (records.empty() || record.end_count >= records.back().end_count) {
			records.push_back(record);
		} else {
			publish_latency_out_of_order_inserts_.fetch_add(1, std::memory_order_relaxed);
			auto it = std::upper_bound(records.begin(), records.end(), record.end_count,
				[](size_t target_end_count, const BatchSendRecord& candidate) {
					return target_end_count < candidate.end_count;
				});
			records.insert(it, record);
		}
	}
}
#endif

#ifdef COLLECT_LATENCY_STATS
void Publisher::ProcessPublishAckLatency(int broker_id, size_t acked_msg) {
	if (!record_results_ || ack_level_ < 1) {
		return;
	}
	std::vector<long long> local_send_to_ack_latencies;
	std::vector<long long> local_submit_to_ack_latencies;
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(send_records_mutexes_[broker_id]);
		auto& records = send_records_per_broker_[broker_id];
		while (!records.empty() && records.front().end_count <= acked_msg) {
			const auto send_to_ack_latency = std::chrono::duration_cast<std::chrono::microseconds>(
					now - records.front().sent_time).count();
			local_send_to_ack_latencies.push_back(send_to_ack_latency);
			if (records.front().has_submit_time) {
				const auto submit_to_ack_latency = std::chrono::duration_cast<std::chrono::microseconds>(
						now - records.front().submit_time).count();
				local_submit_to_ack_latencies.push_back(submit_to_ack_latency);
			}
			records.pop_front();
		}
	}
	if (!local_send_to_ack_latencies.empty() || !local_submit_to_ack_latencies.empty()) {
		std::lock_guard<std::mutex> lock(publish_latency_mutex_);
		if (!local_send_to_ack_latencies.empty()) {
			publish_send_to_ack_latencies_us_.insert(
					publish_send_to_ack_latencies_us_.end(),
					local_send_to_ack_latencies.begin(),
					local_send_to_ack_latencies.end());
			publish_send_to_ack_batch_samples_recorded_.fetch_add(local_send_to_ack_latencies.size(), std::memory_order_relaxed);
		}
		if (!local_submit_to_ack_latencies.empty()) {
			publish_submit_to_ack_latencies_us_.insert(
					publish_submit_to_ack_latencies_us_.end(),
					local_submit_to_ack_latencies.begin(),
					local_submit_to_ack_latencies.end());
			publish_submit_to_ack_batch_samples_recorded_.fetch_add(local_submit_to_ack_latencies.size(), std::memory_order_relaxed);
		}
	}
}
#endif

#ifdef COLLECT_LATENCY_STATS
void Publisher::WritePublishLatencyResults() {
	if (!record_results_ || ack_level_ < 1) {
		return;
	}
	std::vector<long long> send_to_ack_latencies_copy;
	std::vector<long long> submit_to_ack_latencies_copy;
	{
		std::lock_guard<std::mutex> lock(publish_latency_mutex_);
		send_to_ack_latencies_copy = publish_send_to_ack_latencies_us_;
		submit_to_ack_latencies_copy = publish_submit_to_ack_latencies_us_;
	}
	if (send_to_ack_latencies_copy.empty() && submit_to_ack_latencies_copy.empty()) {
		LOG(WARNING) << "No publish ACK latency values could be calculated.";
		return;
	}

	const bool have_send_metric = !send_to_ack_latencies_copy.empty();
	const bool have_submit_metric = !submit_to_ack_latencies_copy.empty();
	const bool have_ordered_metric = (ack_level_ == 1) && have_send_metric;
	Embarcadero::LatencyStats::Summary send_to_ack_summary{};
	Embarcadero::LatencyStats::Summary submit_to_ack_summary{};
	Embarcadero::LatencyStats::Summary send_to_ordered_summary{};
	if (have_send_metric) {
		std::sort(send_to_ack_latencies_copy.begin(), send_to_ack_latencies_copy.end());
		send_to_ack_summary = Embarcadero::LatencyStats::ComputeSummary(send_to_ack_latencies_copy);
		if (have_ordered_metric) {
			send_to_ordered_summary = send_to_ack_summary;
		}
	}
	if (have_submit_metric) {
		std::sort(submit_to_ack_latencies_copy.begin(), submit_to_ack_latencies_copy.end());
		submit_to_ack_summary = Embarcadero::LatencyStats::ComputeSummary(submit_to_ack_latencies_copy);
	}
	const size_t send_to_ack_samples_recorded = publish_send_to_ack_batch_samples_recorded_.load(std::memory_order_relaxed);
	const size_t submit_to_ack_samples_recorded = publish_submit_to_ack_batch_samples_recorded_.load(std::memory_order_relaxed);
	const size_t acked_messages = ack_received_.load(std::memory_order_relaxed);
	const size_t total_batches_sent = total_batches_sent_.load(std::memory_order_relaxed);
	if ((have_send_metric && send_to_ack_summary.count > total_batches_sent) ||
		(have_submit_metric && submit_to_ack_summary.count > total_batches_sent)) {
		LOG(WARNING) << "Publish ACK batch latency samples exceed total sent batches: send_samples="
		             << (have_send_metric ? send_to_ack_summary.count : 0)
		             << " submit_samples=" << (have_submit_metric ? submit_to_ack_summary.count : 0)
		             << " total_batches_sent=" << total_batches_sent;
	}
	if (have_send_metric && send_to_ack_samples_recorded != send_to_ack_summary.count) {
		LOG(WARNING) << "Publish send->ack sample counter mismatch: counter=" << send_to_ack_samples_recorded
		             << " summarized=" << send_to_ack_summary.count;
	}
	if (have_submit_metric && submit_to_ack_samples_recorded != submit_to_ack_summary.count) {
		LOG(WARNING) << "Publish submit->ack sample counter mismatch: counter=" << submit_to_ack_samples_recorded
		             << " summarized=" << submit_to_ack_summary.count;
	}
	const size_t missing_submit_timestamps = publish_submit_time_missing_.load(std::memory_order_relaxed);
	if (missing_submit_timestamps > 0) {
		LOG(WARNING) << "Submit->ACK metric dropped " << missing_submit_timestamps
		             << " batch sample(s) due to missing submit timestamps.";
	}

	if (have_submit_metric) {
		LOG(INFO) << "Publish Submit->ACK Batch Latency Statistics (us):";
		LOG(INFO) << "  Count:   " << submit_to_ack_summary.count;
		LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << submit_to_ack_summary.average_us;
		LOG(INFO) << "  Min:     " << submit_to_ack_summary.min_us;
		LOG(INFO) << "  P50:     " << submit_to_ack_summary.p50_us;
		LOG(INFO) << "  P99:     " << submit_to_ack_summary.p99_us;
		LOG(INFO) << "  P99.9:   " << submit_to_ack_summary.p999_us;
		LOG(INFO) << "  Max:     " << submit_to_ack_summary.max_us;
		LOG(INFO) << "  Semantics: append_submit_to_ack batch latency (sample granularity=batch)";
		LOG(INFO) << "  Invariant: batch_samples <= total_batches_sent (samples=" << submit_to_ack_summary.count
		          << ", total_batches_sent=" << total_batches_sent << ")";
	} else {
		LOG(WARNING) << "Publish Submit->ACK Batch Latency Statistics unavailable (no valid submit timestamps).";
	}
	if (have_send_metric) {
		LOG(INFO) << "Publish Send->ACK Batch Latency Statistics (us):";
		LOG(INFO) << "  Count:   " << send_to_ack_summary.count;
		LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << send_to_ack_summary.average_us;
		LOG(INFO) << "  Min:     " << send_to_ack_summary.min_us;
		LOG(INFO) << "  P50:     " << send_to_ack_summary.p50_us;
		LOG(INFO) << "  P99:     " << send_to_ack_summary.p99_us;
		LOG(INFO) << "  P99.9:   " << send_to_ack_summary.p999_us;
		LOG(INFO) << "  Max:     " << send_to_ack_summary.max_us;
		LOG(INFO) << "  Semantics: append_send_to_ack batch latency (sample granularity=batch)";
	} else {
		LOG(WARNING) << "Publish Send->ACK Batch Latency Statistics unavailable.";
	}
	if (have_ordered_metric) {
		LOG(INFO) << "Publish Send->Ordered Batch Latency Statistics (us):";
		LOG(INFO) << "  Count:   " << send_to_ordered_summary.count;
		LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << send_to_ordered_summary.average_us;
		LOG(INFO) << "  Min:     " << send_to_ordered_summary.min_us;
		LOG(INFO) << "  P50:     " << send_to_ordered_summary.p50_us;
		LOG(INFO) << "  P99:     " << send_to_ordered_summary.p99_us;
		LOG(INFO) << "  P99.9:   " << send_to_ordered_summary.p999_us;
		LOG(INFO) << "  Max:     " << send_to_ordered_summary.max_us;
		LOG(INFO) << "  Semantics: append_send_to_ordered batch latency (derived from ACK path at ack_level=1)";
	}
	LOG(INFO) << "  Submit timestamps missing (submit metric sample drops): " << missing_submit_timestamps;

	// [[MULTI-PUB]] EMBARCADERO_LATENCY_OUT_DIR lets N concurrent publishers on one
	// host each write to a distinct directory, avoiding clobbering a shared-cwd file.
	// Empty/unset preserves the historical cwd-relative behavior. The caller must
	// ensure the directory exists.
	const char* _lat_out_env = std::getenv("EMBARCADERO_LATENCY_OUT_DIR");
	const std::string _lat_out_dir =
		(_lat_out_env && *_lat_out_env) ? (std::string(_lat_out_env) + "/") : std::string();
	const std::string latency_filename = _lat_out_dir + "pub_latency_stats.csv";
	std::ofstream latency_file(latency_filename);
	if (!latency_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << latency_filename;
	} else {
		latency_file << "Average,Min,Median,p90,p95,p99,p999,Max,Count,Metric,Unit,PercentileMethod,Granularity,SampleCountMeaning,AckedMessages,TotalBatchesSent,OutOfOrderInserts,MissingSubmitTimestamps\n";
		if (have_submit_metric) {
			latency_file << std::fixed << std::setprecision(3) << submit_to_ack_summary.average_us
				<< "," << submit_to_ack_summary.min_us
				<< "," << submit_to_ack_summary.p50_us
				<< "," << submit_to_ack_summary.p90_us
				<< "," << submit_to_ack_summary.p95_us
				<< "," << submit_to_ack_summary.p99_us
				<< "," << submit_to_ack_summary.p999_us
				<< "," << submit_to_ack_summary.max_us
				<< "," << submit_to_ack_summary.count
				<< ",append_submit_to_ack_batch_latency"
				<< ",us"
				<< "," << Embarcadero::LatencyStats::kPercentileMethod
				<< ",batch"
				<< ",samples=count_of_fully_acked_batches_with_submit_timestamp"
				<< "," << acked_messages
				<< "," << total_batches_sent
				<< "," << publish_latency_out_of_order_inserts_.load(std::memory_order_relaxed)
				<< "," << missing_submit_timestamps
				<< "\n";
		}
		if (have_send_metric) {
			latency_file << std::fixed << std::setprecision(3) << send_to_ack_summary.average_us
				<< "," << send_to_ack_summary.min_us
				<< "," << send_to_ack_summary.p50_us
				<< "," << send_to_ack_summary.p90_us
				<< "," << send_to_ack_summary.p95_us
				<< "," << send_to_ack_summary.p99_us
				<< "," << send_to_ack_summary.p999_us
				<< "," << send_to_ack_summary.max_us
				<< "," << send_to_ack_summary.count
				<< ",append_send_to_ack_batch_latency"
				<< ",us"
				<< "," << Embarcadero::LatencyStats::kPercentileMethod
				<< ",batch"
				<< ",samples=count_of_fully_acked_batches"
				<< "," << acked_messages
				<< "," << total_batches_sent
				<< "," << publish_latency_out_of_order_inserts_.load(std::memory_order_relaxed)
				<< "," << missing_submit_timestamps
				<< "\n";
		}
		if (have_ordered_metric) {
			latency_file << std::fixed << std::setprecision(3) << send_to_ordered_summary.average_us
				<< "," << send_to_ordered_summary.min_us
				<< "," << send_to_ordered_summary.p50_us
				<< "," << send_to_ordered_summary.p90_us
				<< "," << send_to_ordered_summary.p95_us
				<< "," << send_to_ordered_summary.p99_us
				<< "," << send_to_ordered_summary.p999_us
				<< "," << send_to_ordered_summary.max_us
				<< "," << send_to_ordered_summary.count
				<< ",append_send_to_ordered_batch_latency"
				<< ",us"
				<< "," << Embarcadero::LatencyStats::kPercentileMethod
				<< ",batch"
				<< ",samples=count_of_fully_ordered_batches_derived_from_ack_level_1"
				<< "," << acked_messages
				<< "," << total_batches_sent
				<< "," << publish_latency_out_of_order_inserts_.load(std::memory_order_relaxed)
				<< "," << missing_submit_timestamps
				<< "\n";
		}
		latency_file.close();
	}

	const std::string cdf_filename = _lat_out_dir + "pub_cdf_latency_us.csv";
	std::ofstream cdf_file(cdf_filename);
	if (!cdf_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << cdf_filename;
	} else {
		cdf_file << "Latency_us,CumulativeProbability,Metric\n";
		if (have_submit_metric) {
			for (size_t i = 0; i < submit_to_ack_summary.count; ++i) {
				const long long current_latency = submit_to_ack_latencies_copy[i];
				const double cumulative_probability = static_cast<double>(i + 1) / submit_to_ack_summary.count;
				cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability
				         << ",append_submit_to_ack_batch_latency\n";
			}
		}
		if (have_send_metric) {
			for (size_t i = 0; i < send_to_ack_summary.count; ++i) {
				const long long current_latency = send_to_ack_latencies_copy[i];
				const double cumulative_probability = static_cast<double>(i + 1) / send_to_ack_summary.count;
				cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability
				         << ",append_send_to_ack_batch_latency\n";
			}
		}
		if (have_ordered_metric) {
			for (size_t i = 0; i < send_to_ordered_summary.count; ++i) {
				const long long current_latency = send_to_ack_latencies_copy[i];
				const double cumulative_probability = static_cast<double>(i + 1) / send_to_ordered_summary.count;
				cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability
				         << ",append_send_to_ordered_batch_latency\n";
			}
		}
		cdf_file.close();
	}
}
#endif

