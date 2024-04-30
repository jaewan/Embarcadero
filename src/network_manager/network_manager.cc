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
#include <sstream>
#include <errno.h>
#include "mimalloc.h"

namespace Embarcadero{

#define SKIP_LIST_SIZE 1024
#define MAX_EVENTS 10

//#define EPOLL 1

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

NetworkManager::NetworkManager(size_t queueCapacity, int num_reqReceive_threads, bool test):
						 requestQueue_(queueCapacity),
						 ackQueue_(10000000),
						 num_reqReceive_threads_(num_reqReceive_threads){
	// Create Network I/O threads
	threads_.emplace_back(&NetworkManager::MainThread, this);
	if(test){
		for (int i=0; i< NUM_ACK_THREADS; i++)
			threads_.emplace_back(&NetworkManager::TestAckThread, this);
	}else{
		for (int i=0; i< NUM_ACK_THREADS; i++)
			threads_.emplace_back(&NetworkManager::AckThread, this);
	}
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
				struct sockaddr_in client_address;
				socklen_t client_address_len = sizeof(client_address);
				getpeername(req.client_socket, (struct sockaddr*)&client_address, &client_address_len);
				// Set socket as non-blocking and epoll
#ifdef EPOLL
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

				struct epoll_event events[MAX_EVENTS]; 

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

				// Create publish request
				struct PublishRequest pub_req;
				pub_req.client_id = shake.client_id;
				memcpy(pub_req.topic, shake.topic, 31);
				pub_req.acknowledge = shake.ack;
				pub_req.client_socket = req.client_socket;

				while(running){
					for ( ; i < n; i++) {
						if((events[i].events & EPOLLIN)&& events[i].data.fd == req.client_socket){
							int bytes_read = recv(req.client_socket, (uint8_t*)buf + (READ_SIZE - to_read), to_read, 0);
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
								VLOG(3) << "Last message received:";
									free(buf);
									running = false;
									//TODO(Jae) we do not want to close it for ack
									epoll_ctl(efd, EPOLL_CTL_DEL, req.client_socket, nullptr);
									close(req.client_socket);
									break;
								}
							}
							if(to_read == 0){
								pub_req.size = header->size;
								pub_req.client_order = header->client_order;
								pub_req.payload_address = (void*)buf;
								pub_req.counter = (std::atomic<int>*)malloc(sizeof(std::atomic<int>)); 
								pub_req.counter->store(2);
								cxl_manager_->EnqueueRequest(pub_req);
								disk_manager_->EnqueueRequest(pub_req);

								buf = mi_malloc(READ_SIZE);
								to_read = READ_SIZE;
							}
						}
					}
					n = epoll_wait(efd, events, 10, -1);
					i = 0;
			}
#else
				//Handshake
				EmbarcaderoReq shake;
				size_t to_read = sizeof(shake);
				bool running = true;
				while(to_read > 0){
						int ret = recv(req.client_socket, &shake, to_read, 0);
						if(ret < 0){
							LOG(INFO) << "Error receiving shake:" << strerror(errno);
							return;
						}
						to_read -= ret;
						if(to_read == 0){
							break;
						}
				}

				VLOG(3) << "[DEBUG] Publish req shake finished";
				//TODO(Jae) This code asumes there's only one active client publishing
				// If there are parallel clients, change the ack queue
				int ack_fd = req.client_socket;
				if(shake.ack){
					absl::MutexLock lock(&ack_mu_);
					auto it = ack_connections_.find(shake.client_id);
					if(it != ack_connections_.end()){
						ack_fd = it->second;
					}else{
						int ack_fd = socket(AF_INET, SOCK_STREAM, 0);
						if (ack_fd < 0) {
							perror("Socket creation failed");
							return;
						}

						make_socket_non_blocking(ack_fd);

						int flag = 1;
						if (setsockopt(ack_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
								perror("setsockopt(SO_REUSEADDR) failed");
								close(ack_fd);
								return ;
						}
						if(setsockopt(ack_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)) != 0){
							perror("setsockopt error");
							close(ack_fd);
							return;
						}

						sockaddr_in server_addr; 
						memset(&server_addr, 0, sizeof(server_addr));
						server_addr.sin_family = AF_INET;
						server_addr.sin_family = AF_INET;
						server_addr.sin_port = ntohs(PORT+shake.client_id);
						server_addr.sin_addr.s_addr = inet_addr(inet_ntoa(client_address.sin_addr));
						VLOG(3) << "[DEBUG] Ack connecting to:" << (PORT + shake.client_id);
						if (connect(ack_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
							if (errno != EINPROGRESS) {
								perror("Connect failed");
								close(ack_fd);
								return;
							}
						}
						VLOG(3) << "[DEBUG] Ack connected to:" << (PORT + shake.client_id);
						ack_fd_ = ack_fd;
						ack_efd_ = epoll_create1(0);
						struct epoll_event event;
						event.data.fd = ack_fd;
						event.events = EPOLLOUT; 
						epoll_ctl(ack_efd_, EPOLL_CTL_ADD, ack_fd, &event);
						ack_connections_[shake.client_id] = ack_fd;
					}
				}
				size_t READ_SIZE = shake.size;
				to_read = READ_SIZE;
				void* buf = malloc(READ_SIZE);

				// Create publish request
				struct PublishRequest pub_req;
				pub_req.client_id = shake.client_id;
				memcpy(pub_req.topic, shake.topic, 31);
				pub_req.acknowledge = shake.ack;
				pub_req.client_socket = ack_fd;

				while(running){
						int bytes_read = recv(req.client_socket, (uint8_t*)buf + (READ_SIZE - to_read), to_read, 0);
						if(bytes_read <= 0){
							LOG(INFO) << "Receiving data ERROR:" << strerror(errno);
							break;
						}
						to_read -= bytes_read;
						size_t read = READ_SIZE - to_read;
						MessageHeader *header;
						if(read > sizeof(MessageHeader)){
							header = (MessageHeader*)buf;
							if(header->client_id == -1){
								VLOG(3) << "Last message received:";
								mi_free(buf);
								running = false;
								//TODO(Jae) we do not want to close it for ack
								close(req.client_socket);
								break;
							}
						}
						if(to_read == 0){
							pub_req.size = header->size;
							pub_req.client_order = header->client_order;
							pub_req.payload_address = (void*)buf;
							pub_req.counter = (std::atomic<int>*)mi_malloc(sizeof(std::atomic<int>)); 
							pub_req.counter->store(2);
							cxl_manager_->EnqueueRequest(pub_req);
							//disk_manager_->EnqueueRequest(pub_req);
							buf = mi_malloc(READ_SIZE);
							to_read = READ_SIZE;
						}
				}
#endif

				//close(req.client_socket);
				}// end Receive
				break;
			case Send:
				std::cout << "Send" << std::endl;
				break;
		}
	}
}

void NetworkManager::TestAckThread(){
	std::optional<struct NetworkRequest> optReq;
	thread_count_.fetch_add(1, std::memory_order_relaxed);

	VLOG(3) << "[DEBUG] Testack started" ;
	while(!stop_threads_){
		ackQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		if(ack_count_.fetch_add(1) == 9999999){
			test_acked_all_ = true;
		}
	VLOG(3) << "[DEBUG] acked:" << ack_count_ ;
	}
}
void NetworkManager::AckThread(){
	std::optional<struct NetworkRequest> optReq;
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	char buf[1000000];
	struct epoll_event events[10]; // Adjust size as needed

		VLOG(3) << "[DEBUG] ack started" ;
	while(!stop_threads_){
		size_t ack_count = 0;
		/*
		ackQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		const struct NetworkRequest &req = optReq.value();
		*/
		/*
		while(ackQueue_.read(optReq)){
			ack_count++;
			if(!optReq.has_value()){
				break;
			}
		}
		*/
		size_t DEBUG_num_total_ack = 10066329600/960;
		while(ack_count != DEBUG_num_total_ack){
			ackQueue_.blockingRead(optReq);
			if(!optReq.has_value()){
				break;
			}
			ack_count++;
		}
		size_t acked_size = 0;
		while (acked_size < 1) {
			int n = epoll_wait(ack_efd_, events, 10, -1);
			for (int i = 0; i < n; i++) {
				if (events[i].events & EPOLLOUT && acked_size < 1 ) {
					ssize_t bytesSent = send(ack_fd_, buf, 1, 0);
					if (bytesSent < 0) {
						if (errno != EAGAIN) {
							perror("Ack send failed");
							return;
							break;
						}
					} else {
						acked_size += bytesSent;
					}
				}
			}
		}
		/*
		size_t acked_size = 0;
		while (acked_size < ack_count) {
			int n = epoll_wait(ack_efd_, events, 10, -1);
			for (int i = 0; i < n; i++) {
				if (events[i].events & EPOLLOUT && acked_size < ack_count ) {
					ssize_t bytesSent = send(ack_fd_, buf, ack_count - acked_size, 0);
					if (bytesSent < 0) {
						if (errno != EAGAIN) {
							perror("Ack send failed");
							return;
							break;
						}
					} else {
						acked_size += bytesSent;
					}
				}
			}
		}
		*/
		if(ack_count > 0 && !optReq.has_value()){
		//if(!optReq.has_value()){
			break;
		}
	}
}

void NetworkManager::MainThread(){
	thread_count_.fetch_add(1, std::memory_order_relaxed);

	int server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(server_socket < 0){
		LOG(INFO) << "Socket Creation Failed";
	}
	int flag = 1;
	if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
			perror("setsockopt(SO_REUSEADDR) failed");
			close(server_socket);
			return ;
	}
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
