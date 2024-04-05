#include "network_manager.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common/config.h"

namespace Embarcadero{

class NetworkManager::CallData {
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
		std::cout << "In CallData.Proceed() with status_ == " << status_ << std::endl;
      if (status_ == CREATE) {
        // Make this instance progress to the PROCESS state.
        std::cout << "Creating call data, asking for RPC" << std::endl;
        status_ = PROCESS;
		reply_.set_error(ERR_NO_ERROR);

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
		std::cout << "Creating new CallData object in CallData" << std::endl;
        new CallData(service_, cq_, cxl_manager_, disk_manager_);
        PublishRequest req;
	      std::atomic<int> c{2};
        req.counter = &c;
        req.req = &request_;

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

	void SetError(PublisherError err) {
		// Only overwrite error if currently a success
		if (reply_.error() == ERR_NO_ERROR) {
			reply_.set_error(err);
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


NetworkManager::NetworkManager(size_t queueCapacity, int num_receive_threads, int num_ack_threads):
						 requestQueue_(queueCapacity),
						 ackQueue_(50),
						 num_receive_threads_(num_receive_threads),
						 num_ack_threads_(num_ack_threads) {

	// Start by creating threads to acknowledge when messages are done being processed
	for (int i = 0; i < num_ack_threads; i++) {
		threads_.emplace_back(&NetworkManager::AckThread, this);
	}
	
	// Wait for all ack threads to spawn
	while (thread_count_.load() != num_ack_threads) {}

	// Create service
	// TODO(erika): make IP addr and port parameters
    ServerBuilder builder;
    builder.AddListeningPort(DEFAULT_CHANNEL, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
	
	// One completion queue per two receive threads. This is recommended in grpc perf docs
	for (int i = 0; i < num_receive_threads / 2 + num_receive_threads % 2; i++) {
		std::cout << "[NetworkManager]: Created completion queue " << i << std::endl;
    	cqs_.push_back(builder.AddCompletionQueue());
	}
    server_ = builder.BuildAndStart();
    std::cout << "[NetworkManager]: Server listening on " << DEFAULT_CHANNEL << std::endl;

	// Create receive threads to process received gRPC messages
	for (int i = 0; i < num_receive_threads; i++) {
		threads_.emplace_back(&NetworkManager::ReceiveThread, this);
	}

	// Wait for the threads to all start
	while (thread_count_.load() != num_receive_threads + num_ack_threads) {}
	std::cout << "[NetworkManager]: \tAll threads created!" << std::endl;
}

NetworkManager::~NetworkManager() {

	// We need to stop the receivers before we stop the ack queues
	// Shutdown the gRPC server
    server_->Shutdown();

    // Always shutdown the completion queue after the server.
	for (size_t i = 0; i < cqs_.size(); i++) {
    	cqs_[i]->Shutdown();
	}

	// Notify threads we would like to stop
	stop_threads_ = true;

	// Write a nullopt to the ack queue, so the ack threads can finish flushing the queue
	std::optional<struct NetworkRequest> sentinel = std::nullopt;
	for (int i = 0; i < num_ack_threads_; i++) {
		EnqueueAck(sentinel);
	}

	/*
	for (int i=0; i< NUM_BUFFERS; i++) {
		free(buffers_[i]);
	}
	*/

	// Wait for all threads to terminate
	for(std::thread& thread : threads_) {
		if(thread.joinable()){
			thread.join();
		}
	}

	std::cout << "[NetworkManager]: \tDestructed" << std::endl;
}

//Currently only for ack
void NetworkManager::EnqueueAck(std::optional<struct NetworkRequest> req) {
	ackQueue_.blockingWrite(req);
}

void NetworkManager::Proceed(void *grpcTag) {
	static_cast<CallData*>(grpcTag)->Proceed();
}

void NetworkManager::SetError(void *grpcTag, PublisherError err) {
	static_cast<CallData*>(grpcTag)->SetError(ERR_NO_ERROR);
}

/*
#define READ_SIZE 1024
#define MSG_SIZE 1000000
struct EmbarcaderoReq{
	size_t client_order;
	char topic[32];
	size_t ack;
	size_t size;
};
char JaeDebugBuf[1024];
std::chrono::high_resolution_clock::time_point start;
*/

void NetworkManager::ReceiveThread() {
	int recv_thread_id = thread_count_.fetch_add(1, std::memory_order_relaxed) - num_ack_threads_;
	int my_cq_index = recv_thread_id / 2;
	std::cout << "[Network Manager]: \tStarting Receive I/O Thread " << recv_thread_id << " with cq " << my_cq_index << std::endl;

	// Spawn a new CallData instance to serve new clients.
	if (!stop_threads_) {
    	new CallData(&service_, cqs_[my_cq_index].get(), cxl_manager_, disk_manager_);
    	void* tag;  // uniquely identifies a request.
    	bool ok;
    	while (!stop_threads_) {
      		// Block waiting to read the next event from the completion queue. The
      		// event is uniquely identified by its tag, which in this case is the
      		// memory address of a CallData instance.
      		// The return value of Next should always be checked. This return value
      		// tells us whether there is any kind of event or cq is shutting down.
      		GPR_ASSERT(cqs_[my_cq_index]->Next(&tag, &ok));
			if (!ok) {
				std::cout << "[Network Manager]: \tTerminating Receive I/O Thread " << recv_thread_id << std::endl;
				return;
			}
      		static_cast<CallData*>(tag)->Proceed();
    	}
	}
}

void NetworkManager::AckThread() {
	std::cout << "[Network Manager]: \tStarting Acknowledgement I/O Thread" << std::endl;
	thread_count_.fetch_add(1, std::memory_order_relaxed);

	std::optional<struct NetworkRequest> optReq;
	while(true) {
		ackQueue_.blockingRead(optReq);
		if(!optReq.has_value()) {
			// This should means we are trying to shutdown threads
			assert(stop_threads_ == true);
			std::cout << "[Network Manager]: \tTerminating Acknoweldgement I/O Thread" << std::endl;
			return;
		}
		
		std::cout << "[NetworkManager]: AckThread calling proceed on CallData" << std::endl;
		struct NetworkRequest net_req = optReq.value();
    	static_cast<CallData*>(net_req.grpcTag)->Proceed();
	}
}

/*
int NetworkManager::GetBuffer(){
	static std::atomic<int> counter{0};
	int off = counter.fetch_add(1, std::memory_order_relaxed) % NUM_BUFFERS;
	int zero = 0;
	while(1){
		if(buffers_counters_[off].compare_exchange_weak(zero, 2)){
			return off;
		}
		off = (off+1) % NUM_BUFFERS;
	}
}
*/

} // End of namespace Embarcadero
