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
	int client_socket;
};

class NetworkManager{
	public:
		NetworkManager(size_t queueCapacity, int num_receive_threads=NUM_IO_RECEIVE_THREADS, int num_ack_threads=NUM_IO_ACK_THREADS);
		~NetworkManager();
    void EnqueueAck(std::optional<struct NetworkRequest> req);
		void SetDiskManager(DiskManager* disk_manager){
			disk_manager_ = disk_manager;
		}
		void SetCXLManager(CXLManager* cxl_manager){
			cxl_manager_ = cxl_manager;
		}

	private:
    // Class encompasing the state and logic needed to serve a request.
  class CallData {
   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    CallData(PubSub::AsyncService* service, ServerCompletionQueue* cq, CXLManager *cxl_manager, DiskManager *disk_manager)
        : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE), cxl_manager_(cxl_manager), disk_manager_(disk_manager) {
      // Invoke the serving logic right away.
      Proceed();
    }

    void Proceed() {
      if (status_ == CREATE) {
        // Make this instance progress to the PROCESS state.
        std::cout << "Creating call data, asking for RPC" << std::endl;
        status_ = PROCESS;

        // As part of the initial CREATE state, we *request* that the system
        // start processing SayHello requests. In this request, "this" acts are
        // the tag uniquely identifying the request (so that different CallData
        // instances can serve different requests concurrently), in this case
        // the memory address of this CallData instance.
        service_->RequestPublish(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
      } else if (status_ == PROCESS) {
        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
        new CallData(service_, cq_, cxl_manager_, disk_manager_);

        PublishRequest req;
	      std::atomic<int> c{2};
        req.counter = &c;
        req.req = &request_;
        reply_.set_error(ERR_NO_ERROR);

        if (request_.acknowledge()) {
          // Wait for acknowlegment to respond
          status_ = ACKNOWLEDGE;
        } else {
          // If no acknowledgement is needed, respond now and signal this object ready for destruction
          status_ = FINISH;
          responder_.Finish(reply_, Status::OK, this);
        }
        // No matter what, we need to do processing tasks.
        //cxl_manager_->EnqueueRequest(req);
	      disk_manager_->EnqueueRequest(req);

      } else if (status_ == ACKNOWLEDGE) {
        std::cout << "Acknowledging the CallData() object" << std::endl;

        // And we are done! Let the gRPC runtime know we've finished, using the
        // memory address of this instance as the uniquely identifying tag for
        // the event.
        status_ = FINISH;
        responder_.Finish(reply_, Status::OK, this);
      } else {
        // If we asked for acknowledgement, we can destruct right after moving to FINISH
        // If we did not ask for acknowledgement, we will let cxl/disk to destroy the object
        // by calling the Finish() function.
        if (request_.acknowledge()) {
          Finish();
        }
      }
    }

    void Finish() {
        GPR_ASSERT(status_ == FINISH);
        std::cout << "Destructing CallData() object" << std::endl;
        // Once in the FINISH state, deallocate ourselves (CallData).
        delete this;
    }

   private:

    // The means of communication with the gRPC runtime for an asynchronous
    // server.
    PubSub::AsyncService* service_;
    // The producer-consumer queue where for asynchronous server notifications.
    ServerCompletionQueue* cq_;
    // Context for the rpc, allowing to tweak aspects of it such as the use
    // of compression, authentication, as well as to send metadata back to the
    // client.
    ServerContext ctx_;

    // What we get from the client.
    GRPCPublishRequest request_;
    // What we send back to the client.
    GRPCPublishResponse reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<GRPCPublishResponse> responder_;

    // Let's implement a tiny state machine with the following states.
    enum CallStatus { CREATE, PROCESS, ACKNOWLEDGE, FINISH };
    CallStatus status_;  // The current serving state.

    CXLManager *cxl_manager_;
		DiskManager *disk_manager_;
  };
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
