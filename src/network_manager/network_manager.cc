#include "network_manager.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "common/config.h"

namespace Embarcadero{

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
    	new CallData(&service_, cqs_[my_cq_index].get());
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

	// TODO: Implement this
	//cxl_manager_->EnqueueRequest(pub_req);
	//disk_manager_->EnqueueRequest(pub_req);
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

		// TODO(erika): do something with the message
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

/*
NetworkManager::NetworkManager(size_t num_net_threads) {
    num_net_threads_ = num_net_threads;
}

NetworkManager::~NetworkManager() {
    server_->Shutdown();
    // Always shutdown the completion queue after the server.
    cq_->Shutdown();
}

// TODO: There is no error shutdown handling in this code.
void NetworkManager::Run(uint16_t port) {
    // TODO: fix IP address
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    cq_ = builder.AddCompletionQueue();
    server_ = builder.BuildAndStart();
    std::cout << "Server listening on " << server_address << std::endl;

    // Proceed to the server's main loop.
    HandleRpcs();
}
*/
