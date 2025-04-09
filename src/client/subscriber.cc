#include "subscriber.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <numeric>

Subscriber::Subscriber(std::string head_addr, std::string port, char topic[TOPIC_NAME_SIZE], bool measure_latency)
    : head_addr_(head_addr),
      port_(port),
      shutdown_(false),
      connected_(false),
      measure_latency_(measure_latency),
      buffer_size_((1UL << 33)), // Still 8GB per connection! Needs tuning.
      client_id_(GenerateRandomNum()){
    memcpy(topic_, topic, TOPIC_NAME_SIZE);
    std::string grpc_addr = head_addr + ":" + port;
    stub_ = heartbeat_system::HeartBeat::NewStub(grpc::CreateChannel(grpc_addr, grpc::InsecureChannelCredentials()));
    {
        absl::MutexLock lock(&node_mutex_);
        nodes_[0] = head_addr + ":" + std::to_string(PORT);
    }
    VLOG(5) << "Subscriber initialized. Starting cluster probe thread.";
    cluster_probe_thread_ = std::thread([this]() {
        this->SubscribeToClusterStatus();
    });
     while (!connected_) {
       std::this_thread::yield();
     }
}

// --- Destructor (Unchanged) ---
Subscriber::~Subscriber() {
    VLOG(5) << "Subscriber shutting down...";
    shutdown_ = true;
    //context_.TryCancel();
    if (cluster_probe_thread_.joinable()) {
        cluster_probe_thread_.join();
    }
		// --- Initiate shutdown for worker sockets BEFORE joining ---
    VLOG(5) << "Signaling worker threads to stop by closing sockets...";
    {
        absl::MutexLock lock(&worker_mutex_);
				for (const auto& worker_pair : worker_threads_with_fds_) {
            int fd = worker_pair.second;
            if (fd >= 0) {
                 // Using shutdown() is generally preferred over close() for signaling
                 // SHUT_RDWR signals to disallow both further reads and writes.
                 // This should cause blocked recv() to return.
                 if (::shutdown(fd, SHUT_RDWR) == -1) {
                     // Log error but continue trying to join anyway
                     LOG(WARNING) << "shutdown(fd=" << fd << ", SHUT_RDWR) failed: " << strerror(errno);
                 } else {
                     VLOG(5) << "shutdown(fd=" << fd << ", SHUT_RDWR) called.";
                 }
                 // close(fd); // Alternative, but shutdown() is cleaner for signaling
            }
        }
    }
		// --- Join worker threads ---
    {
        absl::MutexLock lock(&worker_mutex_);
        for (auto& worker_pair : worker_threads_with_fds_) {
             std::thread& t = worker_pair.first; // Get the thread object
             int fd = worker_pair.second;        // Get the fd (for logging)
            if (t.joinable()) {
                VLOG(5) << "Joining thread for FD " << fd << "...";
                t.join(); // Should now return relatively quickly
            }
        }
        worker_threads_with_fds_.clear();
    }
    VLOG(5) << "Subscriber shutdown complete.";
}

void* Subscriber::Consume() {
	// Currently a stub implementation
	// In a real implementation, this would atomically swap the current message buffer
	// and return the previously filled one
	return nullptr;
}

void* Subscriber::ConsumeBatch() {
	// Currently a stub implementation
	// In a real implementation, this would return a batch of messages
	return nullptr;
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
	VLOG(5) << "Storing latency measurements";

	/*
	if (!measure_latency_) {
		LOG(WARNING) << "Latency measurement was not enabled at subscriber initialization";
		return;
	}

	std::vector<long long> latencies;
	int idx = 0;

	for (auto& pair : messages_[idx]) {
		struct msgIdx* m = pair.second;
		void* buf = pair.first;

		if (!buf || !m) {
			continue;
		}

		// Calculate latencies for each message in the buffer
		size_t offset = 0;
		int recv_latency_idx = 0;

		while (offset < m->offset) {
			Embarcadero::MessageHeader* header = 
				reinterpret_cast<Embarcadero::MessageHeader*>(static_cast<uint8_t*>(buf) + offset);

			offset += header->paddedSize;

			// Find the right timestamp record for this message
			while (recv_latency_idx < static_cast<int>(m->timestamps.size()) && 
					offset > m->timestamps[recv_latency_idx].first) {
				recv_latency_idx++;
			}

			// Skip if no matching timestamp found
			if (recv_latency_idx >= static_cast<int>(m->timestamps.size())) {
				break;
			}

			// Extract send timestamp from message payload
			long long send_nanoseconds_since_epoch;
			memcpy(&send_nanoseconds_since_epoch, 
					static_cast<uint8_t*>(buf) + offset - header->paddedSize + sizeof(Embarcadero::MessageHeader), 
					sizeof(long long));

			// Create time point from stored timestamp
			auto sent_time_point = std::chrono::time_point<std::chrono::steady_clock>(
					std::chrono::nanoseconds(send_nanoseconds_since_epoch));

			// Calculate latency
			auto latency = m->timestamps[recv_latency_idx].second - sent_time_point;
			latencies.emplace_back(std::chrono::duration_cast<std::chrono::nanoseconds>(latency).count());
		}
	}

	// Check if we collected any latency measurements
	if (latencies.empty()) {
		LOG(WARNING) << "No latency measurements collected";
		return;
	}

	// Calculate statistics
	std::sort(latencies.begin(), latencies.end());
	long long min_latency = latencies.front();
	long long max_latency = latencies.back();
	long long median_latency = latencies[latencies.size() / 2];
	long long p99_latency = latencies[static_cast<size_t>(latencies.size() * 0.99)];
	double avg_latency = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

	LOG(INFO) << "Latency statistics (ns):"
		<< " Min=" << min_latency
		<< " Avg=" << std::fixed << std::setprecision(2) << avg_latency
		<< " Median=" << median_latency
		<< " P99=" << p99_latency
		<< " Max=" << max_latency
		<< " Count=" << latencies.size();

	// Write raw latency data to file
	std::ofstream latencyFile("latencies.csv");
	if (!latencyFile.is_open()) {
		LOG(ERROR) << "Failed to open file for writing latency data";
		return;
	}

	latencyFile << "Latency_ns\n";
	for (const auto& latency : latencies) {
		latencyFile << latency << "\n";
	}

	latencyFile.close();
	LOG(INFO) << "Latency data written to latencies.csv";
	*/
}

void Subscriber::DEBUG_wait(size_t total_msg_size, size_t msg_size) {
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

    // Step 5 & 6 Combined: Launch one worker thread per connection
    {
        absl::MutexLock lock(&worker_mutex_);
        for (int connected_fd : connected_fds) {
             VLOG(5) << "Launching worker thread for broker " << broker_id << " FD " << connected_fd;
						 worker_threads_with_fds_.emplace_back(
							 std::piecewise_construct,
							 std::forward_as_tuple(&Subscriber::ReceiveWorkerThread, this, this, broker_id, connected_fd), // Thread constructor args
							 std::forward_as_tuple(connected_fd) // FD to store
						 );
        }
    }
    connected_ = true; // Signal started processing
}


void Subscriber::ReceiveWorkerThread(Subscriber* subscriber_instance, int broker_id, int fd_to_handle) {
    // --- Resource Allocation ---
    std::unique_ptr<ManagedConnectionResources> resources;
    try {
        // IMPORTANT: HUGE BUFFER SIZE - Needs tuning!
        resources = std::make_unique<ManagedConnectionResources>(broker_id, subscriber_instance->buffer_size_);
    } catch (const std::runtime_error& e) {
        LOG(ERROR) << "Worker (broker " << broker_id << ", fd " << fd_to_handle << "): Failed to allocate resources: " << e.what();
        close(fd_to_handle);
        return;
    }

    // Get buffer and index pointers
    void* buf = resources->getBuffer();
    msgIdx* m = resources->getMsgIdx();

    // --- Send Subscription Request ---
    Embarcadero::EmbarcaderoReq shake;
    memset(&shake, 0, sizeof(shake));
    shake.num_msg = 0;
    shake.client_id = subscriber_instance->client_id_;
    shake.last_addr = 0;
    shake.client_req = Embarcadero::Subscribe;
    memcpy(shake.topic, subscriber_instance->topic_, TOPIC_NAME_SIZE);

    if (send(fd_to_handle, &shake, sizeof(shake), 0) < static_cast<ssize_t>(sizeof(shake))) {
        LOG(ERROR) << "Worker (broker " << broker_id << "): Failed to send subscription request on fd " << fd_to_handle << ": " << strerror(errno);
        // unique_ptr cleans up resources automatically when function returns
        close(fd_to_handle);
        return; // Exit thread
    }

    VLOG(5) << "Worker (broker " << broker_id << ", fd " << fd_to_handle << "): Successfully subscribed.";

    // --- Main receive loop (Simplified - Blocking recv) ---
    while (!subscriber_instance->shutdown_) {
        size_t current_offset = m->offset;

        // Check if buffer needs wrapping
        if (current_offset >= subscriber_instance->buffer_size_) {
            LOG(WARNING) << "Worker (broker " << broker_id << ", fd " << fd_to_handle << "): Buffer full, wrapping around. Overwriting data.";
            subscriber_instance->DEBUG_do_not_check_order_ = true;
            current_offset = 0;
            m->offset = 0; // Reset offset
        }

        size_t to_read = subscriber_instance->buffer_size_ - current_offset;
        if (to_read == 0) { // Safety check
            LOG(ERROR) << "Worker (broker " << broker_id << ", fd " << fd_to_handle << "): Buffer full and wrap failed? Should not happen.";
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Prevent tight spin loop
            continue;
        }

        // Blocking receive call
        ssize_t bytes_received = recv(fd_to_handle, static_cast<uint8_t*>(buf) + current_offset, to_read, 0);

        if (bytes_received > 0) {
            subscriber_instance->DEBUG_count_.fetch_add(bytes_received, std::memory_order_relaxed);
            m->offset += bytes_received;

            // --- Header Collection Logic REMOVED as requested ---

        } else if (bytes_received == 0) {
            // Connection closed by peer
            VLOG(5) << "Worker (broker " << broker_id << "): Broker disconnected on fd " << fd_to_handle << ". Closing thread.";
            break; // Exit loop
        } else { // bytes_received < 0
            // Error on recv
            if (errno == EINTR) {
                 VLOG(1) << "Worker (broker " << broker_id << ", fd " << fd_to_handle << "): recv interrupted by signal, continuing.";
                continue; // Retry recv
            } else {
                // Check if shutdown was signaled during blocking recv
                 if (subscriber_instance->shutdown_) {
                     LOG(INFO) << "Worker (broker " << broker_id << ", fd " << fd_to_handle << "): Shutdown detected during recv error check.";
                 } else {
                    LOG(ERROR) << "Worker (broker " << broker_id << "): recv failed on fd " << fd_to_handle << ": " << strerror(errno);
                 }
                break; // Exit loop on fatal error or shutdown
            }
        }
    } // End while(!shutdown_)


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
