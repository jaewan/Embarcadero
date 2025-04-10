#pragma once

#include "common/config.h"
#include <thread>
#include <memory>
#include <string>
#include <scalog_sequencer.grpc.pb.h>

// Forward declarations
namespace grpc {
    class Server;
}

namespace Scalog {

#define NUM_BROKERS 4

class ScalogReplicationServiceImpl;

// TODO(Tony) Copied from cxl_manager's Message Header for now due to circular depedency issues.
struct alignas(64) ScalogMessageHeader{
	volatile size_t paddedSize; // This include message+padding+header size
	void* segment_header;
	size_t logical_offset;
	volatile unsigned long long int next_msg_diff; // Relative to message_header, not cxl_addr_
	volatile size_t total_order;
	size_t client_order;
	uint32_t client_id;
	volatile uint32_t complete;
	size_t size;
};

class ScalogReplicationManager {
public:
    ScalogReplicationManager(int broker_id,
                            const std::string& address = "localhost",
                            const std::string& port = "",
                            const std::string& log_file = "");
    ~ScalogReplicationManager();

    // Prevent copying
    ScalogReplicationManager(const ScalogReplicationManager&) = delete;
    ScalogReplicationManager& operator=(const ScalogReplicationManager&) = delete;

    // Wait for the server to shutdown
    void Wait();

    // Explicitly shutdown the server
    void Shutdown();

    void StartSendLocalCut();

private:
    std::unique_ptr<ScalogReplicationServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
    std::thread server_thread_;
};

} // End of namespace Scalog
