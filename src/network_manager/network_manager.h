#ifndef _NETWORK_MANAGER_H_
#define _NETWORK_MANAGER_H_

#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"
//#include "folly/ConcurrentSkipList.h"
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
	int efd;
};

struct alignas(32) SubscribeHeader{
	int broker_id;
	// Logical address of first and last msg
	int first_id; 
	int last_id;
	// Total len of payload
	size_t len;
};

struct alignas(64) EmbarcaderoReq{
	uint16_t client_id;
	uint32_t size;//Pub: Maximum size of messages in this batch
	size_t client_order; // at Sub: used as last offset  set to -2 as sentinel value
	void* last_addr; // Sub: address of last fetched msg
	uint16_t ack;
	uint32_t port;
	ClientRequestType client_req;
	char topic[32]; //This is to align thie struct as 64B
};

class NetworkManager{
	public:
		NetworkManager(size_t queueCapacity, int broker_id, int num_reqReceive_threads=NUM_NETWORK_IO_THREADS,
									 bool test=false);
		~NetworkManager();
		void EnqueueRequest(struct NetworkRequest);
		void SetDiskManager(DiskManager* disk_manager){
			disk_manager_ = disk_manager;
		}
		void SetCXLManager(CXLManager* cxl_manager){
			cxl_manager_ = cxl_manager;
		}

		void WaitUntilAcked(){
			while(!test_acked_all_){}
			return;
		}

	private:
		void ReqReceiveThread();
		void MainThread();
		void AckThread();
		void TestAckThread();
		int GetBuffer();

		folly::MPMCQueue<std::optional<struct NetworkRequest>> requestQueue_;
		folly::MPMCQueue<std::optional<struct NetworkRequest>> ackQueue_;
		int broker_id_;
		std::vector<std::thread> threads_;
		int num_reqReceive_threads_;

		std::atomic<int> thread_count_{0};
		bool stop_threads_ = false;
		volatile bool test_acked_all_ = false;

		//using SkipList= folly::ConcurrentSkipList<int>;
		//std::shared_ptr<SkipList> socketFdList;
		absl::flat_hash_map<size_t, int> ack_connections_; // <client_id, ack_sock>
		absl::Mutex ack_mu_;
		int ack_efd_;
		int ack_fd_ = -1;

		CXLManager *cxl_manager_;
		DiskManager *disk_manager_;
};

} // End of namespace Embarcadero
#endif // _NETWORK_MANAGER_H_
