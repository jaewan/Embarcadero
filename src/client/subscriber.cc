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
      buffer_size_((1UL << 33)),  // 8GB buffer size
      messages_idx_(0),
      client_id_(GenerateRandomNum()) {
    
    // Initialize message storage with two buffers for double-buffering
    messages_.resize(2);
    
    // Copy topic name
    memcpy(topic_, topic, TOPIC_NAME_SIZE);
    
    // Create gRPC stub
    std::string addr = head_addr + ":" + port;
    stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
    
    // Initialize with head broker
    nodes_[0] = head_addr + ":" + std::to_string(PORT);
    
    // Start thread to monitor cluster status
    cluster_probe_thread_ = std::thread([this]() {
        this->SubscribeToClusterStatus();
    });
    
    // Wait for connection to be established
    while (!connected_) {
        std::this_thread::yield();
    }
    
    VLOG(3) << "Subscriber constructed with client_id: " << client_id_ << ", topic: " << topic;
}

Subscriber::~Subscriber() {
    VLOG(3) << "Subscriber destructor called, cleaning up resources";
    
    // Signal all threads to terminate
    shutdown_ = true;
    context_.TryCancel();
    
    // Wait for cluster probe thread to complete
    if (cluster_probe_thread_.joinable()) {
        cluster_probe_thread_.join();
    }
    
    // Wait for all subscription threads to complete
    for (auto& t : subscribe_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    // Free all allocated message buffers
    for (auto& msg_pairs : messages_) {
        for (auto& msg_pair : msg_pairs) {
            if (msg_pair.first) {
                free(msg_pair.first);
                msg_pair.first = nullptr;
            }
            if (msg_pair.second) {
                free(msg_pair.second);
                msg_pair.second = nullptr;
            }
        }
    }
    
    VLOG(3) << "Subscriber successfully destructed";
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
    VLOG(3) << "Checking message order with order level: " << order;
    
    // Skip checks if disabled
    if (DEBUG_do_not_check_order_) {
        VLOG(3) << "Order checking disabled, skipping checks";
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
        VLOG(2) << "Order check passed for level " << order;
    } else {
        LOG(ERROR) << "Order check failed for level " << order;
    }
    
    return order_correct;
}

void Subscriber::StoreLatency() {
    VLOG(2) << "Storing latency measurements";
    
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
}

void Subscriber::DEBUG_wait(size_t total_msg_size, size_t msg_size) {
    VLOG(2) << "Waiting to receive " << total_msg_size << " bytes of data with message size " << msg_size;
    
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
            VLOG(2) << "Received " << DEBUG_count_ << "/" << total_data_size 
                    << " bytes (" << std::fixed << std::setprecision(1) << percentage << "%)";
            last_log_time = now;
        }
    }
    
    auto end = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();
    double throughput_mbps = (total_data_size / seconds) / (1024 * 1024);
    
    LOG(INFO) << "Received all " << total_data_size << " bytes in " << seconds << " seconds"
              << " (" << throughput_mbps << " MB/s)";
}

void Subscriber::SubscribeThread(int epoll_fd, absl::flat_hash_map<int, std::pair<void*, msgIdx*>> fd_to_msg) {
    VLOG(3) << "Starting subscriber thread with epoll_fd " << epoll_fd;
    
    // Set up epoll events array
    epoll_event events[NUM_SUB_CONNECTIONS];
    
    // Main loop
    while (!shutdown_) {
        // Wait for events with a 100ms timeout
        int nfds = epoll_wait(epoll_fd, events, NUM_SUB_CONNECTIONS, 100);
        
        if (nfds == -1) {
            if (errno == EINTR) {
                // Interrupted system call, just continue
                continue;
            }
            
            LOG(ERROR) << "epoll_wait error: " << strerror(errno);
            break;
        }
        
        // Process events
        for (int n = 0; n < nfds; ++n) {
            if (events[n].events & EPOLLIN) {
                int fd = events[n].data.fd;
                auto it = fd_to_msg.find(fd);
                
                if (it == fd_to_msg.end()) {
                    LOG(ERROR) << "Unknown file descriptor: " << fd;
                    continue;
                }
                
                // Get buffer and metadata
                struct msgIdx* m = it->second.second;
                void* buf = it->second.first;
                
                // Calculate remaining buffer space
                size_t to_read = buffer_size_ - m->offset;
                
                // If buffer is full, wrap around or expand if needed
                if (to_read == 0) {
                    LOG(WARNING) << "Subscriber buffer is full. Overwriting from head. "
                                << "Consider increasing buffer_size_ or enable safe overflow handling.";
                    
                    // Skip order checking as we've overwritten data
                    DEBUG_do_not_check_order_ = true;
                    
                    // Reset to beginning of buffer
                    m->offset = 0;
                    to_read = buffer_size_;
                }
                
                // Receive data
                int bytes_received = recv(fd, static_cast<uint8_t*>(buf) + m->offset, to_read, 0);
                
                if (bytes_received > 0) {
                    // Update counters and timestamps
                    DEBUG_count_.fetch_add(bytes_received, std::memory_order_relaxed);
                    m->offset += bytes_received;
                    
                    VLOG(4) << "Received " << bytes_received << " bytes from fd " << fd 
                            << ", total " << m->offset << "/" << buffer_size_;
                    
                    // Record timestamp if measuring latency
                    if (measure_latency_) {
                        m->timestamps.emplace_back(m->offset, std::chrono::steady_clock::now());
                    }
                } else if (bytes_received == 0) {
                    // Connection closed by broker
                    LOG(WARNING) << "Broker disconnected on fd " << fd << ": " << strerror(errno);
                    
                    // Remove from epoll and close socket
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    
                    // In production, we'd want to reconnect to the broker or mark it as failed
                    break;
                } else {
                    // Error occurred
                    if (errno != EWOULDBLOCK && errno != EAGAIN) {
                        LOG(ERROR) << "recv failed on fd " << fd << ": " << strerror(errno);
                    }
                    // For EAGAIN/EWOULDBLOCK, we'll try again on the next epoll event
                    break;
                }
            } else if (events[n].events & (EPOLLERR | EPOLLHUP)) {
                // Handle error or hangup
                int fd = events[n].data.fd;
                LOG(WARNING) << "EPOLLERR or EPOLLHUP on fd " << fd;
                
                // Remove from epoll and close socket
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                
                // In production, we'd want to reconnect to the broker or mark it as failed
            }
        }
    }
    
    // Clean up epoll instance
    close(epoll_fd);
    
    VLOG(3) << "Subscriber thread exiting for epoll_fd " << epoll_fd;
}

void Subscriber::CreateAConnection(int broker_id, std::string address) {
    VLOG(2) << "Creating connections to broker " << broker_id << " at " << address;
    
    // Parse address to get IP and port
    auto [addr, addressPort] = ParseAddressPort(address);
    
    // Create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG(ERROR) << "Failed to create epoll instance: " << strerror(errno);
        return;
    }
    
    // Map to track file descriptors to message buffers
    absl::flat_hash_map<int, std::pair<void*, msgIdx*>> fd_to_msg;
    
    // Create multiple connections to the broker
    int successful_connections = 0;
    
    for (int i = 0; i < NUM_SUB_CONNECTIONS; i++) {
        // Create socket
        int sock = GetNonblockingSock(const_cast<char*>(addr.c_str()), PORT + broker_id, false);
        if (sock < 0) {
            LOG(ERROR) << "Failed to create socket for broker " << broker_id;
            continue;
        }
        
        // Allocate message buffer and metadata
        void* buffer = calloc(buffer_size_, sizeof(char));
        msgIdx* msg_idx = static_cast<msgIdx*>(malloc(sizeof(msgIdx)));
        
        if (!buffer || !msg_idx) {
            LOG(ERROR) << "Failed to allocate memory for message buffer or metadata";
            if (buffer) free(buffer);
            if (msg_idx) free(msg_idx);
            close(sock);
            continue;
        }
        
        // Initialize message index
        new (msg_idx) msgIdx(broker_id);
        
        // Store message buffer and metadata
        messages_[0].push_back(std::make_pair(buffer, msg_idx));
        fd_to_msg.insert({sock, std::make_pair(buffer, msg_idx)});
        
        // Create subscription request
        Embarcadero::EmbarcaderoReq shake;
        shake.num_msg = 0;
        shake.client_id = client_id_;
        shake.last_addr = 0;
        shake.client_req = Embarcadero::Subscribe;
        memcpy(shake.topic, topic_, TOPIC_NAME_SIZE);
        
        // Send subscription request
        int ret = send(sock, &shake, sizeof(shake), 0);
        if (ret < static_cast<int>(sizeof(shake))) {
            LOG(ERROR) << "Failed to send subscription request to broker " << broker_id 
                       << " (" << ret << "/" << sizeof(shake) << " bytes sent): " << strerror(errno);
            close(sock);
            continue;
        }
        
        // Add socket to epoll
        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = sock;
        
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) == -1) {
            LOG(ERROR) << "Failed to add socket to epoll: " << strerror(errno);
            close(sock);
            continue;
        }
        
        VLOG(3) << "Successfully established connection " << i << " to broker " << broker_id;
        successful_connections++;
    }
    
    if (successful_connections == 0) {
        LOG(ERROR) << "Failed to establish any connections to broker " << broker_id;
        close(epoll_fd);
        return;
    }
    
    // Start thread to handle this broker's connections
    subscribe_threads_.emplace_back(&Subscriber::SubscribeThread, this, epoll_fd, fd_to_msg);
    
    VLOG(2) << "Created " << successful_connections << " connections to broker " << broker_id;
}

void Subscriber::SubscribeToClusterStatus() {
    VLOG(2) << "Starting cluster status subscription";
    
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
    std::unique_ptr<grpc::ClientReader<ClusterStatus>> reader(
        stub_->SubscribeToCluster(&context_, client_info));
    
    // Connect to head broker
    CreateAConnection(0, nodes_[0]);
    
    // Process cluster status updates
    while (!shutdown_) {
        if (reader->Read(&cluster_status)) {
            const auto& new_nodes = cluster_status.new_nodes();
            
            if (!new_nodes.empty()) {
                absl::MutexLock lock(&mutex_);
                
                // Add new brokers
                for (const auto& addr : new_nodes) {
                    int broker_id = GetBrokerId(addr);
                    
                    if (nodes_.find(broker_id) != nodes_.end()) {
                        VLOG(3) << "Broker " << broker_id << " already known, skipping";
                        continue;
                    }
                    
                    nodes_[broker_id] = addr;
                    CreateAConnection(broker_id, addr);
                    
                    VLOG(2) << "Added new broker: " << broker_id << " at " << addr;
                }
            }
            
            // Signal that we're connected to the cluster
            connected_ = true;
        } else {
            // Handle read error or end of stream
            if (!shutdown_) {
                LOG(WARNING) << "Cluster status stream ended, reconnecting...";
                
                // In a production implementation, we would implement reconnection logic here
                // For now, wait a bit before trying again
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    
    // Finish the gRPC call
    grpc::Status status = reader->Finish();
    if (!status.ok() && !shutdown_) {
        LOG(ERROR) << "SubscribeToCluster failed: " << status.error_message();
    }
    
    VLOG(3) << "Cluster status subscription thread exiting";
}

void Subscriber::ClusterProbeLoop() {
    VLOG(2) << "Starting cluster probe loop";
    
    // Prepare client info for requests
    heartbeat_system::ClientInfo client_info;
    heartbeat_system::ClusterStatus cluster_status;
    
    {
        absl::MutexLock lock(&mutex_);
        for (const auto& it : nodes_) {
            client_info.add_nodes_info(it.first);
        }
    }
    
    // Periodically poll for cluster status
    while (!shutdown_) {
        // Create new context for each request
        grpc::ClientContext context;
        grpc::Status status = stub_->GetClusterStatus(&context, client_info, &cluster_status);
        
        if (status.ok()) {
            // If not already connected to head, add connection
            if (!connected_) {
                CreateAConnection(0, nodes_[0]);
            }
            
            connected_ = true;
            
            // Process removed nodes
            const auto& removed_nodes = cluster_status.removed_nodes();
            if (!removed_nodes.empty()) {
                absl::MutexLock lock(&mutex_);
                
                // Handle broker failures
                for (const auto& id : removed_nodes) {
                    LOG(WARNING) << "Broker " << id << " has failed";
                    nodes_.erase(id);
                    RemoveNodeFromClientInfo(client_info, id);
                }
            }
            
            // Process new nodes
            const auto& new_nodes = cluster_status.new_nodes();
            if (!new_nodes.empty()) {
                absl::MutexLock lock(&mutex_);
                
                // Add new brokers
                for (const auto& addr : new_nodes) {
                    int broker_id = GetBrokerId(addr);
                    
                    if (nodes_.find(broker_id) != nodes_.end()) {
                        continue;
                    }
                    
                    VLOG(2) << "New broker reported: " << broker_id << " at " << addr;
                    nodes_[broker_id] = addr;
                    client_info.add_nodes_info(broker_id);
                    CreateAConnection(broker_id, addr);
                }
            }
        } else {
            LOG(WARNING) << "Failed to get cluster status: " << status.error_message()
                         << ". Head may be down, trying to reach other brokers.";
            
            // In a production implementation, we would attempt to connect to other brokers
            // and establish a new connection to the newly elected head
        }
        
        // Wait before next poll
        std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL));
    }
    
    VLOG(3) << "Cluster probe loop exiting";
}
