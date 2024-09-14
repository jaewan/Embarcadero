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
	largeMsgQueue_(10000),
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
					if(shake.ack == 2){
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

					// Create publish request
					struct PublishRequest pub_req;
					memcpy(pub_req.topic, shake.topic, TOPIC_NAME_SIZE);
					pub_req.acknowledge = shake.ack;
					pub_req.client_socket = ack_fd;

					BatchHeader batch_header;
					while(running){
						// give up if we can't at least read a partial batch header
						ssize_t bytes_read = recv(req.client_socket, &batch_header, sizeof(BatchHeader), 0);
						if(bytes_read <= 0){
							if(bytes_read < 0)
								LOG(ERROR) << "Receiving data: " << bytes_read << " ERROR:" << strerror(errno);
							running = false;
							break;
						}
						// finish reading batch header
						while(bytes_read < (ssize_t)sizeof(BatchHeader)){
							ssize_t recv_ret = recv(req.client_socket, (uint8_t*)(&batch_header) + bytes_read, sizeof(BatchHeader) - bytes_read, 0);
							if(recv_ret < 0){
								LOG(ERROR) << "Receiving data: " << recv_ret << " ERROR:" << strerror(errno);
								running = false;
								return;
							}
							bytes_read += recv_ret;
						}
						to_read = batch_header.total_size;
						pub_req.total_size = batch_header.total_size;
						// TODO(Jae) Send -1 to ack if this returns nullptr
						void* buf = cxl_manager_->GetCXLBuffer(pub_req);
						size_t read = 0;
						MessageHeader* header;
						size_t header_size = sizeof(MessageHeader);
						size_t bytes_to_next_header = 0;
						while(running){
							bytes_read = recv(req.client_socket, (uint8_t*)buf + read, to_read, 0);
							if(bytes_read < 0){
								LOG(ERROR) << "Receiving data: " << bytes_read << " ERROR:" << strerror(errno);
								running = false;
								return;
							}
              // TODO(Jae) Add validation logic here to check if the message headers are valid and send acknowledgement
							// We need this for ack=1 as well to confirm that the messages are valid
							while(bytes_to_next_header + header_size <= (size_t) bytes_read){
								header = (MessageHeader*)((uint8_t*)buf + read + bytes_to_next_header);
								header->complete = 1;
								bytes_read -= bytes_to_next_header;
								read += bytes_to_next_header;
								to_read -= bytes_to_next_header;
								bytes_to_next_header = header->paddedSize;
							}
							read += bytes_read;
							to_read -= bytes_read;
							bytes_to_next_header -= bytes_read;

							if(to_read == 0){
								break;
							}	
						}
					}
					close(req.client_socket);
				}// end Publish
				break;
			case Subscribe:
				{
					make_socket_non_blocking(req.client_socket);
					int sendBufferSize = 16 * 1024 * 1024;
					if (setsockopt(req.client_socket, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, sizeof(sendBufferSize)) == -1) {
						LOG(ERROR) << "Subscriber setsockopt SNGBUf failed";
						close(req.client_socket);
						return;
					}
					// Enable zero-copy
					int flag = 1;
					if (setsockopt(req.client_socket, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag)) < 0) {
						LOG(ERROR) << "Subscriber setsockopt(SO_ZEROCOPY) failed";
						close(req.client_socket);
						return;
					}

					int efd = epoll_create1(0);
					if(efd < 0){
						LOG(ERROR) << "Subscribe Thread epoll_create1 failed:" << strerror(errno);
						close(req.client_socket);
						return;
					}
					struct epoll_event event;
					event.data.fd = req.client_socket;
					event.events = EPOLLOUT;
					if(epoll_ctl(efd, EPOLL_CTL_ADD, req.client_socket, &event)){
						LOG(ERROR) << "epoll_ctl failed:" << strerror(errno);
						close(req.client_socket);
						close(efd);
					}

					uint16_t client_id = shake.client_id;
					{
						absl::MutexLock lock(&sub_mu_);
						if(!sub_state_.contains(client_id)){
							sub_state_.try_emplace(client_id, std::make_unique<SubscriberState>());
							sub_state_[client_id]->last_offset = shake.client_order;
							sub_state_[client_id]->last_addr = shake.last_addr;
							sub_state_[client_id]->initialized = true;
							LOG(ERROR) << "CREATED STATE FOR client_id=" << client_id;
							LOG(ERROR) << "Is created?? " << sub_state_.contains(client_id);
							printf("MY SUB_STATE prt=%p\n", &sub_state_);
						}
					}
					SubscribeNetworkThread(req.client_socket, efd, shake.topic, client_id);
					close(req.client_socket);
					close(efd);
				}//end Subscribe
				break;
		}
	}
}

// This implementation does not support multiple topics and dynamic message size.
// To make it support multiple topics, make some variables as a map (topic, var)
// Iterate over messages to make sure the large message parittion is not cutting the message in the middle
void NetworkManager::SubscribeNetworkThread(int sock, int efd, char* topic, uint16_t client_id){
	while(!stop_threads_){
		void* msg;
		size_t messages_size = 0;
		struct LargeMsgRequest req;
		size_t zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
		if(largeMsgQueue_.read(req)){
			msg = req.msg;
			messages_size = req.len;
		}else{
			if(!sub_state_.contains(client_id)) {
				printf("MY SUB_STATE prt=%p\n", &sub_state_);
				LOG(ERROR) << "WHY IS THIS HAPPENING?? client_id=" << client_id;
			}
			absl::MutexLock lock(&sub_state_[client_id]->mu);
			if(cxl_manager_->GetMessageAddr(topic, sub_state_[client_id]->last_offset, sub_state_[client_id]->last_addr, msg, messages_size)){
				while(messages_size > zero_copy_send_limit){
					struct LargeMsgRequest r;
					r.msg = msg;
					int mod = zero_copy_send_limit % ((MessageHeader*)msg)->paddedSize;
					r.len = zero_copy_send_limit - mod;
					largeMsgQueue_.blockingWrite(r);
					msg = (uint8_t*)msg + r.len;
					messages_size -= r.len;
				}
			}else{
				continue;;
			}
		}
		size_t sent_bytes = 0;
		if(messages_size < 64 && messages_size != 0){
			LOG(ERROR) << "[DEBUG] messages_size is below 64!!!! cannot happen " << messages_size;
		}
		while(sent_bytes < messages_size){
			struct epoll_event events[10];
			int n = epoll_wait(efd, events, 10, -1);
			if (n == -1) {
				LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
				close(sock);
				close(efd);
				return;
			}
			for (int i = 0; i < n; ++i) {
				if (events[i].events & EPOLLOUT) {
					size_t remaining_bytes = messages_size - sent_bytes;
					size_t to_send = std::min(remaining_bytes, zero_copy_send_limit);
					int ret;
					if(to_send < 1UL<<16)
						ret = send(sock, (uint8_t*)msg + sent_bytes, to_send, 0);
					else
						ret = send(sock, (uint8_t*)msg + sent_bytes, to_send, 0);
						//ret = send(sock, (uint8_t*)messages + sent_bytes, to_send, MSG_ZEROCOPY);
					if (ret > 0) {
							sent_bytes += ret;
							zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
					} else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
							zero_copy_send_limit = std::max(zero_copy_send_limit / 2, 1UL << 16); // Cap send limit at 64K
							continue;
					} else if (ret < 0) {
							LOG(ERROR) << "Error in sending messages: " << strerror(errno);
							close(sock);
							close(efd);
							return;
					}
				} else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
						LOG(INFO) << "Socket error or hang-up";
						close(sock);
						close(efd);
						return;
				}
			}
		}//end send loop
	}//end main while
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
			if(!optReq.has_value()){
				break;
			}
			//struct NetworkRequest &req = optReq.value();
			ack_count++;
		}
		int EPOLL_TIMEOUT = -1; 
		size_t acked_size = 0;
		while (acked_size < ack_count) {
			int n = epoll_wait(ack_efd_, events, 10, EPOLL_TIMEOUT);
			for (int i = 0; i < n; i++) {
				if (events[i].events & EPOLLOUT && acked_size < ack_count ) {
					ssize_t bytesSent = send(ack_fd_, buf, MIN(bufSize, ack_count - acked_size), 0);
					if (bytesSent < 0) {
						bytesSent = 0;
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
