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
#define MIN(a, b) ((a) < (b) ? (a) : (b))

inline void make_socket_non_blocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) {
		perror("fcntl F_GETFL");
		return ;
	}

	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) == -1) {
		perror("fcntl F_SETFL");
		return ;
	}

	int flag = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
		perror("setsockopt(SO_REUSEADDR) failed");
		close(fd);
		return ;
	}
	if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)) != 0){
		perror("setsockopt error");
		close(fd);
		return;
	}
}

NetworkManager::NetworkManager(size_t queueCapacity, int broker_id, int num_reqReceive_threads):
	requestQueue_(queueCapacity),
	ackQueue_(10000),
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
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(thread_count_.load() + 16, &cpuset);
	pthread_t current_thread = pthread_self();
	if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
			LOG(ERROR) << "Error setting thread affinity" ;
	}
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
		switch(shake.client_req){
			case Publish:
				{
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
					//size_t READ_SIZE = shake.size;
					size_t READ_SIZE = ZERO_COPY_SEND_LIMIT;
					to_read = READ_SIZE;
					// Allow 4K buffer space as messages can go over batch size
					size_t buf_size = READ_SIZE + MAX_MSG_SIZE + sizeof(MessageHeader);
					void* buf = mi_malloc(buf_size);

					// Create publish request
					struct PublishRequest pub_req;
					memcpy(pub_req.topic, shake.topic, TOPIC_NAME_SIZE);
					pub_req.acknowledge = shake.ack;
					pub_req.client_socket = ack_fd;

					size_t read = 0;
					int num_msg = 0;
					MessageHeader *header = (MessageHeader*)buf;
					while(running){
						int bytes_read = recv(req.client_socket, (uint8_t*)buf + read, to_read, 0);
						if(bytes_read <= 0){
							if(bytes_read < 0)
								LOG(ERROR) << "Receiving data: " << bytes_read << " ERROR:" << strerror(errno);
							mi_free(buf);
							running = false;
							break;
						}
						read += bytes_read;
						size_t total_size = 0;
						if(read >= sizeof(MessageHeader)){
							while(read >= header->paddedSize){
								num_msg++;
								read -= header->paddedSize;
								total_size += header->paddedSize;
								if(read < sizeof(MessageHeader)){
									break;
								}
								header = (MessageHeader*)((uint8_t*)header + header->paddedSize);
							}
						}

						to_read -= bytes_read;

						if(num_msg > 0){
							pub_req.payload_address = (void*)buf;
							pub_req.num_messages = num_msg;
							pub_req.total_size = total_size;
							void *new_buf = mi_malloc(READ_SIZE);
							if(read){
								memcpy(new_buf, (uint8_t*)buf + total_size, read);
							}
							cxl_manager_->EnqueueRequest(pub_req);

							header = (MessageHeader*)new_buf;;
							buf = new_buf;
							to_read = READ_SIZE;
							num_msg = 0;
						}	
					}
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
	size_t bufSize = 1024*1024;
	char buf[bufSize];
	struct epoll_event events[10]; // Adjust size as needed

	while(!stop_threads_){
		size_t ack_count = 0;
		while(ackQueue_.read(optReq)){
			ack_count++;
			if(!optReq.has_value()){
				ack_count--;
				break;
			}
		}
		int EPOLL_TIMEOUT = -1; 
		size_t acked_size = 0;
		while (acked_size < ack_count) {
			int n = epoll_wait(ack_efd_, events, 10, EPOLL_TIMEOUT);
			for (int i = 0; i < n; i++) {
				if (events[i].events & EPOLLOUT && acked_size < ack_count ) {
					ssize_t bytesSent = send(ack_fd_, buf, MIN(bufSize, ack_count - acked_size), 0);
					if (bytesSent < 0) {
						if (errno != EAGAIN) {
							LOG(ERROR) << " Ack Send failed:" << strerror(errno) << " ack_fd:" << ack_fd_ << " ack size:" << (ack_count - acked_size);
							return;
						}
					} else {
						acked_size += bytesSent;
					}
				}
			}
		}
		// Check if the connection is alive only after ack_fd_ is connected
		if(ack_fd_ > 0){
			int result = recv(ack_fd_, buf, 1, MSG_PEEK | MSG_DONTWAIT);
			if(result == 0){
				LOG(INFO) << "Connection is closed ack_fd_:" << ack_fd_ ;
				//stop_threads_ = true;
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
