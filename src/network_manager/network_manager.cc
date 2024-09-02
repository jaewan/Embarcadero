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
#include <chrono>
#include <errno.h>
#include "mimalloc.h"

namespace Embarcadero{

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

NetworkManager::NetworkManager(size_t queueCapacity, int broker_id, int num_reqReceive_threads):
	requestQueue_(queueCapacity),
	ackQueue_(10000000),
	broker_id_(broker_id),
	num_reqReceive_threads_(num_reqReceive_threads){
		// Create Network I/O threads
		threads_.emplace_back(&NetworkManager::MainThread, this);
		for (int i=0; i< NUM_ACK_THREADS; i++)
			threads_.emplace_back(&NetworkManager::AckThread, this);
		for (int i=0; i< num_reqReceive_threads; i++)
			threads_.emplace_back(&NetworkManager::ReqReceiveThread, this);

		while(thread_count_.load() != (1 + NUM_ACK_THREADS + num_reqReceive_threads_)){}
		LOG(INFO) << "\t[NetworkManager]: \tConstructed";
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
	LOG(INFO) << "[NetworkManager]: \tDestructed";
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
		struct sockaddr_in client_address;
		socklen_t client_address_len = sizeof(client_address);
		getpeername(req.client_socket, (struct sockaddr*)&client_address, &client_address_len);
#ifdef EPOLL
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
		}, messages;
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
#endif
		switch(shake.client_req){
			case Publish:
				{
#ifdef EPOLL
					size_t READ_SIZE = shake.size;
					to_read = READ_SIZE;
					void* buf = mi_malloc(READ_SIZE);

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
									pub_req.client_order = header->order;
									pub_req.payload_address = (void*)buf;
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
					if(strlen(shake.topic) == 0){
						LOG(ERROR) << "Topic cannot be null:" << shake.topic;
						return;
					}
					// TODO(Jae) This code asumes there's only one active client publishing
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
							server_addr.sin_port = ntohs(shake.port);
							server_addr.sin_addr.s_addr = inet_addr(inet_ntoa(client_address.sin_addr));
							if (connect(ack_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
								if (errno != EINPROGRESS) {
									perror("Connect failed");
									close(ack_fd);
									return;
								}
							}
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
					void* buf = mi_malloc(READ_SIZE);

					// Create publish request
					struct PublishRequest pub_req;
					pub_req.client_id = shake.client_id;
					memcpy(pub_req.topic, shake.topic, TOPIC_NAME_SIZE);
					pub_req.acknowledge = shake.ack;
					pub_req.client_socket = ack_fd;

					while(running){
						int bytes_read = recv(req.client_socket, (uint8_t*)buf + (READ_SIZE - to_read), to_read, 0);
						if(bytes_read <= 0){
							if(bytes_read < 0)
								LOG(ERROR) << "Receiving data: " << bytes_read << " ERROR:" << strerror(errno);
							mi_free(buf);
							running = false;
							break;
						}
						to_read -= bytes_read;
						//TODO(Jae) Change this to malloc here to allow dynamic message size during one connection
						if(to_read == 0){
							MessageHeader *header = (MessageHeader*)buf;
							pub_req.paddedSize = header->paddedSize;
							pub_req.order = header->client_order;
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
					close(req.client_socket);
				}// end Publish
				break;
			case Subscribe:
				{
					size_t last_offset = shake.client_order;
					void* last_addr = shake.last_addr;
					void* messages = nullptr;
					SubscribeHeader reply_shake;

					while(!stop_threads_){
						size_t messages_size = 0;

						if(cxl_manager_->GetMessageAddr(shake.topic, last_offset, last_addr, messages, messages_size)){
							reply_shake.len = messages_size;
							reply_shake.first_id = ((MessageHeader*)messages)->logical_offset;
							reply_shake.last_id = last_offset;
							reply_shake.broker_id = broker_id_;
							// Send
							VLOG(3) << "Sending " << messages_size << " first:" << reply_shake.first_id << " last:" << reply_shake.last_id;;
							size_t ret = send(req.client_socket, &reply_shake, sizeof(reply_shake), 0);
							if(ret < 0){
								LOG(ERROR) << "Error in sending reply_shake";
								break;
							}	
							ret = send(req.client_socket, messages, messages_size, 0);
							if(ret < 0){
								LOG(ERROR) << "Error in sending messages";
								break;
							}else if(ret < messages_size){
								// messages are too large to send in one call
								size_t total_sent = ret;
								while(total_sent < messages_size){
									ret = send(req.client_socket, (uint8_t*)messages+total_sent, messages_size-total_sent, 0);
									if(ret < 0){
										LOG(ERROR) << "Error in sending messages";
										return;
									}	
									total_sent += ret;
								}
							}
						}else{
							char c;
							if(recv(req.client_socket, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0){
								LOG(INFO) << "Subscribe Connection is closed :" << req.client_socket ;
								return ;
							}
							std::this_thread::yield();
						}
					}
				}//end Subscribe
				break;
		}
	}
}

// Current impl opens ack connection at publish and does not close it
void NetworkManager::AckThread(){
	std::optional<struct NetworkRequest> optReq;
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	struct AckResponse buf[128];
	struct epoll_event events[10]; // Adjust size as needed

	while(!stop_threads_){
		size_t ack_count = 0;
		// Can only read in size of max ack buff
		while(ackQueue_.read(optReq) && ack_count <= 128){
			if(!optReq.has_value()){
				break;
			} else {
				const struct NetworkRequest &req = optReq.value();
				buf[ack_count].success = req.success;
				buf[ack_count].order = req.order;
				ack_count++;
			}
		}

		int EPOLL_TIMEOUT = -1; 
		size_t total_bytes_sent = 0;
		size_t total_bytes_to_send = ack_count * sizeof(struct AckResponse);
		while (total_bytes_sent < total_bytes_to_send) {
			int n = epoll_wait(ack_efd_, events, 10, EPOLL_TIMEOUT);
			for (int i = 0; i < n; i++) {
				if (events[i].events & EPOLLOUT && total_bytes_sent < total_bytes_to_send) {
					ssize_t bytes_sent = send(ack_fd_, &(((char *)buf)[total_bytes_sent]), total_bytes_to_send - total_bytes_sent, 0);
					if (bytes_sent < 0) {
						if (errno != EAGAIN) {
							LOG(ERROR) << " Ack Send failed:";
							stop_threads_ = true;
							return;
						}
					} else {
						total_bytes_sent += bytes_sent;
					}
				}
			}
		}
		// Check if the connection is alive only after ack_fd_ is connected
		if(ack_fd_ > 0){
			int result = recv(ack_fd_, buf, 1, MSG_PEEK | MSG_DONTWAIT);
			if(result == 0){
				LOG(ERROR) << "Connection is closed ack_fd_:" << ack_fd_ ;
				stop_threads_ = true;
				break;
			}
		}
		if(ack_count > 0 && !optReq.has_value()) // Check ack_count >0 as the read is non-blocking
			break;
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
	server_address.sin_port = htons(PORT + broker_id_);
	server_address.sin_addr.s_addr = INADDR_ANY;

	while (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
		LOG(ERROR)<< "!!!!! Error binding socket:" << (PORT + broker_id_) << " broker_id: " << broker_id_;
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
				req.efd = efd;
				struct sockaddr_in client_addr;
				socklen_t client_addr_len = sizeof(client_addr);
				req.client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
				if (req.client_socket < 0) {
					std::cerr << "!!!!! Error accepting connection:" << strerror(errno) << std::endl;
					break;
					continue;
				}
				requestQueue_.blockingWrite(req);
			}
		}
	}
}

} // End of namespace Embarcadero
