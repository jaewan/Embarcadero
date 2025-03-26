#include "common.h"

heartbeat_system::SequencerType parseSequencerType(const std::string& value) {
    static const std::unordered_map<std::string, heartbeat_system::SequencerType> sequencerMap = {
        {"EMBARCADERO", heartbeat_system::SequencerType::EMBARCADERO},
        {"KAFKA", heartbeat_system::SequencerType::KAFKA},
        {"SCALOG", heartbeat_system::SequencerType::SCALOG},
        {"CORFU", heartbeat_system::SequencerType::CORFU}
    };
    
    auto it = sequencerMap.find(value);
    if (it != sequencerMap.end()) {
        return it->second;
    }
    
    LOG(ERROR) << "Invalid SequencerType: " << value;
    throw std::runtime_error("Invalid SequencerType: " + value);
}

void RemoveNodeFromClientInfo(heartbeat_system::ClientInfo& client_info, int32_t node_to_remove) {
    auto* nodes_info = client_info.mutable_nodes_info();
    
    int write_idx = 0;
    int size = nodes_info->size();
    
    for (int read_idx = 0; read_idx < size; ++read_idx) {
        if (nodes_info->Get(read_idx) != node_to_remove) {
            if (write_idx != read_idx) {
                nodes_info->SwapElements(read_idx, write_idx);
            }
            write_idx++;
        }
    }
    
    // Remove all elements from write_idx to the end
    int elements_to_remove = size - write_idx;
    for (int i = 0; i < elements_to_remove; ++i) {
        nodes_info->RemoveLast();
    }
}

std::pair<std::string, int> ParseAddressPort(const std::string& input) {
    size_t colonPos = input.find(':');
    if (colonPos == std::string::npos) {
        throw std::invalid_argument("Invalid input format. Expected 'address:port'");
    }

    std::string address = input.substr(0, colonPos);
    std::string portStr = input.substr(colonPos + 1);

    int port;
    try {
        port = std::stoi(portStr);
    } catch (const std::exception& e) {
        throw std::invalid_argument("Invalid port number: " + portStr);
    }

    if (port < 0 || port > 65535) {
        throw std::out_of_range("Port number out of valid range (0-65535): " + portStr);
    }

    return std::make_pair(address, port);
}

int GetBrokerId(const std::string& input) {
    try {
        auto [addr, port] = ParseAddressPort(input);
        return port - PORT;
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to get broker ID from address: " << input << ", error: " << e.what();
        return -1;
    }
}

int GetNonblockingSock(char* broker_address, int port, bool send) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG(ERROR) << "Socket creation failed: " << strerror(errno);
        return -1;
    }

    // Set socket to non-blocking mode
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        LOG(ERROR) << "fcntl F_GETFL failed: " << strerror(errno);
        close(sock);
        return -1;
    }

    flags |= O_NONBLOCK;
    if (fcntl(sock, F_SETFL, flags) == -1) {
        LOG(ERROR) << "fcntl F_SETFL failed: " << strerror(errno);
        close(sock);
        return -1;
    }

    // Set socket options
    int flag = 1; // Enable options
    
    // Set SO_REUSEADDR to allow reusing the port
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        LOG(ERROR) << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno);
        close(sock);
        return -1;
    }

    // Set TCP_NODELAY to disable Nagle's algorithm
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0) {
        LOG(ERROR) << "setsockopt(TCP_NODELAY) failed: " << strerror(errno);
        close(sock);
        return -1;
    }

    // Configure buffer size based on send/receive mode
    int bufferSize = 16 * 1024 * 1024;  // 16 MB buffer
    
    if (send) {
        // Configure for sending
        if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) == -1) {
            LOG(ERROR) << "setsockopt(SO_SNDBUF) failed: " << strerror(errno);
            close(sock);
            return -1;
        }
        
        // Enable zero-copy for sending
        if (setsockopt(sock, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag)) < 0) {
            LOG(ERROR) << "setsockopt(SO_ZEROCOPY) failed: " << strerror(errno);
            close(sock);
            return -1;
        }
    } else {
        // Configure for receiving
        if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) == -1) {
            LOG(ERROR) << "setsockopt(SO_RCVBUF) failed: " << strerror(errno);
            close(sock);
            return -1;
        }
    }

    // Connect to the server
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, broker_address, &server_addr.sin_addr) <= 0) {
        LOG(ERROR) << "Invalid address: " << broker_address;
        close(sock);
        return -1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        if (errno != EINPROGRESS) {
            LOG(ERROR) << "Connect failed to " << broker_address << ":" << port 
                       << " - " << strerror(errno);
            close(sock);
            return -1;
        }
        // For non-blocking socket, EINPROGRESS is expected
    }

    return sock;
}

unsigned long default_huge_page_size() {
    FILE* f = fopen("/proc/meminfo", "r");
    unsigned long hps = 0;
    if (!f) {
        LOG(WARNING) << "Failed to open /proc/meminfo, using default huge page size";
        return 2 * 1024 * 1024; // Default to 2MB if /proc/meminfo can't be read
    }
    
    char* line = nullptr;
    size_t len = 0;
    ssize_t read;
    
    while ((read = getline(&line, &len, f)) != -1) {
        if (sscanf(line, "Hugepagesize: %lu kB", &hps) == 1) {
            hps *= 1024; // Convert from KB to bytes
            break;
        }
    }
    
    free(line);
    fclose(f);
    
    if (hps == 0) {
        LOG(WARNING) << "Failed to determine huge page size, using default";
        hps = 2 * 1024 * 1024; // Default to 2MB if not found
    }
    
    return hps;
}

void* mmap_large_buffer(size_t need, size_t& allocated) {
    void* buffer;
    size_t map_align = default_huge_page_size();
    
    // Align the needed size to the huge page size
    size_t aligned_size = ALIGN_UP(need, map_align);
    
    // First attempt: try to use huge pages
    buffer = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

    if (buffer == MAP_FAILED) {
        LOG(INFO) << "MAP_HUGETLB allocation failed, falling back to regular pages. "
                  << "Check /sys/kernel/mm/hugepages for optimal performance.";
        
        // Second attempt: use regular pages with pre-population
        buffer = mmap(NULL, need, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
                     
        if (buffer == MAP_FAILED) {
            LOG(ERROR) << "mmap failed: " << strerror(errno);
            throw std::runtime_error("Failed to allocate memory");
        }
        
        allocated = need;
    } else {
        allocated = aligned_size;
    }
    
    // Optional: try to lock the memory to prevent swapping
    // Disabled by default to avoid permission issues
    /*
    if (mlock(buffer, allocated) != 0) {
        LOG(WARNING) << "mlock failed: " << strerror(errno) 
                    << " - memory may be swapped";
    }
    */
    
    // Zero-initialize the buffer
    memset(buffer, 0, allocated);
    
    return buffer;
}

int GenerateRandomNum() {
    // Create a properly seeded random number generator
    static thread_local std::mt19937 gen(std::random_device{}());
    
    // Define distribution for the range [NUM_MAX_BROKERS, 999999]
    std::uniform_int_distribution<int> dist(NUM_MAX_BROKERS, 999999);
    
    return dist(gen);
}

bool CheckAvailableCores() {
    // Wait for 1 second to allow the process to be attached to the cgroup
    sleep(1);
    
    cpu_set_t mask;
    CPU_ZERO(&mask);

    if (sched_getaffinity(0, sizeof(mask), &mask) == -1) {
        LOG(ERROR) << "Failed to get CPU affinity: " << strerror(errno);
        return false;
    }

    // Count the available cores
    size_t num_cores = 0;
    std::vector<int> available_cores;
    
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &mask)) {
            num_cores++;
            available_cores.push_back(i);
        }
    }
    
    // Log the available cores
    std::ostringstream oss;
    oss << "Process can run on " << num_cores << " CPUs: ";
    for (size_t i = 0; i < available_cores.size(); ++i) {
        oss << available_cores[i];
        if (i < available_cores.size() - 1) {
            oss << ", ";
        }
    }
    LOG(INFO) << oss.str();
    
    return num_cores == CGROUP_CORE;
}
