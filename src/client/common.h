#pragma once

#include <iostream>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <vector>
#include <cstring>
#include <random>
#include <fstream>
#include <functional>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>

#include <grpcpp/grpcpp.h>
#include <cxxopts.hpp>
#include <glog/logging.h>
#include <mimalloc.h>
#include "absl/synchronization/mutex.h"
#include "folly/ProducerConsumerQueue.h"

#include "common/config.h"
#include "../cxl_manager/cxl_manager.h"
#include "corfu_client.h"
#include <heartbeat.grpc.pb.h>

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY    0x4000000
#endif

#define CORFU_SEQUENCER_ADDR "192.168.60.173:"
// Define if batch optimization is enabled
#define BATCH_OPTIMIZATION 1

using heartbeat_system::HeartBeat;
using heartbeat_system::SequencerType;

// Forward declarations
class CorfuSequencerClient;

/**
 * Parses string representation of SequencerType to enum value
 */
heartbeat_system::SequencerType parseSequencerType(const std::string& value);

/**
 * Structure to store message indices and timestamps
 */
struct msgIdx {
    int broker_id;
    size_t offset = 0;
    std::vector<std::pair<size_t, std::chrono::steady_clock::time_point>> timestamps;
    
    explicit msgIdx(int b) : broker_id(b) {}
};

/**
 * Removes a node from ClientInfo
 */
void RemoveNodeFromClientInfo(heartbeat_system::ClientInfo& client_info, int32_t node_to_remove);

/**
 * Parses address and port from a string in format "address:port"
 */
std::pair<std::string, int> ParseAddressPort(const std::string& input);

/**
 * Gets broker ID from address:port string
 */
int GetBrokerId(const std::string& input);

/**
 * Creates a non-blocking socket
 * @param broker_address The broker address to connect to
 * @param port The port to connect to
 * @param send If true, configures socket for sending, otherwise for receiving
 * @return Socket file descriptor or -1 on error
 */
int GetNonblockingSock(char* broker_address, int port, bool send = true);

/**
 * Gets the default huge page size from the system
 */
unsigned long default_huge_page_size(void);

/**
 * Macro to align a value up to the nearest multiple of align_to
 */
#define ALIGN_UP(x, align_to) (((x) + ((align_to)-1)) & ~((align_to)-1))

/**
 * Maps a large buffer using huge pages if possible
 * @param need The size needed
 * @param allocated Output parameter for the actual size allocated
 * @return Pointer to the allocated memory
 */
void* mmap_large_buffer(size_t need, size_t& allocated);

/**
 * Generates a random number for client IDs
 */
int GenerateRandomNum();

/**
 * Checks if Cgroup is successful by verifying available cores
 */
bool CheckAvailableCores();
