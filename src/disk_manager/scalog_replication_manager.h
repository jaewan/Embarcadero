#pragma once

#include "common/config.h"
#include "../cxl_manager/cxl_datastructure.h"
#include <thread>
#include <scalog_sequencer.grpc.pb.h>

using Embarcadero::TInode;

// Forward declarations
namespace grpc
{
    class Server;
}

namespace Scalog
{

    class ScalogReplicationServiceImpl;

    class ScalogReplicationManager
    {
    public:
        ScalogReplicationManager(int broker_id,
                                 bool log_to_memory,
                                 const std::string &address = "localhost",
                                 const std::string &port = "",
                                 const std::string &log_file = "");
        ~ScalogReplicationManager();

        // Prevent copying
        ScalogReplicationManager(const ScalogReplicationManager &) = delete;
        ScalogReplicationManager &operator=(const ScalogReplicationManager &) = delete;

        // Wait for the server to shutdown
        void Wait();

        // Explicitly shutdown the server
        void Shutdown();

        void StartSendLocalCut();

        // [[CXL_SCALOG]] Set up CXL-based replication: replica polls CXL directly.
        void StartCXLReplication(void *cxl_addr, TInode *tinode);

    private:
        std::unique_ptr<ScalogReplicationServiceImpl> service_;
        std::unique_ptr<grpc::Server> server_;
        std::thread server_thread_;
    };

} // End of namespace Scalog
