#include "network_manager.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <glog/logging.h>
#include <cstring>
#include <errno.h>

namespace Embarcadero{

#define SKIP_LIST_SIZE 1024
#define MAX_EVENTS 10

inline void make_socket_non_blocking(int sfd) {
	int flags = fcntl(sfd, F_GETFL, 0);
	if (flags == -1) {
		perror("fcntl F_GETFL");
		return ;
	}

	flags |= O_NONBLOCK;
	if (fcntl(sfd, F_SETFL, flags) == -1) {
		perror("fcntl F_SETFL");
		return ;
	}
}

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
				// Set socket as non-blocking and epoll
				make_socket_non_blocking(req.client_socket);
				struct epoll_event event;
				int efd = req.efd;
				event.data.fd = req.client_socket;
				//TODO(Jae) add write after read test
				event.events = EPOLLIN;
				if(epoll_ctl(efd, EPOLL_CTL_ADD, req.client_socket, &event) == -1){
					std::cerr << "!!!!! Error epoll_ctl:" << strerror(errno) << std::endl;
					return;
				}

				struct epoll_event events[MAX_EVENTS]; // Adjust size as needed

				//Handshake
				EmbarcaderoReq shake;
				int i,n;
				size_t to_read = sizeof(shake);
				bool running = true;
				while(to_read > 0){
 					n = epoll_wait(efd, events, MAX_EVENTS, -1);
					for( i=0; i< n; i++){
						if((events[i].events & EPOLLIN) && events[i].data.fd == req.client_socket){
							int ret = read(req.client_socket, &shake, to_read);
							to_read -= ret;
							if(to_read == 0){
								if(i == n-1){
									n = epoll_wait(efd, events, MAX_EVENTS, -1);
								}
								break;
							}
						}
					}
				}

				VLOG(3) << "[DEBUG] Publish req shake";
				size_t READ_SIZE = shake.size;
				to_read = READ_SIZE;
				void* buf = malloc(READ_SIZE);

				while(running){
					for ( ; i < n; i++) {
						if((events[i].events & EPOLLIN)&& events[i].data.fd == req.client_socket){
								VLOG(3) << "Before recv";
							int bytes_read = recv(req.client_socket, (uint8_t*)buf + (READ_SIZE - to_read), to_read, 0);
								VLOG(3) << "Read:"<< bytes_read;
							if(bytes_read <= 0 && errno != EAGAIN){
								LOG(INFO) << "Receiving data ERROR:" << strerror(errno);
								break;
							}
							to_read -= bytes_read;
							size_t read = READ_SIZE - to_read;
							MessageHeader *header;
							if(read > sizeof(MessageHeader)){
								header = (MessageHeader*)buf;
								if(header->client_id == -1){
									free(buf);
									running = false;
								VLOG(3) << "Finish receiving pub messages";
									//TODO(Jae) we do not want to close it for ack
									epoll_ctl(efd, EPOLL_CTL_DEL, req.client_socket, nullptr);
									close(req.client_socket);
								}
							}
							if(to_read == 0){
								to_read = READ_SIZE;
								VLOG(3) << "Received a req" ;
								//TODO(Jae) enqueue
								//buf = malloc(READ_SIZE);
							}
						}
					}
								VLOG(3) << "before epoll_wait";
					n = epoll_wait(efd, events, 10, -1);
								VLOG(3) << "after epoll wait: " << n;
					i = 0;

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
	if(server_socket < 0){
		LOG(INFO) << "Socket Creation Failed";
	}
	int flag = 1;
	setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

	//make_socket_non_blocking(server_socket);

	struct sockaddr_in server_address;
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(PORT);
	server_address.sin_addr.s_addr = INADDR_ANY;
	
	while (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
		std::cerr << "!!!!! Error binding socket" << std::endl;
		sleep(5);
	}

  if(listen(server_socket, SOMAXCONN) == -1){
		std::cerr << "!!!!! Error Listen:" << strerror(errno) << std::endl;
		return;
	}

	// Create epoll instance
	int efd = epoll_create1(0);
	if (efd == -1) perror("epoll_create1");

	// Add server socket to epoll
	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = server_socket;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, server_socket, &event) == -1){
		perror("epoll_ctl");
		LOG(INFO) << "epoll_ctl Error" << strerror(errno);
	}
	struct epoll_event events[MAX_EVENTS];

  while (true) {
		int n = epoll_wait(efd, events, MAX_EVENTS, -1);
		for(int i=0; i< n; i++){
			if (events[i].data.fd == server_socket) {
				struct NetworkRequest req;
				req.req_type = Receive;
				req.efd = efd;
				struct sockaddr_in client_addr;
				socklen_t client_addr_len = sizeof(client_addr);
				req.client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
				if (req.client_socket < 0) {
						std::cerr << "!!!!! Error accepting connection:" << strerror(errno) << std::endl;
						break;
						continue;
				}
				//EnqueueRequest(req);
				requestQueue_.blockingWrite(req);
			}
		}
   }
}

} // End of namespace Embarcadero
