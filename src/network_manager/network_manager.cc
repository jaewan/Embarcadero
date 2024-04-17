#include "network_manager.h"
#include "request_data.h"

#include <string>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <glog/logging.h>

namespace Embarcadero{

NetworkManager::NetworkManager(std::shared_ptr<AckQueue> ack_queue, std::shared_ptr<ReqQueue> cxl_req_queue,
                               std::shared_ptr<ReqQueue> disk_req_queue, int num_receive_threads, int num_ack_threads):
						 ackQueue_(ack_queue),
						 reqQueueCXL_(cxl_req_queue),
						 reqQueueDisk_(disk_req_queue),
						 num_ack_threads_(num_ack_threads) {

	// Start by creating threads to acknowledge when messages are done being processed
	for (int i = 0; i < num_ack_threads; i++) {
		threads_.emplace_back(&NetworkManager::AckThread, this);
	}
	// Wait for the threads to all start
	while (thread_count_.load() != num_ack_threads) {}

	// Create service
	// TODO: make IP addr and port parameters
 	std::string channel = DEFAULT_CHANNEL, ip, port;
    std::istringstream iss(channel);
    std::getline(iss, ip, ':'); // Extract IP
    std::getline(iss, port, ':'); // Extract port
    int port_num = std::stoi(port); // Convert port to integer
	std::string channels[NUM_CHANNEL];
    ServerBuilder builders[NUM_CHANNEL];

	for (int i=0 ; i < NUM_CHANNEL; i++){
		builders[i].AddListeningPort(ip+":"+std::to_string(port_num), grpc::InsecureServerCredentials());
		builders[i].RegisterService(&service_[i]);
		port_num++;
	}
	
	// Ensure the num_receive_threads is even
	num_receive_threads = num_receive_threads + num_receive_threads%2;
	// One completion queue per two receive threads. This is recommended in grpc perf docs
	for (int i = 0; i < num_receive_threads / 2 ; i++) {
    	cqs_.push_back(builders[i%NUM_CHANNEL].AddCompletionQueue());
	}
	for (int i=0 ; i < NUM_CHANNEL; i++){
		server_[i] = builders[i].BuildAndStart();
	}

	// Create receive threads to process received gRPC messages
	for (int i = 0; i < num_receive_threads; i++) {
		threads_.emplace_back(&NetworkManager::ReceiveThread, this);
	}
	
	// Wait for the threads to all start
	while (thread_count_.load() != num_receive_threads + num_ack_threads) {}
	LOG(INFO) << "[NetworkManager] Constructed!";
}

NetworkManager::~NetworkManager() {

	// We need to stop the receivers before we stop the ack queues
	// Shutdown the gRPC server
	for (int i=0 ; i < NUM_CHANNEL; i++){
    	server_[i]->Shutdown();
	}

    // Always shutdown the completion queue after the server.
	for (size_t i = 0; i < cqs_.size(); i++) {
    	cqs_[i]->Shutdown();
	}

	// Notify threads we would like to stop
	stop_threads_ = true;

	// Write a nullopt to the ack queue, so the ack threads can finish flushing the queue
	std::optional<void *> sentinel = std::nullopt;
	for (int i = 0; i < num_ack_threads_; i++) {
		EnqueueAck(ackQueue_, sentinel);
	}

	// Wait for all threads to terminate
	for(std::thread& thread : threads_) {

		if(thread.joinable()){
			thread.join();
		}
	}

	LOG(INFO) << "[NetworkManager] Destructed";
}

void NetworkManager::ReceiveThread() {
	int recv_thread_id = thread_count_.fetch_add(1, std::memory_order_relaxed) - num_ack_threads_;
	int my_cq_index = recv_thread_id / 2;
	LOG(INFO) << "Starting Receive I/O Thread " << recv_thread_id << " with cq " << my_cq_index;

	// Spawn a new RequestData instance to serve new clients.
	if (!stop_threads_) {
    	new RequestData(&service_[my_cq_index%NUM_CHANNEL], cqs_[my_cq_index].get(), reqQueueCXL_, reqQueueDisk_);
    	void* tag;  // uniquely identifies a request.
    	bool ok;
    	while (!stop_threads_) {
      		// Block waiting to read the next event from the completion queue. The
      		// event is uniquely identified by its tag, which in this case is the
      		// memory address of a RequestData instance.
      		// The return value of Next should always be checked. This return value
      		// tells us whether there is any kind of event or cq is shutting down.
      		GPR_ASSERT(cqs_[my_cq_index]->Next(&tag, &ok));
			if (!ok) {
				LOG(INFO) << "Terminating Receive I/O Thread " << recv_thread_id;
				return;
			}
      		static_cast<RequestData*>(tag)->Proceed();
    	}
	}
}

void NetworkManager::AckThread() {
	LOG(INFO) << "Starting Acknowledgement I/O Thread";
	thread_count_.fetch_add(1, std::memory_order_relaxed);

	std::optional<void *> optReq;
	while(true) {
		ackQueue_->blockingRead(optReq);
		if(!optReq.has_value()) {
			// This should means we are trying to shutdown threads
			assert(stop_threads_ == true);
			LOG(INFO) << "Terminating Acknoweldgement I/O Thread";
			return;
		}
		
		void *grpcTag = optReq.value();
		VLOG(3) << "Got net_req, tag=" << grpcTag;
    	static_cast<RequestData*>(grpcTag)->Proceed();
	}
}
  /*
#define READ_SIZE 1024

void NetworkManager::Network_io_thread(){

	thread_count_.fetch_add(1, std::memory_order_relaxed);
	std::optional<struct NetworkRequest> optReq;

	while(!stop_threads_){
		requestQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		const struct NetworkRequest &req = optReq.value();
		switch(req.req_type){
			case Receive:
				//TODO(Jae) define if its publish or subscribe
				//while(processed < batch_size){
				while(true){
					void* buf = malloc(BUFFER_SIZE);

					int bytes_read = read(req.client_socket, buf, READ_SIZE);
					if(bytes_read <= 0){
					 	std::cout << "\t\t !!!!!!! Bytes Read:"<< bytes_read << std::endl;
						break;
					}
					while((size_t)bytes_read < sizeof(EmbarcaderoReq)){
						int ret = read(req.client_socket, (uint8_t*)buf + bytes_read, READ_SIZE - bytes_read);
						if(ret <=0)
							perror("!!!!!!!!!!!!!!!! read error\n\n\n");
						bytes_read += ret;
					}
					struct EmbarcaderoReq *clientReq = (struct EmbarcaderoReq*)buf;
					// Create publish request
					struct PublishRequest pub_req;
					pub_req.client_id = clientReq->client_id;
					pub_req.client_order = clientReq->client_order;
					pub_req.size = clientReq->size;
					memcpy(pub_req.topic, clientReq->topic, 31);
					pub_req.acknowledge = clientReq->ack;
					pub_req.payload_address = (uint8_t*)buf;// + sizeof(EmbarcaderoReq);
					pub_req.counter = (std::atomic<int>*)malloc(sizeof(std::atomic<int>)); 

					// Transform EmbarcaderoReq at buf to MessageHeader
					struct MessageHeader *header = (MessageHeader*)buf;
					header->client_id = pub_req.client_id;
					header->client_order = pub_req.client_order;
					header->size = pub_req.size;
					header->paddedSize = 64 - (header->size % 64) + header->size + sizeof(MessageHeader);
					header->segment_header = nullptr;
					header->logical_offset = (size_t)-1; // Sentinel value
					header->next_message = nullptr;

					bool close = false;
					int to_read = (pub_req.size + sizeof(EmbarcaderoReq) - bytes_read);
					while(to_read){
						int ret = read(req.client_socket, (uint8_t*)buf + bytes_read, to_read);
						if(ret == 0){
							close = true;
							break;
						}
						to_read -= ret;
						bytes_read += ret;
					}
					if(close)
						break;

					cxl_manager_->EnqueueRequest(pub_req);
					disk_manager_->EnqueueRequest(pub_req);
					//processed++;
				}
				close(req.client_socket);
				break;
			case Send:
				std::cout << "Send" << std::endl;
				break;
		}
*/

} // End of namespace Embarcadero
