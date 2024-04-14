#include "network_manager.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace Embarcadero{

NetworkManager::NetworkManager(size_t queueCapacity, int num_io_threads):
						 requestQueue_(queueCapacity),
						 ackQueue_(50),
						 num_io_threads_(num_io_threads){
	// Create Network I/O threads
	threads_.emplace_back(&NetworkManager::MainThread, this);
	threads_.emplace_back(&NetworkManager::AckThread, this);
	threads_.emplace_back(&NetworkManager::AckThread, this);
	threads_.emplace_back(&NetworkManager::AckThread, this);
	for (int i=4; i< num_io_threads_; i++)
		threads_.emplace_back(&NetworkManager::Network_io_thread, this);

	while(thread_count_.load() != num_io_threads_){}
	std::cout << "[NetworkManager]: \tCreated" << std::endl;
}

NetworkManager::~NetworkManager(){
	stop_threads_ = true;
	std::optional<struct NetworkRequest> sentinel = std::nullopt;
	ackQueue_.blockingWrite(sentinel);
	for (int i=4; i<num_io_threads_; i++)
		requestQueue_.blockingWrite(sentinel);
	
	for(std::thread& thread : threads_){
		if(thread.joinable()){
			thread.join();
		}
	}
	std::cout << "[NetworkManager]: \tDestructed" << std::endl;
}

//Currently only for ack
void NetworkManager::EnqueueRequest(struct NetworkRequest req){
	ackQueue_.blockingWrite(req);
}

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
	}
}

void NetworkManager::AckThread(){
	std::optional<struct NetworkRequest> optReq;
	thread_count_.fetch_add(1, std::memory_order_relaxed);

	while(!stop_threads_){
		ackQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		const struct NetworkRequest &req = optReq.value();
	}
}

void NetworkManager::MainThread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);

	int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;
	
	 while (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        std::cerr << "Error binding socket" << std::endl;
		sleep(5);
    }

    listen(server_socket, 32);

    while (true) {
		struct NetworkRequest req;
		req.req_type = Receive;
        req.client_socket = accept(server_socket, nullptr, nullptr);
        if (req.client_socket < 0) {
            std::cerr << "Error accepting connection" << std::endl;
            continue;
        }
		//EnqueueRequest(req);
		requestQueue_.blockingWrite(req);
    }
}

} // End of namespace Embarcadero
