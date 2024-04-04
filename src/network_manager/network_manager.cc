#include "network_manager.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>

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

	// Create receive threads to process received gRPC messages
	for (int i = 0; i < num_receive_threads; i++) {
		threads_.emplace_back(&NetworkManager::ReceiveThread, this);
	}

	// Wait for the threads to all start
	while (thread_count_.load() != num_receive_threads + num_ack_threads) {}
	std::cout << "[NetworkManager]: \tAll threads created!" << std::endl;
}

NetworkManager::~NetworkManager() {
	// Notify threads we would like to stop
	stop_threads_ = true;

	// TODO: we need to stop the receivers before we stop the ack queues

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

void NetworkManager::ReceiveThread(){
	std::cout << "[Network Manager]: \tStarting Receive I/O Thread" << std::endl;
	thread_count_.fetch_add(1, std::memory_order_relaxed);

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
