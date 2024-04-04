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

/*
class NetworkManager final {
 public:
  NetworkManager(size_t num_net_threads);
  ~NetworkManager();
  void Run(uint16_t port);

 private:
  // Class encompasing the state and logic needed to serve a request.
  class CallData {
   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    CallData(PubSub::AsyncService* service, ServerCompletionQueue* cq)
        : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE) {
      // Invoke the serving logic right away.
      Proceed();
    }

    void Proceed() {
      if (status_ == CREATE) {
        // Make this instance progress to the PROCESS state.
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
        new CallData(service_, cq_);

        // The actual processing.
        std::string prefix("Hello ");
        reply_.set_error(ERR_NO_ERROR);

        // And we are done! Let the gRPC runtime know we've finished, using the
        // memory address of this instance as the uniquely identifying tag for
        // the event.
        status_ = FINISH;
        responder_.Finish(reply_, Status::OK, this);
      } else {
        GPR_ASSERT(status_ == FINISH);
        // Once in the FINISH state, deallocate ourselves (CallData).
        delete this;
      }
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
    PublishRequest request_;
    // What we send back to the client.
    PublishResponse reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<PublishResponse> responder_;

    // Let's implement a tiny state machine with the following states.
    enum CallStatus { CREATE, PROCESS, FINISH };
    CallStatus status_;  // The current serving state.
  };

  // This can be run in multiple threads if needed.
  void HandleRpcs() {
    // Spawn a new CallData instance to serve new clients.
    new CallData(&service_, cq_.get());
    void* tag;  // uniquely identifies a request.
    bool ok;
    while (true) {
      // Block waiting to read the next event from the completion queue. The
      // event is uniquely identified by its tag, which in this case is the
      // memory address of a CallData instance.
      // The return value of Next should always be checked. This return value
      // tells us whether there is any kind of event or cq_ is shutting down.
      GPR_ASSERT(cq_->Next(&tag, &ok));
      GPR_ASSERT(ok);
      static_cast<CallData*>(tag)->Proceed();
    }
  }

  size_t num_net_threads_;
  std::unique_ptr<ServerCompletionQueue> cq_;
  PubSub::AsyncService service_;
  std::unique_ptr<Server> server_;
};
*/

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
};

} // End of namespace Embarcadero
#endif // _NETWORK_MANAGER_H_
