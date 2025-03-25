#include "publisher.h"
#include <random>
#include <algorithm>
#include <fstream>

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
      sent_bytes_per_broker_(NUM_MAX_BROKERS) {
    
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
    
    // Wait for all threads to complete
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    if (cluster_probe_thread_.joinable()) {
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
    
    VLOG(3) << "Publisher successfully destructed";
}

void Publisher::Init(int ack_level) {
    ack_level_ = ack_level;
    
    // Generate unique port for acknowledgment server
    ack_port_ = GenerateRandomNum();
    
    // Start acknowledgment thread if needed
    if (ack_level >= 1) {
        VLOG(3) << "Initializing Publisher with ack_level: " << ack_level
                << ", ack_port: " << ack_port_;
        
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
    
    // Wait for connection to be established
    while (!connected_) {
        std::this_thread::yield();
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
    
    VLOG(2) << "Publisher initialization complete with " << num_threads_.load() << " threads";
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
    pubQue_.ReturnReads();
    
    // Wait for all messages to be sent
    VLOG(2) << "Polling for " << n << " messages to be published";
    while (client_order_ < n) {
        std::this_thread::yield();
    }
    
    // If acknowledgments are enabled, wait for all acks
    if (ack_level_ >= 1) {
        VLOG(2) << "Waiting for acknowledgments, received " << ack_received_ << " out of " << client_order_;
        while (ack_received_ < client_order_) {
            std::this_thread::yield();
        }
    }
    
    // Signal shutdown and cancel gRPC context
    shutdown_ = true;
    context_.TryCancel();
    
    // Wait for all publisher threads to finish
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    VLOG(2) << "Polling complete, all messages sent and acknowledged";
}

void Publisher::DEBUG_check_send_finish() {
    WriteFinished();
    publish_finished_ = true;
    pubQue_.ReturnReads();
    
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void Publisher::FailBrokers(size_t total_message_size, double failure_percentage, std::function<bool()> killbrokers) {
    VLOG(2) << "Setting up broker failure simulation at " << failure_percentage << " of total messages";
    
    measure_real_time_throughput_ = true;
    size_t num_brokers = nodes_.size();
    
    // Initialize counters for sent bytes
    for (size_t i = 0; i < num_brokers; i++) {
        sent_bytes_per_broker_[i].store(0);
    }
    
    // Start thread to monitor progress and kill brokers at specified percentage
    kill_brokers_thread_ = std::thread([=, this]() {
        size_t bytes_to_kill_brokers = total_message_size * failure_percentage;
        VLOG(2) << "Will kill brokers after sending " << bytes_to_kill_brokers << " bytes";
        
        while (!shutdown_ && total_sent_bytes_ < bytes_to_kill_brokers) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        if (!shutdown_) {
            LOG(INFO) << "Failure trigger reached (" << total_sent_bytes_ << " bytes sent), killing brokers";
            killbrokers();
        }
    });
    
    // Start thread to measure real-time throughput
    real_time_throughput_measure_thread_ = std::thread([=, this]() {
        std::vector<size_t> prev_throughputs(num_brokers, 0);
        std::vector<std::vector<size_t>> throughputs(num_brokers);
        
        // Open file for writing throughput data
				//TODO(Jae) Rewrite this to be relative path
        std::string filename("~/Embarcadero/data/failure/real_time_throughput.csv");
        std::ofstream throughputFile(filename);
        if (!throughputFile.is_open()) {
            LOG(ERROR) << "Failed to open file for writing throughput data: " << filename;
            return;
        }
        
        // Write CSV header
        for (size_t i = 0; i < num_brokers; i++) {
            throughputFile << i << ",";
        }
        throughputFile << "RealTimeThroughput\n";
        
        // Measuring loop
        const int measurement_interval_ms = 5;
        const double time_factor = 1000.0 / measurement_interval_ms; // For converting to per-second rate
        
        while (!shutdown_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(measurement_interval_ms));
            
            size_t sum = 0;
            for (size_t i = 0; i < num_brokers; i++) {
                size_t bytes = sent_bytes_per_broker_[i].load(std::memory_order_relaxed);
                size_t real_time_throughput = (bytes - prev_throughputs[i]);
                throughputs[i].emplace_back(real_time_throughput);
                
                // Convert to GB/s for CSV
                double gbps = (real_time_throughput * time_factor) / (1024.0 * 1024.0 * 1024.0);
                throughputFile << gbps << ",";
                
                sum += real_time_throughput;
                prev_throughputs[i] = bytes;
            }
            
            // Convert total to GB/s
            double total_gbps = (sum * time_factor) / (1024.0 * 1024.0 * 1024.0);
            throughputFile << total_gbps << "\n";
            throughputFile.flush();
        }
        
        throughputFile.close();
    });
}

void Publisher::WriteFinished() {
    pubQue_.WriteFinished();
    VLOG(3) << "WriteFinished signaled";
}

void Publisher::EpollAckThread() {
    if (ack_level_ < 1) {
        return;
    }
    
    VLOG(2) << "Starting acknowledgment server on port " << ack_port_;
    
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
    
    // Set up server address
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ack_port_);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    // Bind the socket
    if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        LOG(ERROR) << "Bind failed: " << strerror(errno);
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
    int max_events = NUM_MAX_BROKERS;
    std::vector<epoll_event> events(max_events);
    char buffer[1024 * 1024];  // 1MB buffer for receiving data
    size_t total_received = 0;
    int EPOLL_TIMEOUT = 1;  // 1 millisecond timeout
    std::vector<int> client_sockets;
    
    // Signal that initialization is complete
    thread_count_.fetch_add(1);
    
    // Map to track acknowledgments per socket for ack_level 1
    absl::flat_hash_map<int, size_t> ack1_per_sock;
    
    // Main epoll loop
    while (!shutdown_ || total_received < client_order_) {
        int num_events = epoll_wait(epoll_fd, events.data(), max_events, EPOLL_TIMEOUT);
        
        for (int i = 0; i < num_events; i++) {
            if (events[i].data.fd == server_sock) {
                // Handle new connection
                sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
                
                if (client_sock == -1) {
                    LOG(ERROR) << "Accept failed: " << strerror(errno);
                    continue;
                }
                
                // Set client socket to non-blocking mode
                int flags = fcntl(client_sock, F_GETFL, 0);
                if (fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
                    LOG(ERROR) << "Failed to set client socket to non-blocking: " << strerror(errno);
                    close(client_sock);
                    continue;
                }
                
                // Add client socket to epoll
                event.events = EPOLLIN | EPOLLET;  // Edge-triggered mode
                event.data.fd = client_sock;
                ack1_per_sock[client_sock] = 0;
                
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &event) == -1) {
                    LOG(ERROR) << "Failed to add client socket to epoll: " << strerror(errno);
                    close(client_sock);
                } else {
                    client_sockets.push_back(client_sock);
                    VLOG(3) << "New acknowledgment connection accepted from " 
                           << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port);
                }
            } else {
                // Handle data from existing connection
                int client_sock = events[i].data.fd;
                ssize_t bytes_received = 0;
                
                if (ack_level_ == 1) {
                    // For ack_level 1, receive logical offset
                    size_t acked_logical_id;
                    bytes_received = recv(client_sock, &acked_logical_id, sizeof(acked_logical_id), 0);
                    
                    if (bytes_received == sizeof(acked_logical_id)) {
                        // Update acknowledgment count
                        size_t previous_ack = ack1_per_sock[client_sock];
                        size_t new_acks = acked_logical_id - previous_ack;
                        total_received += new_acks;
                        ack1_per_sock[client_sock] = acked_logical_id;
                        ack_received_ = total_received;
                        
                        VLOG(4) << "Received ack for logical offset " << acked_logical_id 
                               << " (+" << new_acks << " new), total: " << total_received;
                    }
                } else {
                    // For other ack levels, receive count of acknowledged messages
                    bytes_received = recv(client_sock, buffer, sizeof(buffer), 0);
                    
                    if (bytes_received > 0) {
                        VLOG(3) << "Received " << bytes_received << " acknowledgment bytes";
                        total_received += bytes_received;
                        ack_received_ = total_received;
                    }
                }
                
                // Handle connection closure or errors
                if (bytes_received == 0) {
                    LOG(WARNING) << "Broker connection closed, total acks: " << total_received;
                } else if (bytes_received < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        LOG(ERROR) << "recv error: " << strerror(errno);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
                        close(client_sock);
                        
                        // Remove from client sockets list
                        auto it = std::find(client_sockets.begin(), client_sockets.end(), client_sock);
                        if (it != client_sockets.end()) {
                            client_sockets.erase(it);
                        }
                        
                        // Remove from ack tracking
                        ack1_per_sock.erase(client_sock);
                    }
                }
            }
        }
    }
    
    // Clean up client sockets
    for (int client_sock : client_sockets) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
        close(client_sock);
    }
    
    // Clean up epoll and server socket
    close(epoll_fd);
    close(server_sock);
    
    VLOG(2) << "Acknowledgment server shut down, received " << total_received << " acks";
}

void Publisher::PublishThread(int broker_id, int pubQuesIdx) {
    VLOG(2) << "Starting publisher thread " << pubQuesIdx << " for broker " << broker_id;
    
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
            
            auto [_addr, _port] = ParseAddressPort(it->second);
            addr = _addr;
            num_brokers = nodes_.size();
        }
        
        // Create socket
        sock = GetNonblockingSock(const_cast<char*>(addr.c_str()), PORT + brokerId);
        if (sock < 0) {
            LOG(ERROR) << "Failed to create socket to broker " << brokerId;
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
        memcpy(shake.topic, topic_, TOPIC_NAME_SIZE);
        shake.ack = ack_level_;
        shake.port = ack_port_;
        shake.num_msg = num_brokers;  // Using num_msg field to indicate number of brokers
        
        // Send handshake with epoll for non-blocking
        struct epoll_event events[10];
        bool running = true;
        size_t sent_bytes = 0;
        
        while (!shutdown_ && running) {
            int n = epoll_wait(efd, events, 10, -1);
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
                            running = false;
                            VLOG(3) << "Handshake complete with broker " << brokerId;
                            break;
                        }
                    }
                }
            }
        }
        
        return true;
    };
    
    // Connect to initial broker
    if (!connect_to_server(broker_id)) {
        LOG(ERROR) << "Failed to connect to broker " << broker_id;
        return;
    }
    
    // Signal thread is initialized
    thread_count_.fetch_add(1);
    
    // Track batch sequence for this thread
    size_t batch_seq = pubQuesIdx;
    
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
            if (publish_finished_) {
                VLOG(3) << "Publishing finished, exiting publisher thread " << pubQuesIdx;
                break;
            } else {
                // Short sleep to avoid busy waiting
                std::this_thread::sleep_for(std::chrono::microseconds(1));
                continue;
            }
        }
        
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
        } catch (const std::exception& e) {
            LOG(ERROR) << "Exception sending batch header: " << e.what();
            
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
                LOG(ERROR) << "Failed to connect to replacement broker " << new_broker_id;
                return;
            }
            
            try {
                send_batch_header();
            } catch (const std::exception& e) {
                LOG(ERROR) << "Failed to send batch header to replacement broker: " << e.what();
                return;
            }
            
            LOG(INFO) << "Thread " << pubQuesIdx << " redirected from broker:" << broker_id 
                      << " to broker:" << new_broker_id << " after failure";
            
            broker_id = new_broker_id;
        }
        
        // Send message data
        size_t sent_bytes = 0;
        size_t zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
        
        while (sent_bytes < len) {
            size_t remaining_bytes = len - sent_bytes;
            size_t to_send = std::min(remaining_bytes, zero_copy_send_limit);
            
            // Choose between regular send and zero-copy based on message size
            int send_flags = (to_send >= (1UL << 16)) ? MSG_ZEROCOPY : 0;
            
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
                
                // Reduce zero-copy size for backoff strategy
                zero_copy_send_limit = std::max(zero_copy_send_limit / 2, 1UL << 6);
            } else if (bytesSent < 0) {
                // Connection failure, switch to a different broker
                LOG(WARNING) << "Send failed to broker " << broker_id << ": " << strerror(errno);
                
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
                    LOG(ERROR) << "Failed to connect to replacement broker " << new_broker_id;
                    return;
                }
                
                // Reset and try again with new broker
                try {
                    send_batch_header();
                } catch (const std::exception& e) {
                    LOG(ERROR) << "Failed to send batch header to replacement broker: " << e.what();
                    return;
                }
                
                LOG(INFO) << "Thread " << pubQuesIdx << " redirected from broker:" << broker_id 
                          << " to broker:" << new_broker_id << " after failure";
                
                broker_id = new_broker_id;
                sent_bytes = 0;
            }
        }
        
        // Update batch sequence for next iteration
        batch_seq += num_threads_.load();
    }
    
    // Clean up resources
    if (sock >= 0) close(sock);
    if (efd >= 0) close(efd);
    
    VLOG(3) << "Publisher thread " << pubQuesIdx << " for broker " << broker_id << " exiting";
}

void Publisher::SubscribeToClusterStatus() {
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
    
    // Process cluster status updates
    while (!shutdown_) {
        if (reader->Read(&cluster_status)) {
            const auto& new_nodes = cluster_status.new_nodes();
            
            if (!new_nodes.empty()) {
                absl::MutexLock lock(&mutex_);
                
                // Adjust queue size based on number of brokers on first connection
                if (!connected_) {
                    int num_brokers = 1 + new_nodes.size();
                    queueSize_ /= num_brokers;
                    VLOG(2) << "Adjusted queue size to " << queueSize_ << " for " << num_brokers << " brokers";
                }
                
                // Add new brokers
                for (const auto& addr : new_nodes) {
                    int broker_id = GetBrokerId(addr);
                    nodes_[broker_id] = addr;
                    brokers_.emplace_back(broker_id);
                    
                    // Start publisher threads for this broker
                    if (!AddPublisherThreads(num_threads_per_broker_, broker_id)) {
                        LOG(ERROR) << "Failed to add publisher threads for broker " << broker_id;
                        return;
                    }
                    
                    VLOG(2) << "Added new broker: " << broker_id << " at " << addr;
                }
                
                // Sort brokers for deterministic round-robin assignment
                std::sort(brokers_.begin(), brokers_.end());
            }
            
            // If this is initial connection, handle head node
            if (!connected_) {
                // Connect to head node
                if (!AddPublisherThreads(num_threads_per_broker_, brokers_[0])) {
                    LOG(ERROR) << "Failed to add publisher threads for head broker";
                    return;
                }
                
                // Signal that we're connected
                connected_ = true;
                VLOG(2) << "Connected to cluster with head broker ID " << brokers_[0];
            }
        } else {
            // Handle read error or end of stream
            if (!shutdown_) {
                LOG(WARNING) << "Cluster status stream ended, reconnecting...";
                // Could implement reconnection logic here
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

bool Publisher::AddPublisherThreads(size_t num_threads, int broker_id) {
    VLOG(2) << "Adding " << num_threads << " publisher threads for broker " << broker_id;
    
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
