#include "network_manager.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <glog/logging.h>

namespace Embarcadero{

#define SKIP_LIST_SIZE 1024

NetworkManager::NetworkManager(size_t queueCapacity, int num_reqReceive_threads):
						 requestQueue_(queueCapacity),
						 ackQueue_(5000),
						 num_reqReceive_threads_(num_reqReceive_threads){
	// Create Network I/O threads
	threads_.emplace_back(&NetworkManager::MainThread, this);
	for (int i=0; i< NUM_ACK_THREADS; i++)
		threads_.emplace_back(&NetworkManager::AckThread, this);
	for (int i=0; i< num_reqReceive_threads; i++)
		threads_.emplace_back(&NetworkManager::ReqReceiveThread, this);

	//socketFdList = SkipList::createInstance(SKIP_LIST_SIZE);

	while(thread_count_.load() != (1 + NUM_ACK_THREADS + num_reqReceive_threads_)){}
	std::cout << "[NetworkManager]: \tCreated" << std::endl;
}

NetworkManager::~NetworkManager(){
	stop_threads_ = true;
	std::optional<struct NetworkRequest> sentinel = std::nullopt;
	ackQueue_.blockingWrite(sentinel);
	for (int i=0; i<num_reqReceive_threads_; i++)
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

void NetworkManager::ReqReceiveThread(){
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

				{
				//Handshake
				EmbarcaderoReq shake;
				int ret = read(req.client_socket, &shake, sizeof(shake));
				if(ret <=0)
					LOG(INFO) << "!!!!!!!!!!!!!!!! read shake error\n\n\n";
				VLOG(3) << "[DEBUG] Publish req shake";
				size_t READ_SIZE = shake.size;
					void* buf = malloc(READ_SIZE);
				while(true){
					//void* buf = malloc(READ_SIZE);

					VLOG(3) << "[DEBUG] start receving pub msg";
					int bytes_read = read(req.client_socket, buf, READ_SIZE);
					if(bytes_read <= 0){
						break;
					}
					VLOG(3) << "[DEBUG] publish message recved:" << bytes_read;
					while((size_t)bytes_read < sizeof(MessageHeader)){
						ret = read(req.client_socket, (uint8_t*)buf + bytes_read, READ_SIZE - bytes_read);
						if(ret <=0){
							perror("!!!!!!!!!!!!!!!! read error\n\n\n");
							return;
						}
						bytes_read += ret;
					}

					VLOG(3) << "[DEBUG] publish message recved:" << bytes_read;
					MessageHeader *header = (MessageHeader*)buf;
					// Finish Message
					if(header->client_id == -1){
						VLOG(3) << "[DEBUG] Finishing pub";
						free(buf);
						break;
					}
					//send(req.client_socket, "1", 1, 0);
					// Create publish request
					/*
					struct PublishRequest pub_req;
					pub_req.client_id = header->client_id;
					pub_req.client_order = header->client_order;
					pub_req.size = header->size;
					memcpy(pub_req.topic, shake.topic, 31);
					pub_req.acknowledge = shake.ack;
					pub_req.payload_address = (void*)buf;
					pub_req.counter = (std::atomic<int>*)malloc(sizeof(std::atomic<int>)); 
					pub_req.counter->store(2);
					pub_req.client_socket = req.client_socket;

					bool close = false;
					int to_read = (pub_req.size + sizeof(MessageHeader) - bytes_read);
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
					*/
				}
				//close(req.client_socket);
				}// end Receive
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
	char buf = '1';
	//static std::atomic<int> DEBUG_Ack_num[100] = {};

	while(!stop_threads_){
		ackQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		const struct NetworkRequest &req = optReq.value();
		//std::cout << "[DEBUG] socket:" << req.client_socket << std::endl;
		/*
		if(DEBUG_Ack_num[req.client_socket].fetch_add(1) == 9999){
			int ret = send(req.client_socket, &buf, 1, 0);
			std::cout << "Acked:" << req.client_socket << std::endl;
			if(ret <=0 )
				std::cout<< strerror(errno) << std::endl;
			close(req.client_socket);
		}
		*/
			int ret = send(req.client_socket, &buf, 1, 0);
			if(ret <=0 )
				std::cout<< strerror(errno) << std::endl;
	}
	//TODO(Jae) close socket. Should implemente a counter
	
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
