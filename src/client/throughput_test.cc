#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <vector>
#include <cstring>
#include <random>

#include <grpcpp/grpcpp.h>
#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>
#include <mimalloc.h>
#include "absl/synchronization/mutex.h"
#include "folly/ProducerConsumerQueue.h"

#include <heartbeat.grpc.pb.h>
#include "common/config.h"
#include "../cxl_manager/cxl_manager.h"

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY    0x4000000
#endif

using heartbeat_system::HeartBeat;
using heartbeat_system::SequencerType;

struct msgIdx{
	int broker_id;
	size_t offset = 0;
	std::vector<std::pair<size_t, std::chrono::steady_clock::time_point>> timestamps;
	msgIdx(int b):broker_id(b){}
};

void RemoveNodeFromClientInfo(heartbeat_system::ClientInfo& client_info, int32_t node_to_remove) {
	auto* nodes_info = client_info.mutable_nodes_info();
	int size = nodes_info->size();
	for (int i = 0; i < size; ++i) {
		if (nodes_info->Get(i) == node_to_remove) {
			// Remove this element by swapping it with the last element and then removing the last
			nodes_info->SwapElements(i, size - 1);
			nodes_info->RemoveLast();
			--size;
			--i;  // Recheck this index since we swapped elements
		}
	}
}

std::pair<std::string, int> ParseAddressPort(const std::string& input) {
	size_t colonPos = input.find(':');
	if (colonPos == std::string::npos) {
		throw std::invalid_argument("Invalid input format. Expected 'address:port'");
	}

	std::string address = input.substr(0, colonPos);
	std::string portStr = input.substr(colonPos + 1);

	int port;
	try {
		port = std::stoi(portStr);
	} catch (const std::exception& e) {
		throw std::invalid_argument("Invalid port number");
	}

	if (port < 0 || port > 65535) {
		throw std::out_of_range("Port number out of valid range (0-65535)");
	}

	return std::make_pair(address, port);
}

int GetBrokerId(const std::string& input) {
	auto [addr, addressPort] = ParseAddressPort(input);
	return addressPort - PORT;
}

int GetNonblockingSock(char *broker_address, int port, bool send = true){
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		LOG(ERROR) << "Socket creation failed";
		return -1;
	}

	int flags = fcntl(sock, F_GETFL, 0);
	if (flags == -1) {
		LOG(ERROR) << "fcntl F_GETFL";
		return -1;
	}

	flags |= O_NONBLOCK;
	if (fcntl(sock, F_SETFL, flags) == -1) {
		LOG(ERROR) << "fcntl F_SETFL";
		return -1;
	}

	int flag = 1; // Enable the option
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
		LOG(ERROR) << "setsockopt(SO_REUSEADDR) failed";
		close(sock);
		return -1;
	}

	if(setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)) != 0){
		LOG(ERROR) << "setsockopt error";
		close(sock);
		return -1;
	}

	int BufferSize = 16 * 1024 * 1024;
	if(send){
		if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &BufferSize, sizeof(BufferSize)) == -1) {
			LOG(ERROR) << "setsockopt SNDBUf failed";
			close(sock);
			return -1;
		}
		// Enable zero-copy
		if (setsockopt(sock, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag)) < 0) {
			LOG(ERROR) << "setsockopt(SO_ZEROCOPY) failed";
			close(sock);
			return -1;
		}
	}else{
		if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &BufferSize, sizeof(BufferSize)) == -1) {
			LOG(ERROR) << "setsockopt RCVBUf failed";
			close(sock);
			return -1;
		}
	}

	sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	server_addr.sin_addr.s_addr = inet_addr(broker_address);

	if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
		if (errno != EINPROGRESS) {
			LOG(ERROR) << "Connect failed to addr:" << broker_address << " " << strerror(errno);
			close(sock);
			return -1;
		}
	}

	return sock;
}

unsigned long default_huge_page_size(void){
	FILE *f = fopen("/proc/meminfo", "r");
	unsigned long hps = 0;
	size_t linelen = 0;
	char *line = NULL;

	if (!f)
		return 0;
	while (getline(&line, &linelen, f) > 0) {
		if (sscanf(line, "Hugepagesize:       %lu kB", &hps) == 1) {
			hps <<= 10;
			break;
		}
	}
	free(line);
	fclose(f);
	return hps;
}

#define ALIGN_UP(x, align_to)   (((x) + ((align_to)-1)) & ~((align_to)-1))

void *mmap_large_buffer(size_t need, size_t &allocated){
	void *buffer;
	size_t sz;
	size_t map_align = default_huge_page_size();
	/* Attempt to use huge pages if possible. */
	sz = ALIGN_UP(need, map_align);
	buffer = mmap(NULL, sz, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

	if (buffer == (void *)-1) {
		sz = need;
		buffer = mmap(NULL, sz, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,-1, 0);
		if (buffer != (void *)-1){
			LOG(INFO) <<"MAP_HUGETLB attempt failed, look at /sys/kernel/mm/hugepages for optimal performance";
		}else{
			LOG(ERROR) <<"mmap failed";
			exit(1);
		}
	}
	/*
		 if (mlock(buffer, sz) != 0) {
		 LOG(ERROR) << "mlock failed:" << strerror(errno);
		 }
		 */

	allocated = sz;
	memset(buffer, 0, sz);
	return buffer;
}

int GenerateRandomNum(){
	// Generate a random number
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(NUM_MAX_BROKERS, 999999);
	return  dis(gen);
}

class Buffer{
	public:
		Buffer(size_t num_buf, size_t total_buf_size, int client_id, size_t message_size):
			num_buf_(num_buf){
				bufs = (struct Buf*)malloc(sizeof(struct Buf) * num_buf);
				size_t allocated;
				// 4K is a buffer space as message can go over
				size_t buf_size = (total_buf_size / num_buf) + 4096; 
				for(size_t i=0; i < num_buf; i++){
					bufs[i].buffer = mmap_large_buffer(buf_size, allocated);
					bufs[i].tail = 0;
					bufs[i].head = 0;
					bufs[i].len = allocated;
				}
				header_.client_id = client_id;
				header_.size = message_size;
				header_.total_order = 0;
				int padding = message_size % 64;
				if(padding){
					padding = 64 - padding;
				}
				header_.paddedSize = message_size + padding + sizeof(Embarcadero::MessageHeader);
				header_.segment_header = nullptr;
				header_.logical_offset = (size_t)-1; // Sentinel value
				header_.next_msg_diff = 0;
				header_.complete = 0;
			}

		~Buffer(){
			for(size_t i=0; i < num_buf_; i++){
				munmap(bufs[i].buffer, bufs[i].len);
			}
			free(bufs);
		}

		bool Write(int bufIdx, size_t client_order, char* msg, size_t len){
			static const size_t header_size = sizeof(Embarcadero::MessageHeader);
			size_t padded;
			if(len == header_.size){
				padded = header_.paddedSize;
			}else{
				LOG(ERROR) << "Jae handle dynamic message sizes:" << len << " size:" << header_size;
				padded = len % 64;
				if(padded){
					padded = 64 - padded;
				}
				padded = len + padded + header_size;
				header_.paddedSize = padded;
				//header_.size = len;
			}
			header_.client_order = client_order;
			if(bufs[bufIdx].tail + header_size + padded > bufs[bufIdx].len){
				//LOG(ERROR) << "tail:" << bufs[bufIdx].tail << " write size:" << padded << " will go over buffer:" << bufs[bufIdx].len;
				return false;
			}
			memcpy((void*)((uint8_t*)bufs[bufIdx].buffer + bufs[bufIdx].tail), &header_, header_size);
			memcpy((void*)((uint8_t*)bufs[bufIdx].buffer + bufs[bufIdx].tail + header_size), msg, len);
			bufs[bufIdx].tail += padded;
			return true;
		}

		void* Read(int bufIdx, size_t &len ){
			while(!shutdown_ && bufs[bufIdx].tail <= bufs[bufIdx].head){
				std::this_thread::yield();
			}
			size_t head = bufs[bufIdx].head;
			size_t tail = bufs[bufIdx].tail;
			len = tail - head;
			bufs[bufIdx].head = tail;
			return (void*)((uint8_t*)bufs[bufIdx].buffer + head);
		}

		void ReturnReads(){
			shutdown_ = true;
		}

	private:
		struct Buf{
			void* buffer;
			size_t head;
			size_t tail;
			size_t len;
		} *bufs;
		size_t num_buf_;
		bool shutdown_ = false;
		Embarcadero::MessageHeader header_;
};

class Client{
	public:
		Client(std::string head_addr, std::string port, int num_threads, size_t message_size, size_t queueSize, bool fixed_batch_size):
			head_addr_(head_addr), port_(port), shutdown_(false), connected_(false), client_order_(0),
			client_id_(GenerateRandomNum()), num_threads_(num_threads), message_size_(message_size),
			pubQue_(num_threads, queueSize, client_id_, message_size), fixed_batch_size_(fixed_batch_size) {
				std::string addr = head_addr+":"+port;
				stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
				nodes_[0] = head_addr+":"+std::to_string(PORT);
				brokers_.emplace_back(0);
				cluster_probe_thread_ = std::thread([this](){
						this->ClusterProbeLoop();
						});
				while(!connected_){
					std::this_thread::yield();
				}
				LOG(INFO) << "Publisher Constructed";
			}

		~Client(){
			shutdown_ = true;
			cluster_probe_thread_.join();
			for(auto &t : threads_){
				if(t.joinable())
					t.join();
			}
			if(ack_thread_.joinable())
				ack_thread_.join();
			if(cluster_probe_thread_.joinable())
				cluster_probe_thread_.join();

			LOG(INFO) << "Destructed Client";
		};

		void Init(char topic[TOPIC_NAME_SIZE], int ack_level, int order){
			memcpy(topic_, topic, TOPIC_NAME_SIZE);
			ack_level_ = ack_level;
			order_ = order;
			ack_port_ = GenerateRandomNum();
			ack_thread_ = std::thread([this](){
					this->EpollAckThread();
					});

			// Setup sockets before creating threads, in case share across threads
			for (int pub_thread_id=0; pub_thread_id < num_threads_; pub_thread_id++) {
				int broker_id = pub_thread_id%brokers_.size();
				auto [addr, addressPort] = ParseAddressPort(nodes_[broker_id]);
				int sock = GetNonblockingSock(addr.data(), PORT + broker_id);
				// *********** Initiate Shake ***********
				int efd = epoll_create1(0);
				if(efd < 0){
					LOG(ERROR) << "Publish Thread epoll_create1 failed:" << strerror(errno);
					close(sock);
					return;
				}
				struct epoll_event event;
				event.data.fd = sock;
				event.events = EPOLLOUT;
				if(epoll_ctl(efd, EPOLL_CTL_ADD, sock, &event)){
					LOG(ERROR) << "epoll_ctl failed:" << strerror(errno);
					close(sock);
					close(efd);
				}

				Embarcadero::EmbarcaderoReq shake;
				shake.client_id = client_id_;
				memcpy(shake.topic, topic_, TOPIC_NAME_SIZE);
				shake.ack = ack_level_;
				shake.client_order = order_;
				shake.port = ack_port_;
				shake.size = message_size_ + sizeof(Embarcadero::MessageHeader);

				struct epoll_event events[10]; // Adjust size as needed
				bool running = true;
				size_t sent_bytes = 0;

				while (!shutdown_ && running) {
					int n = epoll_wait(efd, events, 10, -1);
					for (int i = 0; i < n; i++) {
						if (events[i].events & EPOLLOUT) {
							ssize_t bytesSent = send(sock, (int8_t*)(&shake) + sent_bytes, sizeof(shake) - sent_bytes, 0);
							if (bytesSent <= 0) {
								bytesSent = 0;
								if (errno != EAGAIN && errno != EWOULDBLOCK) {
									LOG(ERROR) << "send failed:" << strerror(errno);
									running = false;
									close(sock);
									close(efd);
									return;
								}
							}
							sent_bytes += bytesSent;
							if(sent_bytes == sizeof(shake)){
								running = false;
								break;
							}
						}
					}
				}
				pub_socks_.push_back(sock);
				pub_efd_.push_back(efd);
			}
			for (int pub_thread_id=0; pub_thread_id < num_threads_; pub_thread_id++) {
				threads_.emplace_back(&Client::PublishThread, this, pub_thread_id, pub_socks_[pub_thread_id], pub_efd_[pub_thread_id]);
			}

			while(thread_count_.load() != num_threads_){std::this_thread::yield();}
			return;
		}

		void Publish(char* message, size_t len){
			static size_t i = 0;
			static size_t j = 0;
			const static size_t batch_size = 1UL<<19;
			size_t n = batch_size/(len+64);
			pubQue_.Write(i, client_order_, message, len);
			j++;
			if(j == n){
				i = (i+1)%num_threads_;
				j = 0;
			}
			client_order_++;
		}

		void Poll(size_t n){
			pubQue_.ReturnReads();
			while(client_order_ < n){
				std::this_thread::yield();
			}
			for(auto &t : threads_){
				if(t.joinable())
					t.join();
			}
			shutdown_ = true;
			ack_thread_.join();
			return;
		}

		void DEBUG_check_send_finish(){
			LOG(INFO) << "DEBUG_check_send_finish called" ;
			pubQue_.ReturnReads();
			for(auto &t : threads_){
				if(t.joinable())
					t.join();
			}
			return;
		}

		bool CreateNewTopic(char topic[TOPIC_NAME_SIZE], int order, SequencerType seq_type){
			grpc::ClientContext context;
			heartbeat_system::CreateTopicRequest create_topic_req;;
			heartbeat_system::CreateTopicResponse create_topic_reply;;
			create_topic_req.set_topic(topic);
			create_topic_req.set_order(order);
			create_topic_req.set_sequencer_type(seq_type);
			grpc::Status status = stub_->CreateNewTopic(&context, create_topic_req, &create_topic_reply);
			if(status.ok()){
				return create_topic_reply.success();
			}
			return false;
		}

	private:
		std::string head_addr_;
		std::string port_;
		bool shutdown_;
		bool connected_;
		bool fixed_batch_size_;
		size_t client_order_;
		uint16_t client_id_;
		int num_threads_;
		size_t message_size_;
		Buffer pubQue_;

		std::unique_ptr<HeartBeat::Stub> stub_;
		std::thread cluster_probe_thread_;
		// <broker_id, address::port of network_mgr>
		absl::flat_hash_map<int, std::string> nodes_;
		std::vector<int> brokers_;
		absl::Mutex mutex_;
		char topic_[TOPIC_NAME_SIZE];
		int ack_level_;
		int order_;
		int ack_port_;
		std::vector<std::thread> threads_;
		
		std::vector<int> pub_socks_;
		std::vector<int> pub_efd_;

		std::thread ack_thread_;
		std::atomic<int> thread_count_{0};

		void EpollAckThread(){
			if(ack_level_ != 2)
				return;
			int server_sock = socket(AF_INET, SOCK_STREAM, 0);
			if (server_sock < 0) {
				LOG(ERROR) << "Socket creation failed";
				return;
			}

			int flag = 1;
			if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
				std::cerr << "setsockopt(SO_REUSEADDR) failed\n";
				close(server_sock);
				return;
			}

			setsockopt(server_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

			sockaddr_in server_addr;
			memset(&server_addr, 0, sizeof(server_addr));
			server_addr.sin_family = AF_INET;
			server_addr.sin_port = htons(ack_port_);
			server_addr.sin_addr.s_addr = INADDR_ANY;

			if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
				std::cerr << "Bind failed\n";
				close(server_sock);
				return;
			}

			if (listen(server_sock, SOMAXCONN) < 0) {
				std::cerr << "Listen failed\n";
				close(server_sock);
				return;
			}

			int epoll_fd = epoll_create1(0);
			if (epoll_fd == -1) {
				std::cerr << "Failed to create epoll file descriptor\n";
				close(server_sock);
				return;
			}

			epoll_event event;
			event.events = EPOLLIN;
			event.data.fd = server_sock;
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &event) == -1) {
				std::cerr << "Failed to add server socket to epoll\n";
				close(server_sock);
				close(epoll_fd);
				return;
			}

			int max_events = nodes_.size();
			std::vector<epoll_event> events(max_events);
			char buffer[1024*1024];
			size_t total_received = 0;
			int EPOLL_TIMEOUT = 1; // 1 millisecond timeout
			std::vector<int> client_sockets;

			while (!shutdown_ || total_received < client_order_) {
				int num_events = epoll_wait(epoll_fd, events.data(), max_events, EPOLL_TIMEOUT);
				for (int i = 0; i < num_events; i++) {
					if (events[i].data.fd == server_sock) {
						// New connection
						sockaddr_in client_addr;
						socklen_t client_addr_len = sizeof(client_addr);
						int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
						if (client_sock == -1) {
							std::cerr << "Accept failed\n";
							continue;
						}
						//Make client_sock non-blocking
						int flags = fcntl(client_sock, F_GETFL, 0);
						fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);
						event.events = EPOLLIN | EPOLLET;
						event.data.fd = client_sock;
						if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &event) == -1) {
							std::cerr << "Failed to add client socket to epoll\n";
							epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
							close(client_sock);
						} else {
							client_sockets.push_back(client_sock);
						}
					} else {
						// Data from existing client
						int client_sock = events[i].data.fd;
						ssize_t bytes_received = 0;

						while (total_received < client_order_ && (bytes_received = recv(client_sock, buffer, 1024*1024, 0)) > 0) {
							total_received += bytes_received;
							// Process received data here
							// For example, you might want to count the number of 1-byte messages:
							// message_count += bytes_received;
						}

						if (bytes_received == 0) {
							// Connection closed by client
							std::cout << "Client disconnected. Total bytes received: " << total_received << std::endl;
							epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
							close(client_sock);
							client_sockets.erase(std::remove(client_sockets.begin(), client_sockets.end(), client_sock), client_sockets.end());
						} else if (bytes_received == -1) {
							if (errno != EAGAIN && errno != EWOULDBLOCK) {
								// Error occurred
								std::cerr << "recv error: " << strerror(errno) << std::endl;
								epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
								close(client_sock);
								client_sockets.erase(std::remove(client_sockets.begin(), client_sockets.end(), client_sock), client_sockets.end());
							}
						}
					}
				}
			}

			// Close all remaining open client sockets
			for (int client_sock : client_sockets) {
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
				close(client_sock);
			}

			// Cleanup
			close(epoll_fd);
			close(server_sock);
		}

		void AckThread(){
			int server_sock = socket(AF_INET, SOCK_STREAM, 0);
			int flag = 1;
			if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
				LOG(ERROR) << "setsockopt(SO_REUSEADDR) failed";
				close(server_sock);
				return ;
			}
			setsockopt(server_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

			sockaddr_in server_addr;
			memset(&server_addr, 0, sizeof(server_addr));
			server_addr.sin_family = AF_INET;
			server_addr.sin_port = htons(ack_port_);
			server_addr.sin_addr.s_addr = INADDR_ANY;

			if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
				LOG(ERROR) << "Bind failed";
				close(server_sock);
				return ;
			}

			if (listen(server_sock, SOMAXCONN) < 0) {
				LOG(ERROR) << "Listen failed";
				close(server_sock);
				return ;
			}

			sockaddr_in client_addr;
			socklen_t client_addr_len = sizeof(client_addr);
			int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
			if (client_sock < 0) {
				LOG(ERROR) << "Accept failed";
				return ;
			}
			size_t read = 0;
			uint8_t* data = (uint8_t*)malloc(1024);
			while (!shutdown_ || read<client_order_){//shutdown_ is to ensure the client_order_ is fully updated
				size_t bytesReceived;
				if((bytesReceived = recv(client_sock, data, 1024, 0))){
					read += bytesReceived;
				}else{
					//LOG(ERROR) << "Read error:" << bytesReceived << " " << strerror(errno);
				}
			}
			LOG(INFO) << "Acked:" << read;
			return;
		}

		void PublishThread(int pubQuesIdx, int sock, int efd){
			// *********** Sending Messages ***********
			std::vector<double> send_times;
			std::vector<double> poll_times;
			thread_count_.fetch_add(1);
			while(!shutdown_){
				size_t len;
				void *msg = pubQue_.Read(pubQuesIdx, len);
				if(len == 0)
					break;
				Embarcadero::BatchHeader batch_header;
				batch_header.total_size = len;

				ssize_t bytesSent = send(sock, (uint8_t*)(&batch_header), sizeof(Embarcadero::BatchHeader), 0);
				if(bytesSent < (ssize_t)sizeof(Embarcadero::BatchHeader)){
					LOG(ERROR) << "Jae compelte this too when batch header is partitioned.";
					return;
				}

				size_t sent_bytes = 0;
				size_t zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
				while(sent_bytes < len){
					size_t remaining_bytes = len - sent_bytes;
					size_t to_send = std::min(remaining_bytes, zero_copy_send_limit);
					// First attempts to send messages for efficiency. 
					// epoll_wait at failure where there's not enough buffer space
					if(to_send < 1UL<<16)
						bytesSent = send(sock, (uint8_t*)msg + sent_bytes, to_send, 0);
					else
						bytesSent = send(sock, (uint8_t*)msg + sent_bytes, to_send, MSG_ZEROCOPY);
					if (bytesSent > 0) {
						sent_bytes += bytesSent;
						zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
					} else if (bytesSent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
						struct epoll_event events[10];
						int n = epoll_wait(efd, events, 10, -1);
						if (n == -1) {
							LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
							exit(1);
						}
						for (int i = 0; i < n; i++) {
							if (events[i].events & EPOLLOUT) {
								break;
							}
						}
						zero_copy_send_limit = std::max(zero_copy_send_limit / 2, 1UL<<16); // Cap backoff at 1000ms
					} else if (bytesSent < 0) {
						LOG(ERROR) << "send failed: " << strerror(errno);
						close(sock);
						close(efd);
						return;
					}

				}
			}
			close(sock);
			close(efd);
		}

		// Current implementation does not send messages to newly added nodes after init
		void ClusterProbeLoop(){
			heartbeat_system::ClientInfo client_info;
			heartbeat_system::ClusterStatus cluster_status;
			for (const auto &it: nodes_){
				client_info.add_nodes_info(it.first);
			}
			while(!shutdown_){
				grpc::ClientContext context;
				grpc::Status status = stub_->GetClusterStatus(&context, client_info, &cluster_status);
				if(status.ok()){
					connected_ = true;
					const auto& removed_nodes = cluster_status.removed_nodes();
					const auto& new_nodes = cluster_status.new_nodes();
					if(!removed_nodes.empty()){
						absl::MutexLock lock(&mutex_);
						//TODO(Jae) Handle broker failure
						for(const auto& id:removed_nodes){
							LOG(ERROR) << "Failed Node reported : " << id;
							nodes_.erase(id);
							RemoveNodeFromClientInfo(client_info, id);
						}
					}
					if(!new_nodes.empty()){
						absl::MutexLock lock(&mutex_);
						for(const auto& addr:new_nodes){
							int broker_id = GetBrokerId(addr);
							LOG(INFO) << "New Node reported:" << broker_id;
							nodes_[broker_id] = addr;
							brokers_.emplace_back(broker_id);
							client_info.add_nodes_info(broker_id);
						}
					}
				}else{
					LOG(ERROR) << "Head is dead, try reaching other brokers for a newly elected head";
				}
				std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL));
			}
		}

};

//TODO(Jae) Broker Failure is not handled, dynamic node addition not handled
class Subscriber{
	public:
		Subscriber(std::string head_addr, std::string port, char topic[TOPIC_NAME_SIZE], bool measure_latency=false):
			head_addr_(head_addr), port_(port), shutdown_(false), connected_(false), measure_latency_(measure_latency), buffer_size_((1UL<<33)), messages_idx_(0), client_id_(GenerateRandomNum()){
				messages_.resize(2);
				memcpy(topic_, topic, TOPIC_NAME_SIZE);
				std::string addr = head_addr+":"+port;
				stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
				nodes_[0] = head_addr+":"+std::to_string(PORT);
				cluster_probe_thread_ = std::thread([this](){
						this->ClusterProbeLoop();
						});
				while(!connected_){
					std::this_thread::yield();
				}
				LOG(INFO) << "Subscriber Constructed";
			}

		~Subscriber(){
			shutdown_ = true;
			cluster_probe_thread_.join();
			for( auto &t : subscribe_threads_){
				if(t.joinable()){
					t.join();
				}
			}
			for( auto &msg_pairs : messages_){
				for( auto &msg_pair : msg_pairs){
					free(msg_pair.first);
					free(msg_pair.second);
				}
			}
			LOG(INFO) << "Subscriber Destructed";
		};

		void* Consume(){
			//int i = messages_idx_.fetch_xor(1);
			return nullptr;
		}

		void* ConsumeBatch(){
			//int i = messages_idx_.fetch_xor(1);
			return nullptr;
		}

		bool DEBUG_check_order(int order){
			int idx = 0;
			for(auto &msg_pair : messages_[idx]){
				//for(auto &msg_pairs : messages_){
				//for( auto &msg_pair : msg_pairs){
				void* buf = msg_pair.first;
				// Check if messages are given logical offsets
				Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)buf;
				while(header->paddedSize != 0){
					if(header->logical_offset == (size_t)-1){
						LOG(ERROR) << "msg:" << header->client_order << " is not given logical offset";
						return false;
					}
					header = (Embarcadero::MessageHeader*)((uint8_t*)header + header->paddedSize);
				}
				if(order == 0){
					continue;
				}
				// Check if messages are given total order
				header = (Embarcadero::MessageHeader*)buf;
				absl::flat_hash_set<int> DEBUG_duplicate;
				while(header->paddedSize != 0){
					if(header->total_order == 0 && header->logical_offset != 0){
						LOG(ERROR) << "msg:" << header->client_order << " logical off:" << header->logical_offset << " is not given total order";
						return false;
					}
					if(DEBUG_duplicate.contains(header->total_order)){
						LOG(ERROR) << "!!! msg:" << header->client_order << " total order is duplicate:" << header->total_order;
					}else{
						DEBUG_duplicate.insert(header->total_order);
					}
					header = (Embarcadero::MessageHeader*)((uint8_t*)header + header->paddedSize);
				}
				if(order == 1){
					continue;
				}
				// Check if messages are given total order the same as client_order
				header = (Embarcadero::MessageHeader*)buf;
				while(header->paddedSize != 0){
					if(header->total_order != header->client_order){
						LOG(ERROR) << "msg:" << header->client_order << " logical off:" << header->logical_offset << " was given a wrong total order" << header->total_order;
						return false;
					}
					header = (Embarcadero::MessageHeader*)((uint8_t*)header + header->paddedSize);
				}
			}
			//}
			return true;
			}

			void StoreLatency(){
				std::vector<long long> latencies;
				int idx = 0;
				for(auto &pair:messages_[idx]){
					struct msgIdx *m = pair.second;
					void* buf = pair.first;
					size_t off = 0;
					int recv_latency_idx = 0;
					while(off < m->offset){
						Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)((uint8_t*)buf + off);
						while(off > m->timestamps[recv_latency_idx].first){
							recv_latency_idx++;
						}
						long long send_nanoseconds_since_epoch;
						memcpy(&send_nanoseconds_since_epoch, (void*)((uint8_t*)header + sizeof(Embarcadero::MessageHeader)), sizeof(long long));
						auto received_time_point = std::chrono::time_point<std::chrono::steady_clock>(
								std::chrono::nanoseconds(send_nanoseconds_since_epoch));

						auto latency = m->timestamps[recv_latency_idx].second - received_time_point;;
						latencies.emplace_back(std::chrono::duration_cast<std::chrono::microseconds>(latency).count());
						off += header->paddedSize;
					}
				}
				std::ofstream latencyFile("latencies.csv");
				if (!latencyFile.is_open()) {
					LOG(ERROR) << "Failed to open file for writing";
					return ;
				}
				latencyFile << "Latency\n";
				for (const auto& latency : latencies) {
					latencyFile << latency << "\n";
				}

				latencyFile.close();
			}

			void DEBUG_wait(size_t total_msg_size, size_t msg_size){
				size_t num_msg = total_msg_size/msg_size;
				auto start = std::chrono::steady_clock::now();
				size_t total_data_size = num_msg * sizeof(Embarcadero::MessageHeader) + total_msg_size;
				while(DEBUG_count_ < total_data_size){
					std::this_thread::yield();
					if(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-start).count() >=3){
						start = std::chrono::steady_clock::now();
						VLOG(3) << "Received:" << DEBUG_count_ << "/" << total_data_size;
					}
				}
				return;
			}

			private:
			std::string head_addr_;
			std::string port_;
			bool shutdown_;
			bool connected_;
			std::unique_ptr<HeartBeat::Stub> stub_;
			std::thread cluster_probe_thread_;
			std::vector<std::thread> subscribe_threads_;
			// <broker_id, address::port of network_mgr>
			absl::flat_hash_map<int, std::string> nodes_;
			absl::Mutex mutex_;
			char topic_[TOPIC_NAME_SIZE];
			bool measure_latency_;
			size_t buffer_size_;
			std::atomic<size_t> DEBUG_count_ = 0;
			void* last_fetched_addr_;
			int last_fetched_offset_;
			std::vector<std::vector<std::pair<void*, msgIdx*>>> messages_;
			std::atomic<int> messages_idx_;
			uint16_t client_id_;

			void SubscribeThread(int epoll_fd, absl::flat_hash_map<int, std::pair<void*, msgIdx*>> fd_to_msg){
				epoll_event events[NUM_SUB_CONNECTIONS];
				while(!shutdown_){
					int nfds = epoll_wait(epoll_fd, events, NUM_SUB_CONNECTIONS, 100); // 0.1 second timeout
					if (nfds == -1) {
						if (errno == EINTR) continue;  // Interrupted system call, just continue
						LOG(ERROR) << "epoll_wait error" << std::endl;
						break;
					}
					for (int n = 0; n < nfds; ++n) {
						if (events[n].events & EPOLLIN) {
							int fd = events[n].data.fd;
							//int idx = messages_idx_.load();
							int idx = 0;
							struct msgIdx *m = fd_to_msg[fd].second; // &messages_[idx][fd_to_msg_idx[fd]].second;
							void* buf = fd_to_msg[fd].first;// messages_[idx][fd_to_msg_idx[fd]].first;
																							// This ensures the receive never goes out of the boundary
																							// bit it may cause the incomplete recv
							size_t to_read = buffer_size_ - m->offset;
							int bytes_received = recv(fd, (uint8_t*)buf + m->offset, to_read, 0);
							if (bytes_received > 0) {
								DEBUG_count_.fetch_add(bytes_received);
								m->offset += bytes_received;
								if(measure_latency_){
									m->timestamps.emplace_back(m->offset, std::chrono::steady_clock::now());
								}
							} else if (bytes_received == 0) {
								LOG(ERROR) << "Server " << fd << " disconnected";
								epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
								close(fd);
								//TODO(Jae) remove from other data structures
								break;
							} else {
								if (errno != EWOULDBLOCK && errno != EAGAIN) {
									LOG(ERROR) << "Recv failed EWOULDBLOCK or EAGAIN for server " << fd << " " << strerror(errno);
								}
								break;
							}
						}
					} // end epoll cycle
				} // end while(shutdown_)

				close(epoll_fd);
			}

			void CreateAConnection(int broker_id, std::string address){
				absl::flat_hash_map<int, std::pair<void*, msgIdx*>> fd_to_msg;
				auto [addr, addressPort] = ParseAddressPort(address);
				for (int i=0; i < NUM_SUB_CONNECTIONS; i++){
					int epoll_fd = epoll_create1(0);
					if (epoll_fd < 0) {
						LOG(ERROR) << "Failed to create epoll instance";
						return ;
					}
					int sock = GetNonblockingSock(addr.data(), PORT + broker_id, false);

					std::pair<void*, msgIdx*> msg(static_cast<void*>(calloc(buffer_size_, sizeof(char))), (msgIdx*)malloc(sizeof(msgIdx)));
					// This is for client retrieval, double buffer
					//std::pair<void*, msgIdx> msg1(static_cast<void*>(malloc(buffer_size_)), msgIdx(broker_id));
					int idx = messages_[0].size();
					messages_[0].push_back(msg);
					//messages_[1].push_back(msg1);
					fd_to_msg.insert({sock, msg});;

					//Create a connection by Sending a Sub request
					Embarcadero::EmbarcaderoReq shake;
					shake.client_order = 0;
					shake.client_id = client_id_;
					shake.last_addr = 0;
					shake.client_req = Embarcadero::Subscribe;
					memcpy(shake.topic, topic_, TOPIC_NAME_SIZE);
					int ret = send(sock, &shake, sizeof(shake), 0);
					if(ret < sizeof(shake)){
						LOG(ERROR) << "fd:" << sock << " addr:" << addr << " id:" << broker_id  << 
							"sent:" << ret<< "/" <<sizeof(shake) 	<< " failed:" << strerror(errno);
						close(sock);
						continue;
					}

					epoll_event ev;
					ev.events = EPOLLIN;
					ev.data.fd = sock;
					if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) == -1) {
						LOG(ERROR) << "Failed to add new server to epoll";
						close(sock);
					}
					subscribe_threads_.emplace_back(&Subscriber::SubscribeThread, this, epoll_fd, fd_to_msg);
				}
			}

			void ClusterProbeLoop(){
				heartbeat_system::ClientInfo client_info;
				heartbeat_system::ClusterStatus cluster_status;
				for (const auto &it: nodes_){
					client_info.add_nodes_info(it.first);
				}
				while(!shutdown_){
					grpc::ClientContext context;
					grpc::Status status = stub_->GetClusterStatus(&context, client_info, &cluster_status);
					if(status.ok()){
						// Head is alive (broker 0), add a connection
						if(!connected_){
							CreateAConnection(0, nodes_[0]);
						}
						connected_ = true;
						const auto& removed_nodes = cluster_status.removed_nodes();
						const auto& new_nodes = cluster_status.new_nodes();
						if(!removed_nodes.empty()){
							absl::MutexLock lock(&mutex_);
							//TODO(Jae) Handle broker failure
							for(const auto& id:removed_nodes){
								LOG(ERROR) << "Failed Node reported : " << id;
								nodes_.erase(id);
								RemoveNodeFromClientInfo(client_info, id);
							}
						}
						if(!new_nodes.empty()){
							absl::MutexLock lock(&mutex_);
							for(const auto& addr:new_nodes){
								int broker_id = GetBrokerId(addr);
								LOG(INFO) << "New Node reported:" << broker_id;
								nodes_[broker_id] = addr;
								client_info.add_nodes_info(broker_id);
								CreateAConnection(broker_id, addr);
							}
						}
					}else{
						LOG(ERROR) << "Head is dead, try reaching other brokers for a newly elected head";
					}
					std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL));
				}
			}

		};

		// Checks if Cgroup is successful. Wait for 1 second to allow the process to be attached to the cgroup
		bool CheckAvailableCores(){
			sleep(1);
			size_t num_cores = 0;
			cpu_set_t mask;
			CPU_ZERO(&mask);

			if (sched_getaffinity(0, sizeof(mask), &mask) == -1) {
				perror("sched_getaffinity");
				exit(EXIT_FAILURE);
			}

			printf("This process can run on CPUs: ");
			for (int i = 0; i < CPU_SETSIZE; i++) {
				if (CPU_ISSET(i, &mask)) {
					printf("%d ", i);
					num_cores++;
				}
			}
			return num_cores == CGROUP_CORE;
		}

		void PublishThroughputTest(size_t total_message_size, size_t message_size, int num_threads, int ack_level, int order, SequencerType seq_type, bool fixed_batch_size){
			size_t n = total_message_size/message_size;
			LOG(INFO) << "[Throuput Test] total_message:" << total_message_size << " message_size:" << message_size << " n:" << n << " num_threads:" << num_threads;
			char* message = (char*)malloc(sizeof(char)*message_size);
			char topic[TOPIC_NAME_SIZE];
			memset(topic, 0, TOPIC_NAME_SIZE);
			memcpy(topic, "TestTopic", 9);

			size_t q_size = total_message_size + (total_message_size/message_size)*64;
			if(q_size < 1024){
				q_size = 1024;
			}
			Client c("127.0.0.1", std::to_string(BROKER_PORT), num_threads, message_size, q_size, fixed_batch_size);
			LOG(INFO) << "Client Created" ;
			c.CreateNewTopic(topic, order, seq_type);
			c.Init(topic, ack_level, order);
			auto start = std::chrono::high_resolution_clock::now();

			for(size_t i=0; i<n; i++){
				c.Publish(message, message_size);
			}

			auto produce_end = std::chrono::high_resolution_clock::now();
			c.DEBUG_check_send_finish();
			auto send_end = std::chrono::high_resolution_clock::now();
			c.Poll(n);

			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> elapsed = end - start;
			double seconds = elapsed.count();
			double bandwidthMbps = ((message_size*n) / seconds) / (1024 * 1024);  // Convert to Megabytes per second
			std::cout << "Produce time: " << ((std::chrono::duration<double>)(produce_end-start)).count() << std::endl;
			std::cout << "Send time: " << ((std::chrono::duration<double>)(send_end-start)).count() << std::endl;
			std::cout << "Total time: " << seconds << std::endl;
			std::cout << "Bandwidth: " << bandwidthMbps << " MBps" << std::endl;
			free(message);
		}

		void SubscribeThroughputTest(size_t total_msg_size, size_t msg_size, int order){
			LOG(INFO) << "[Subscribe Throuput Test] ";
			char topic[TOPIC_NAME_SIZE];
			memset(topic, 0, TOPIC_NAME_SIZE);
			memcpy(topic, "TestTopic", 9);
			auto start = std::chrono::high_resolution_clock::now();
			Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic);
			s.DEBUG_wait(total_msg_size, msg_size);
			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> elapsed = end - start;
			double seconds = elapsed.count();
			LOG(INFO) << (total_msg_size/(1024*1024))/seconds << "MB/s";
			s.DEBUG_check_order(order);
		}

		void E2EThroughputTest(size_t total_message_size, size_t message_size, int num_threads, int ack_level, int order, SequencerType seq_type, bool fixed_batch_size){
			size_t n = total_message_size/message_size;
			LOG(INFO) << "[E2E Throuput Test] total_message:" << total_message_size << " message_size:" << message_size << " n:" << n << " num_threads:" << num_threads;
			std::string message(message_size, 0);
			char topic[TOPIC_NAME_SIZE];
			memset(topic, 0, TOPIC_NAME_SIZE);
			memcpy(topic, "TestTopic", 9);

			size_t q_size = total_message_size + (total_message_size/message_size)*64;
			if(q_size < 1024){
				q_size = 1024;
			}
			Client c("127.0.0.1", std::to_string(BROKER_PORT), num_threads, message_size, q_size, fixed_batch_size);
			c.CreateNewTopic(topic, order, seq_type);
			Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic);
			c.Init(topic, ack_level, order);

			auto start = std::chrono::high_resolution_clock::now();
			for(size_t i=0; i<n; i++){
				c.Publish(message.data(), message_size);
			}

			c.Poll(n);
			auto pub_end = std::chrono::high_resolution_clock::now();
			s.DEBUG_wait(total_message_size, message_size);
			auto end = std::chrono::high_resolution_clock::now();
			auto pub_duration = std::chrono::duration_cast<std::chrono::seconds>(pub_end - start);
			LOG(INFO) << "Pub Bandwidth: " << (total_message_size/(1024*1024))/pub_duration.count() << " MB/s";
			auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
			LOG(INFO) << "E2E Bandwidth: " << (total_message_size/(1024*1024))/duration.count() << " MB/s";
			s.DEBUG_check_order(order);
		}

		void LatencyTest(size_t total_message_size, size_t message_size, int num_threads, int ack_level, int order, SequencerType seq_type, bool fixed_batch_size){
			size_t n = total_message_size/message_size;
			LOG(INFO) << "[Latency Test] total_message:" << total_message_size << " message_size:" << message_size << " n:" << n << " num_threads:" << num_threads;
			char message[message_size];
			char topic[TOPIC_NAME_SIZE];
			memset(topic, 0, TOPIC_NAME_SIZE);
			memcpy(topic, "TestTopic", 9);

			size_t q_size = total_message_size + (total_message_size/message_size)*64;
			if(q_size < 1024){
				q_size = 1024;
			}
			Client c("127.0.0.1", std::to_string(BROKER_PORT), num_threads, message_size, q_size, fixed_batch_size);
			c.CreateNewTopic(topic, order, seq_type);
			Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, true);
			c.Init(topic, ack_level, order);

			auto start = std::chrono::high_resolution_clock::now();
			for(size_t i=0; i<n; i++){
				auto timestamp = std::chrono::steady_clock::now();
				long long nanoseconds_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
						timestamp.time_since_epoch()).count();
				memcpy(message, &nanoseconds_since_epoch, sizeof(long long));
				c.Publish(message, message_size);
			}
			c.Poll(n);
			auto pub_end = std::chrono::high_resolution_clock::now();
			s.DEBUG_wait(total_message_size, message_size);
			auto end = std::chrono::high_resolution_clock::now();

			auto pub_duration = std::chrono::duration_cast<std::chrono::seconds>(pub_end - start);
			auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
			LOG(INFO) << "Pub Bandwidth: " << (total_message_size/(1024*1024))/pub_duration.count() << " MB/s";
			LOG(INFO) << "E2E Bandwidth: " << (total_message_size/(1024*1024))/duration.count() << " MB/s";
			s.DEBUG_check_order(order);
			s.StoreLatency();
		}

		heartbeat_system::SequencerType parseSequencerType(const std::string& value) {
			if (value == "EMBARCADERO") return heartbeat_system::SequencerType::EMBARCADERO;
			if (value == "KAFKA") return heartbeat_system::SequencerType::KAFKA;
			if (value == "SCALOG") return heartbeat_system::SequencerType::SCALOG;
			if (value == "CORFU") return heartbeat_system::SequencerType::CORFU;
			throw std::runtime_error("Invalid SequencerType: " + value);
		}

		int main(int argc, char* argv[]) {
			google::InitGoogleLogging(argv[0]);
			google::InstallFailureSignalHandler();
			FLAGS_logtostderr = 1; // log only to console, no files.
			cxxopts::Options options("embarcadero-throughputTest", "Embarcadero Throughput Test");

			options.add_options()
				("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
				("a,ack_level", "Acknowledgement level", cxxopts::value<int>()->default_value("1"))
				("o,order_level", "Order Level", cxxopts::value<int>()->default_value("0"))
				("sequencer", "Sequencer Type: Embarcadero(0), Kafka(1), Scalog(2), Corfu(3)", cxxopts::value<std::string>()->default_value("EMBARCADERO"))
				("s,total_message_size", "Total size of messages to publish", cxxopts::value<size_t>()->default_value("10737418240"))
				("m,size", "Size of a message", cxxopts::value<size_t>()->default_value("1024"))
				("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
				("f,fixed_batch_size", "Use a fixed batch size for publishing", cxxopts::value<bool>()->default_value("false"))
				("t,num_threads", "Number of request threads", cxxopts::value<size_t>()->default_value("16"));

			auto result = options.parse(argc, argv);
			size_t message_size = result["size"].as<size_t>();
			size_t total_message_size = result["total_message_size"].as<size_t>();
			size_t num_threads = result["num_threads"].as<size_t>();
			int ack_level = result["ack_level"].as<int>();
			int order = result["order_level"].as<int>();
			bool fixed_batch_size = result["fixed_batch_size"].as<bool>();
			SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());
			FLAGS_v = result["log_level"].as<int>();

			if(result["run_cgroup"].as<int>() > 0 && !CheckAvailableCores()){
				LOG(ERROR) << "CGroup core throttle is wrong";
				return -1;
			}

			//PublishThroughputTest(total_message_size, message_size, num_threads, ack_level, order, seq_type, fixed_batch_size);
			//SubscribeThroughputTest(total_message_size, message_size, order, fixed_batch_size);
			//E2EThroughputTest(total_message_size, message_size, num_threads, ack_level, order, seq_type, fixed_batch_size);
			LatencyTest(total_message_size, message_size, num_threads, ack_level, order, seq_type, fixed_batch_size);

			return 0;
		}
