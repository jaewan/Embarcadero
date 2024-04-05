#ifndef _NETWORK_MANAGER_H_
#define _NETWORK_MANAGER_H_

#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"
#include <string>
#include "absl/strings/str_format.h"
#include <grpcpp/grpcpp.h>

#include "common/config.h"
#include <pubsub.grpc.pb.h>
#include "../client/client.h"
#include "../disk_manager/disk_manager.h"
#include "../cxl_manager/cxl_manager.h"

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;

namespace Embarcadero{

class CXLManager;
class DiskManager;
enum NetworkRequestType {Acknowledge, Receive, Send, Test};
struct NetworkRequest{
	NetworkRequestType req_type;
	void *grpcTag;
};

class NetworkManager{
	public:
		NetworkManager(size_t queueCapacity, int num_receive_threads=NUM_IO_RECEIVE_THREADS, int num_ack_threads=NUM_IO_ACK_THREADS);
		~NetworkManager();
    void EnqueueAck(std::optional<struct NetworkRequest> req);
	void Proceed(void *grpcTag);
	void SetError(void *grpcTag, PublisherError err);
		void SetDiskManager(DiskManager* disk_manager){
			disk_manager_ = disk_manager;
		}
		void SetCXLManager(CXLManager* cxl_manager){
			cxl_manager_ = cxl_manager;
		}
	// Class encompasing the state and logic needed to serve a request.
	class CallData;

	private:
		folly::MPMCQueue<std::optional<struct NetworkRequest>> requestQueue_;
		folly::MPMCQueue<std::optional<struct NetworkRequest>> ackQueue_;
		void ReceiveThread();
		void AckThread();
		//int GetBuffer();

		std::vector<std::thread> threads_;
    int num_receive_threads_;
    int num_ack_threads_;

		//char *buffers_[NUM_BUFFERS];
		//std::atomic<int> buffers_counters_[NUM_BUFFERS];

		CXLManager *cxl_manager_;
		DiskManager *disk_manager_;

		std::atomic<int> thread_count_{0};
		bool stop_threads_ = false;

    std::vector<std::unique_ptr<ServerCompletionQueue>> cqs_;
    PubSub::AsyncService service_;
    std::unique_ptr<Server> server_;
};

} // End of namespace Embarcadero
#endif // _NETWORK_MANAGER_H_
