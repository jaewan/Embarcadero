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
#include "../embarlet/req_queue.h"
#include "../embarlet/ack_queue.h"

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;

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
		NetworkManager(std::shared_ptr<AckQueue> ack_queue, std::shared_ptr<ReqQueue> cxl_req_queue, std::shared_ptr<ReqQueue> disk_req_queue, int num_receive_threads=NUM_IO_RECEIVE_THREADS, int num_ack_threads=NUM_IO_ACK_THREADS);
		~NetworkManager();

	private:
		void ReceiveThread();
		void AckThread();

		std::vector<std::thread> threads_;
    int num_receive_threads_;
    int num_ack_threads_;

		std::shared_ptr<AckQueue> ackQueue_;
		std::shared_ptr<ReqQueue> reqQueueCXL_;
		std::shared_ptr<ReqQueue> reqQueueDisk_;

		std::atomic<int> thread_count_{0};
		bool stop_threads_ = false;

    std::vector<std::unique_ptr<ServerCompletionQueue>> cqs_;
    PubSub::AsyncService service_;
    std::unique_ptr<Server> server_;

		CXLManager *cxl_manager_;
		DiskManager *disk_manager_;
};

} // End of namespace Embarcadero
#endif // _NETWORK_MANAGER_H_
