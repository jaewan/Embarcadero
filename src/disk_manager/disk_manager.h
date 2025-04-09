#ifndef INCLUDE_DISK_MANGER_H_
#define INCLUDE_DISK_MANGER_H_

#include <filesystem>
#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"
#include "common/config.h"
#include "../network_manager/network_manager.h"
#include "corfu_replication_manager.h"
#include "scalog_replication_manager.h"

namespace Embarcadero{

namespace fs = std::filesystem;
class NetworkManager;

struct ReplicationRequest{
	TInode* tinode;
	TInode* replica_tinode;
	int fd;
	int broker_id;
};

struct MemcpyRequest{
    void* addr;
    void* buf;
    size_t len;
};

class DiskManager{
	public:
		DiskManager(int broker_id, void* cxl_manager, bool log_to_memory,
								heartbeat_system::SequencerType sequencerType, size_t queueCapacity = 64);
		~DiskManager();
		void SetNetworkManager(NetworkManager* network_manager){network_manager_ = network_manager;}
		// Current Implementation strictly requires the active brokers to be MAX_BROKER_NUM
		// Change this to get real-time num brokers
		void Replicate(TInode* TInode_addr, TInode* replica_tinode, int replication_factor);
		void StartScalogReplicaLocalSequencer();

	private:
		void ReplicateThread();
		void CopyThread();
		bool GetMessageAddr(TInode* tinode, int order, int broker_id, size_t &last_offset,
			void* &last_addr, void* &messages, size_t &messages_size);

		std::vector<std::thread> threads_;
		folly::MPMCQueue<std::optional<struct ReplicationRequest>> requestQueue_;
		folly::MPMCQueue<std::optional<MemcpyRequest>> copyQueue_;
		int broker_id_;
		void* cxl_addr_;
		bool log_to_memory_;
		heartbeat_system::SequencerType sequencerType_;

		NetworkManager *network_manager_;
		std::unique_ptr<Corfu::CorfuReplicationManager> corfu_replication_manager_;
		std::unique_ptr<Scalog::ScalogReplicationManager> scalog_replication_manager_;

		std::atomic<int> offset_{0};
		bool stop_threads_ = false;
		std::atomic<size_t> thread_count_{0};
		std::atomic<size_t> num_io_threads_{0};
		std::atomic<size_t> num_active_threads_{0};
		fs::path prefix_path_;
};

} // End of namespace Embarcadero
#endif
