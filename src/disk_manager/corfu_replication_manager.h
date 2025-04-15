#pragma once

#include "common/config.h"
#include <thread>
#include <memory>
#include <string>

// Forward declarations
namespace grpc {
    class Server;
}

namespace Corfu {

class CorfuReplicationServiceImpl;

class CorfuReplicationManager {
public:
    CorfuReplicationManager(int broker_id,
														const std::string& address = "localhost",
                            const std::string& port = "",
                            const std::string& log_file = "");
    ~CorfuReplicationManager();

    // Prevent copying
    CorfuReplicationManager(const CorfuReplicationManager&) = delete;
    CorfuReplicationManager& operator=(const CorfuReplicationManager&) = delete;

    // Wait for the server to shutdown
    void Wait();

    // Explicitly shutdown the server
    void Shutdown();

private:
    std::unique_ptr<CorfuReplicationServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
    std::thread server_thread_;
};

} // End of namespace Corfu
