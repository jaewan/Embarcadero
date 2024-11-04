#ifndef _NETWORK_MANAGER_H_
#define _NETWORK_MANAGER_H_

#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"
#include "absl/synchronization/mutex.h"
#include "absl/container/flat_hash_map.h"

#include "common/config.h"
#include "../disk_manager/disk_manager.h"
#include "../cxl_manager/cxl_manager.h"

namespace Embarcadero{

class CXLManager;
class DiskManager;

enum ClientRequestType {Publish, Subscribe};

struct NetworkRequest{
	int client_socket;
};

struct alignas(64) EmbarcaderoReq{
	uint32_t client_id;
	uint32_t ack;
	size_t num_msg; // at Sub: used as last offset  set to -2 as sentinel value, at Pub: used as num brokers
	void* last_addr; // Sub: address of last fetched msg
	uint32_t port;
	ClientRequestType client_req;
	char topic[32]; //This is to align thie struct as 64B
};

struct LargeMsgRequest{
	void* msg;
	size_t len;
};

struct SubscriberState{
	absl::Mutex mu;
	size_t last_offset;
	void* last_addr;
	bool initialized = false;
};

class NetworkManager{
	public:
		NetworkManager(int broker_id, int num_reqReceive_threads=NUM_NETWORK_IO_THREADS);
		~NetworkManager();

		void EnqueueRequest(struct NetworkRequest);
		void SetDiskManager(DiskManager* disk_manager){disk_manager_ = disk_manager;}
		void SetCXLManager(CXLManager* cxl_manager){cxl_manager_ = cxl_manager;}

	private:
		void ReqReceiveThread();
		void MainThread();
		void AckThread(char* topic, int ack_fd);
		void SubscribeNetworkThread(int , int, char*, int);

		folly::MPMCQueue<std::optional<struct NetworkRequest>> requestQueue_;
		folly::MPMCQueue<struct LargeMsgRequest> largeMsgQueue_;
		int broker_id_;
		std::vector<std::thread> threads_;
		int num_reqReceive_threads_;

		std::atomic<int> thread_count_{0};
		bool stop_threads_ = false;

		absl::flat_hash_map<size_t, int> ack_connections_; // <client_id, ack_sock>
		absl::Mutex ack_mu_;
		absl::Mutex sub_mu_;
		absl::flat_hash_map<int /* client_id */, std::unique_ptr<SubscriberState>> sub_state_;
		int ack_efd_;
		int ack_fd_ = -1;

		CXLManager *cxl_manager_;
		DiskManager *disk_manager_;
};

} // End of namespace Embarcadero
#endif // _NETWORK_MANAGER_H_
