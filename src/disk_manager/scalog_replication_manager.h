#pragma once

#include "common/config.h"
#include <thread>
#include <memory>
#include <string>

// Forward declarations
namespace grpc {
    class Server;
}

namespace Scalog {

#define NUM_BROKERS 4

class ScalogReplicationServiceImpl;

class ScalogReplicationManager {
public:
    ScalogReplicationManager(const std::string& address = "localhost",
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

private:
    std::unique_ptr<ScalogReplicationServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
    std::thread server_thread_;
};

} // End of namespace Scalog
