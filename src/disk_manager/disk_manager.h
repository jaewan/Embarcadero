#ifndef INCLUDE_DISK_MANGER_H_
#define INCLUDE_DISK_MANGER_H_

#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"
#include "common/config.h"
#include "../network_manager/network_manager.h"

namespace Embarcadero{

class NetworkManager;

class DiskManager{
	public:
		DiskManager(size_t queueCapacity, int broker_id, int num_io_threads=NUM_DISK_IO_THREADS);
		~DiskManager();
		void EnqueueRequest(struct PublishRequest);
		void SetNetworkManager(NetworkManager* network_manager){network_manager_ = network_manager;}
		// Current Implementation strictly requires the active brokers to be MAX_BROKER_NUM
		// Change this to get real-time num brokers
		void Replicate(TInode* TInode_addr, int replication_factor);

	private:
		void DiskIOThread();

		std::vector<std::thread> threads_;
		folly::MPMCQueue<std::optional<struct PublishRequest>> requestQueue_;

		NetworkManager *network_manager_;

		int log_fd_;
		std::atomic<int> offset_{0};
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};
		int broker_id_;
		int num_io_threads_;
};

} // End of namespace Embarcadero
#endif
