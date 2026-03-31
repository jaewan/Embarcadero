#pragma once

#include "common/config.h"
#include "../cxl_manager/cxl_datastructure.h"
#include <thread>
#include <scalog_sequencer.grpc.pb.h>

using Embarcadero::TInode;

// Forward declarations
namespace grpc {
    class Server;
}

namespace Scalog {

class ScalogReplicationServiceImpl;

class ScalogReplicationManager {
public:
    ScalogReplicationManager(int broker_id,
														bool log_to_memory,
                            const std::string& address = "localhost",
                            const std::string& port = "",
                            const std::string& log_file = "",
                            const std::string& sequencer_ip = "",
                            int sequencer_port = 0);
    ~ScalogReplicationManager();

    // Prevent copying
    ScalogReplicationManager(const ScalogReplicationManager&) = delete;
    ScalogReplicationManager& operator=(const ScalogReplicationManager&) = delete;

    // Wait for the server to shutdown
    void Wait();

    // Explicitly shutdown the server
    void Shutdown();

    void StartSendLocalCut();
    void StartCXLReplication(void* cxl_addr, TInode* tinode);
    void StartReplicaPollingThread(void* cxl_addr, TInode* tinode, int primary_broker_id, int replica_index);

private:
    std::unique_ptr<ScalogReplicationServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
    std::thread server_thread_;
    std::string base_dir_;
};

} // End of namespace Scalog
