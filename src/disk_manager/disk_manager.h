#ifndef INCLUDE_DISK_MANGER_H_
#define INCLUDE_DISK_MANGER_H_

#include <filesystem>
#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"
#include "common/config.h"
#include "../network_manager/network_manager.h"

namespace Embarcadero{

namespace fs = std::filesystem;
class NetworkManager;

struct ReplicationRequest{
	TInode* tinode;
	int fd;
	int broker_id;
};

class DiskManager{
	public:
		DiskManager(size_t queueCapacity, int broker_id, void* cxl_manager, 
						int num_io_threads=NUM_DISK_IO_THREADS, bool log_to_memory=false);
		~DiskManager();
		void SetNetworkManager(NetworkManager* network_manager){network_manager_ = network_manager;}
		// Current Implementation strictly requires the active brokers to be MAX_BROKER_NUM
		// Change this to get real-time num brokers
		void Replicate(TInode* TInode_addr, int replication_factor);

	private:
		void DiskIOThread();
		bool GetMessageAddr(TInode* tinode, int order, int broker_id, size_t &last_offset,
			void* &last_addr, void* &messages, size_t &messages_size);

		std::vector<std::thread> threads_;
		folly::MPMCQueue<std::optional<struct ReplicationRequest>> requestQueue_;

		NetworkManager *network_manager_;

		void* cxl_addr_;
		std::atomic<int> offset_{0};
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};
		int broker_id_;
		int num_io_threads_;
		bool log_to_memory_;
		void* logs_[NUM_MAX_BROKERS];
		fs::path prefix_path_;
};

} // End of namespace Embarcadero
#endif
