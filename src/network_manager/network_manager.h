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
enum NetworkRequestType {Acknowledge, Receive, Send};

struct NetworkRequest{
	NetworkRequestType req_type;
	int client_socket;
	int efd;
};

struct alignas(64) EmbarcaderoReq{
	size_t client_id;
	size_t client_order;
	size_t ack;
	size_t size;
	char topic[32]; //This is to align thie struct as 64B
};

struct alignas(64) EmbarcaderoHandshake{
	size_t client_id;
	size_t client_order;
	size_t size; //Maximum size of messages in this batch
	char topic[32]; //This is to align thie struct as 64B
	size_t ack;
};

class NetworkManager{
	public:
		NetworkManager(size_t queueCapacity, int num_reqReceive_threads=NUM_NETWORK_IO_THREADS, bool test=false);
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
		std::vector<std::thread> threads_;
		int num_reqReceive_threads_;

		std::atomic<int> thread_count_{0};
		bool stop_threads_ = false;
		volatile bool test_acked_all_ = false;
		enum ackType{
			Batch,
			Immediate
		};

		//using SkipList= folly::ConcurrentSkipList<int>;
		//std::shared_ptr<SkipList> socketFdList;
		absl::flat_hash_map<size_t, int> ack_connections_; // <client_id, ack_sock>
		absl::Mutex ack_mu_;
		int ack_efd_;
		int ack_fd_;
		ackType ack_type_ = Immediate;

		CXLManager *cxl_manager_;
		DiskManager *disk_manager_;
};

} // End of namespace Embarcadero
#endif // _NETWORK_MANAGER_H_
