#ifndef _NETWORK_MANAGER_H_
#define _NETWORK_MANAGER_H_

#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"

#include "common/config.h"
#include "../disk_manager/disk_manager.h"
#include "../cxl_manager/cxl_manager.h"

namespace Embarcadero{

class CXLManager;
class DiskManager;
enum NetworkRequestType {Acknowledge, Receive, Send};

struct NetworkRequest{
	NetworkRequestType req_type;
	int client_socket;
};

struct alignas(64) EmbarcaderoReq{
	size_t client_id;
	size_t client_order;
	size_t size;
	char topic[32]; //This is to align thie struct as 64B
	size_t ack;
};

class NetworkManager{
	public:
		NetworkManager(size_t queueCapacity, int num_io_threads=NUM_NETWORK_IO_THREADS);
		~NetworkManager();
		void EnqueueRequest(struct NetworkRequest);
		void SetDiskManager(DiskManager* disk_manager){
			disk_manager_ = disk_manager;
		}
		void SetCXLManager(CXLManager* cxl_manager){
			cxl_manager_ = cxl_manager;
		}

	private:
		folly::MPMCQueue<std::optional<struct NetworkRequest>> requestQueue_;
		folly::MPMCQueue<std::optional<struct NetworkRequest>> ackQueue_;
		void Network_io_thread();
		void MainThread();
		void AckThread();
		int GetBuffer();

		std::vector<std::thread> threads_;
		int num_io_threads_;
		std::atomic<int> thread_count_{0};
		bool stop_threads_ = false;

		CXLManager *cxl_manager_;
		DiskManager *disk_manager_;
};

} // End of namespace Embarcadero
#endif // _NETWORK_MANAGER_H_
