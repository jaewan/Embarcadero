#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
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
#include "folly/MPMCQueue.h"

#include <heartbeat.grpc.pb.h>
#include "common/config.h"
#include "../cxl_manager/cxl_manager.h"

using heartbeat_system::HeartBeat;

class Client{
	public:
		Client(std::string head_addr, std::string port, size_t queueSize):
			head_addr_(head_addr), port_(port), shutdown_(false), connected_(false), total_queue_size_(queueSize), client_order_(0){
				std::string addr = head_addr+":"+port;
				stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
				client_id_ = GenerateRandomNum();
				nodes_[0] = head_addr+":"+std::to_string(PORT);
				cluster_probe_thread_ = std::thread([this](){
						this->ClusterProbeLoop();
						});
				while(!connected_){
					std::this_thread::yield();
				}
				{
					absl::MutexLock lock(&mutex_);
					//Add headnode as headnode is not reported from ClusterProbeLoop
					size_t queueSize = total_queue_size_ / (nodes_.size() + 1);
					pubQues_.emplace_back(queueSize);
					broker_id_to_queue_idx_[0] = pubQues_.size() - 1;
				}
			}

		~Client(){
			LOG(INFO) << "Destructing Client";
			shutdown_ = true;
			cluster_probe_thread_.join();
			std::optional<char*> sentinel = std::nullopt;
			int len = pubQues_.size();
			for(auto& q:pubQues_){
				for(int i =0; i<num_threads_; i++) // This is not correct for sanity overwrite sentinel
					q.blockingWrite(sentinel);
			}
			for(auto &t : threads_)
				t.join();
		};

		void Init(int num_threads, int msg_copy, char topic[TOPIC_NAME_SIZE], int ack_level, int order, size_t message_size){
			num_threads_ = num_threads;
			msg_copy_ = msg_copy;
			memcpy(topic_, topic, TOPIC_NAME_SIZE);
			ack_level_ = ack_level;
			order_ = order;
			ack_port_ = GenerateRandomNum();
			message_size_ = message_size;
			ack_thread_ = std::thread([this](){
					this->EpollAckThread();
					});
			int num_nodes = pubQues_.size();
			for (int i=0; i < num_threads; i++){
				threads_.emplace_back(&Client::PublishThread, this, i%num_nodes);
			}
			return;
		}

		void Publish(std::string &message){
			static int i = 0;
			pubQues_[i].blockingWrite(message.data());
			i = (i+1)%pubQues_.size();
		}

		void CorfuPublish(std::string &message){
			static int i = 0;
			// TODO(Erika) RPC global sequencer and get the messages ordered
			// Best way to do this is to create a CorfuClient
			// Make batch argument
			// stub_->GetMessageOrder(num_batch)
			pubQues_[i].blockingWrite(message.data());
			i = (i+1)%pubQues_.size();
		}

		void Poll(int n){
			while(client_order_ != n){
				std::this_thread::yield();
			}
			shutdown_ = true;
			ack_thread_.join();
			return;
		}

		bool CreateNewTopic(char topic[TOPIC_NAME_SIZE], int order){
			grpc::ClientContext context;
			heartbeat_system::CreateTopicRequest create_topic_req;;
			heartbeat_system::CreateTopicResponse create_topic_reply;;
			create_topic_req.set_topic(topic);
			create_topic_req.set_order(order);
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
		int client_id_;
		size_t total_queue_size_;
		std::unique_ptr<HeartBeat::Stub> stub_;
		std::thread cluster_probe_thread_;
		// <broker_id, address::port of network_mgr>
		absl::flat_hash_map<int, std::string> nodes_;
		absl::Mutex mutex_;
		std::vector<folly::MPMCQueue<std::optional<char*>>> pubQues_;
		absl::flat_hash_map<int, int> broker_id_to_queue_idx_;
		std::atomic<size_t> client_order_;
		int num_threads_;
		int msg_copy_;
		char topic_[TOPIC_NAME_SIZE];
		int ack_level_;
		int order_;
		int ack_port_;
		size_t message_size_;
		std::vector<std::thread> threads_;
		std::thread ack_thread_;

		void EpollAckThread(){
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
			char buffer[1024];
			size_t total_received = 0;
			int EPOLL_TIMEOUT = 1; // 1 millisecond timeout

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
							close(client_sock);
						}
					} else {
						// Data from existing client
						int client_sock = events[i].data.fd;
						ssize_t bytes_received;

						while (total_received < client_order_ && (bytes_received = recv(client_sock, buffer, 1024, 0)) > 0) {
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
						} else if (bytes_received == -1) {
							if (errno != EAGAIN && errno != EWOULDBLOCK) {
								// Error occurred
								std::cerr << "recv error: " << strerror(errno) << std::endl;
								epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
								close(client_sock);
							}
						}
					}
				}
			}

			// Cleanup
			close(server_sock);
			close(epoll_fd);
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
					VLOG(3) << "Ack Received:" << read;
				}else{
					//LOG(ERROR) << "Read error:" << bytesReceived << " " << strerror(errno);
				}
			}
			LOG(INFO) << "Acked:" << read;
			return;
		}

		void PublishThread(int pubQuesIdx){
			int broker_id = 0;
			for(auto& it:broker_id_to_queue_idx_){
				if(it.second == pubQuesIdx){
					broker_id = it.first;
					break;
				}
			}
			auto [addr, addressPort] = ParseAddressPort(nodes_[broker_id]);
			//int sock = GetNonblockingSock("127.0.0.1", PORT);
			int sock = GetNonblockingSock(addr.data(), PORT + broker_id);
			// *********** Initiate Shake ***********
			int efd = epoll_create1(0);
			struct epoll_event event;
			event.data.fd = sock;
			event.events = EPOLLOUT;
			epoll_ctl(efd, EPOLL_CTL_ADD, sock, &event);

			Embarcadero::EmbarcaderoReq shake;
			shake.client_id = client_id_;
			memcpy(shake.topic, topic_, TOPIC_NAME_SIZE);
			shake.ack = ack_level_;
			shake.client_order = order_;
			shake.port = ack_port_;
			shake.size = message_size_ + sizeof(Embarcadero::MessageHeader);
			int n, i;
			struct epoll_event events[10]; // Adjust size as needed
			bool running = true;
			size_t sent_bytes = 0;
			while (running) {
				n = epoll_wait(efd, events, 10, -1);
				for (i = 0; i < n; i++) {
					if (events[i].events & EPOLLOUT) {
						ssize_t bytesSent = send(sock, (int8_t*)(&shake) + sent_bytes, sizeof(shake) - sent_bytes, 0);
						if (bytesSent < 0) {
							if (errno != EAGAIN) {
								perror("send failed");
								running = false;
								break;
							}
						}
						sent_bytes += bytesSent;
						if(sent_bytes == sizeof(shake)){
							running = false;
							if(i == n-1){
								i = 0;
								n = epoll_wait(efd, events, 10, -1);
							}
							break;
						}
					}
				}
			}

			// *********** Sending Messages ***********
			Embarcadero::MessageHeader header;
			header.client_id = client_id_;
			header.size = message_size_;
			header.total_order = 0;
			int padding = message_size_ % 64;
			if(padding){
				padding = 64 - padding;
			}
			header.paddedSize = message_size_ + padding + sizeof(Embarcadero::MessageHeader);
			header.segment_header = nullptr;
			header.logical_offset = (size_t)-1; // Sentinel value
			header.next_message = nullptr;
			std::optional<char*> optReq;

			while(!shutdown_){
				pubQues_[pubQuesIdx].blockingRead(optReq);
				if(!optReq.has_value()){
					return;
				}
				char* message = optReq.value();
				header.client_order = client_order_.fetch_add(1);

				bool send_msg = true;
				sent_bytes = 0;
				while (send_msg) {
					for (; i < n; i++) {
						if (events[i].events & EPOLLOUT) {
							ssize_t bytesSent;
							if(sent_bytes < sizeof(header)){
								bytesSent = send(sock, (uint8_t*)&header + sent_bytes, sizeof(header) - sent_bytes, 0);
							}else{
								bytesSent = send(sock, (uint8_t*)message + sent_bytes - 64, shake.size - sent_bytes, 0);
							}
							if (bytesSent < 0) {
								if (errno != EAGAIN) {
									LOG(ERROR) << "send failed";
									send_msg = false;
									break;
								}
							}
							sent_bytes += bytesSent;
							if(sent_bytes == shake.size){
								send_msg = false;
								break;
							}
						}
					}
					n = epoll_wait(efd, events, 10, -1);
					i = 0;
				}
			}
			close(sock);
			close(efd);
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

							/*
								 pubQues_.erase(pubQues_.begin() + broker_id_to_queue_idx_[id]);
								 broker_id_to_queue_idx_.erase(id);
								 */
						}
					}
					if(!new_nodes.empty()){
						absl::MutexLock lock(&mutex_);
						size_t queueSize = total_queue_size_ / (nodes_.size() + new_nodes.size());
						for(const auto& addr:new_nodes){
							int broker_id = GetBrokerId(addr);
							LOG(INFO) << "New Node reported:" << broker_id;
							nodes_[GetBrokerId(addr)] = addr;
							client_info.add_nodes_info(broker_id);

							//pubQues.emplace(broker_id, folly::MPMCQueue<std::optional<char*>>(queueSize));
							pubQues_.emplace_back(queueSize);
							broker_id_to_queue_idx_[broker_id] = pubQues_.size() - 1;
						}
					}
				}else{
					LOG(ERROR) << "Head is dead, try reaching other brokers for a newly elected head";
				}
				std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL));
			}
		}

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

		int GenerateRandomNum(){
			// Generate a random number
			std::random_device rd;
			std::mt19937 gen(rd());
			std::uniform_int_distribution<> dis(NUM_MAX_BROKERS, 999999);
			return  dis(gen);
		}

		int GetNonblockingSock(char *broker_address, int port){
			int sock = socket(AF_INET, SOCK_STREAM, 0);
			if (sock < 0) {
				LOG(ERROR) << "Socket creation failed";
				return -1;
			}
			int flags = fcntl(sock, F_GETFL, 0);
			if (flags == -1) {
				perror("fcntl F_GETFL");
				return -1;
			}

			flags |= O_NONBLOCK;
			if (fcntl(sock, F_SETFL, flags) == -1) {
				perror("fcntl F_SETFL");
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

			sockaddr_in server_addr;
			memset(&server_addr, 0, sizeof(server_addr));
			server_addr.sin_family = AF_INET;
			server_addr.sin_port = htons(port);
			server_addr.sin_addr.s_addr = inet_addr(broker_address);

			if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
				if (errno != EINPROGRESS) {
					LOG(ERROR) << "Connect failed";
					close(sock);
					return -1;
				}
			}

			return sock;
		}
};

#define ACK_SIZE 1024
#define SERVER_ADDR "127.0.0.1"

std::atomic<size_t> totalBytesRead_(0);
std::atomic<size_t> client_order_(0);
int ack_port_;;

// This is to avoid contention if brokers and clients run on the same node
int GenerateRandomPORT(){
	// Generate a random number
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(NUM_MAX_BROKERS, 999999);
	return  dis(gen);
}

int make_socket_non_blocking(int sfd) {
	int flags = fcntl(sfd, F_GETFL, 0);
	if (flags == -1) {
		perror("fcntl F_GETFL");
		return -1;
	}

	flags |= O_NONBLOCK;
	if (fcntl(sfd, F_SETFL, flags) == -1) {
		perror("fcntl F_SETFL");
		return -1;
	}
	return 0;
}

std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> send_data(size_t message_size,
		size_t total_message_size, int ack_level, size_t CLIENT_ID, bool record_latency) {
	std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> times;
	times.reserve(1<<15);
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("Socket creation failed");
		return times;
	}

	make_socket_non_blocking(sock);

	int flag = 1; // Enable the option
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
		perror("setsockopt(SO_REUSEADDR) failed");
		close(sock);
		return times;
	}

	if(setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)) != 0){
		perror("setsockopt error");
		close(sock);
		return times;
	}

	sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

	if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
		if (errno != EINPROGRESS) {
			perror("Connect failed");
			close(sock);
			return times;
		}
	}

	int efd = epoll_create1(0);
	struct epoll_event event;
	event.data.fd = sock;
	event.events = EPOLLOUT;
	epoll_ctl(efd, EPOLL_CTL_ADD, sock, &event);

	// =============== Sending Shake ===============
	Embarcadero::EmbarcaderoReq shake;
	shake.client_id = CLIENT_ID;
	shake.client_order = 0;
	memset(shake.topic, 0, TOPIC_NAME_SIZE);
	memcpy(shake.topic, "TestTopic", 9);
	shake.ack = ack_level;
	shake.port = ack_port_;
	shake.size = message_size + sizeof(Embarcadero::MessageHeader);
	int n, i;
	struct epoll_event events[10]; // Adjust size as needed
	bool running = true;
	size_t sent_bytes = 0;
	//This is to measure throughput more precisely
	if(!record_latency){
		times.emplace_back(std::chrono::high_resolution_clock::now());
	}
	while (running) {
		n = epoll_wait(efd, events, 10, -1);
		for (i = 0; i < n; i++) {
			if (events[i].events & EPOLLOUT) {
				ssize_t bytesSent = send(sock, (int8_t*)(&shake) + sent_bytes, sizeof(shake) - sent_bytes, 0);
				if (bytesSent < 0) {
					if (errno != EAGAIN) {
						perror("send failed");
						running = false;
						break;
					}
				}
				sent_bytes += bytesSent;
				if(sent_bytes == sizeof(shake)){
					running = false;
					if(i == n-1){
						i = 0;
						n = epoll_wait(efd, events, 10, -1);
					}
					break;
				}
			}
		}
	}

	char *data = (char*)calloc(message_size+64, sizeof(char));

	Embarcadero::MessageHeader header;
	header.client_id = CLIENT_ID;
	header.size = message_size;
	header.total_order = 0;
	header.client_order = client_order_.fetch_add(1);
	int padding = message_size % 64;
	if(padding){
		padding = 64 - padding;
	}
	header.paddedSize = message_size + padding + sizeof(Embarcadero::MessageHeader);
	header.segment_header = nullptr;
	header.logical_offset = (size_t)-1; // Sentinel value
	header.next_message = nullptr;

	size_t run_count = total_message_size/message_size;

	sent_bytes = 0;
	running = true;
	bool stop_sending = false;
	int num_send_called_this_msg = 0;
	while (running) {
		for (; i < n; i++) {
			if (events[i].events & EPOLLOUT && (!stop_sending || header.client_order < run_count)) {
				if(!stop_sending && header.client_order >= run_count){
					stop_sending = true;
					header.client_id = -1;
					std::cout << "Closing the connectiong" << std::endl;
				}
				if(record_latency)
					times.emplace_back(std::chrono::high_resolution_clock::now());
				ssize_t bytesSent;
				if(sent_bytes < sizeof(header)){
					bytesSent = send(sock, (uint8_t*)&header + sent_bytes, sizeof(header) - sent_bytes, 0);
				}else{
					bytesSent = send(sock, (uint8_t*)data + sent_bytes, shake.size - sent_bytes, 0);
				}
				num_send_called_this_msg++;
				if (bytesSent < 0) {
					if(record_latency)
						times.pop_back();
					if (errno != EAGAIN) {
						perror("send failed");
						running = false;
						break;
					}
				}
				sent_bytes += bytesSent;
				if(sent_bytes == shake.size){
					sent_bytes = 0;
					num_send_called_this_msg = 0;
					header.client_order = client_order_.fetch_add(1);
				}else{
					if(record_latency && num_send_called_this_msg>1){
						times.pop_back();
					}
				}
			}
		}
		if (header.client_order >= run_count && stop_sending) { // Example break condition
			break;
			//running = false;
		}
		n = epoll_wait(efd, events, 10, -1);
		i = 0;
	}
	if(!record_latency){
		times.emplace_back(std::chrono::high_resolution_clock::now());
	}
	close(sock);
	close(efd);
	free(data);

	return times;
}

std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> read_ack(size_t TOTAL_DATA_SIZE,
		size_t message_size, size_t CLIENT_ID, bool record_latency){
	int server_sock = socket(AF_INET, SOCK_STREAM, 0);
	std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> times;
	times.reserve(1<<15);
	std::chrono::time_point<std::chrono::high_resolution_clock> DEBUG_end_time;
	if (server_sock < 0) {
		perror("Socket creation failed");
		return times;
	}

	int flag = 1;
	if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
		perror("setsockopt(SO_REUSEADDR) failed");
		close(server_sock);
		return times;
	}
	setsockopt(server_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

	sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(ack_port_);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
		perror("Bind failed");
		close(server_sock);
		return times;
	}

	if (listen(server_sock, SOMAXCONN) < 0) {
		perror("Listen failed");
		close(server_sock);
		return times;
	}

	sockaddr_in client_addr;
	socklen_t client_addr_len = sizeof(client_addr);
	int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
	if (client_sock < 0) {
		perror("Accept failed");
		return times;
	}

	char *data = (char*)calloc(TOTAL_DATA_SIZE/message_size, sizeof(char));
	ssize_t bytesReceived;
	int to_read = TOTAL_DATA_SIZE/message_size;//sizeof(std::chrono::time_point<std::chrono::high_resolution_clock>);
	while (to_read > 0){
		if((bytesReceived = recv(client_sock, (uint8_t*)data + ((TOTAL_DATA_SIZE/message_size) - to_read) , 1024, 0))){
			if(record_latency){
				auto t = std::chrono::high_resolution_clock::now();
				for(int i =0; i < bytesReceived; i++){
					times.emplace_back(t);
				}
			}
			to_read -= bytesReceived;
		}else{
			perror("Read error");
		}
	}
	free(data);
	close(client_sock);
	return times;
}

void SingleClientMultipleThreads(size_t num_threads, size_t total_message_size, size_t message_size, int ack_level, bool record_latency){
	LOG(INFO) << "Starting SingleClientMultipleThreads Throughput Test with " << num_threads << " threads, total message size:" << total_message_size;
	size_t client_id = 1;
	ack_port_ = GenerateRandomPORT();
	std::vector<std::future<std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>>>> pub_futures;

	std::future<std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>>> ack_future;
	if(ack_level > 0){
		ack_future = std::async(read_ack, total_message_size, message_size, client_id, record_latency);
	}


	// Spawning threads to publish
	for (size_t i = 0; i < num_threads; ++i) {
		pub_futures.emplace_back(std::async(std::launch::async, send_data, message_size, total_message_size, ack_level, client_id, record_latency));
	}
	std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> pub_times;
	std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> ack_times;
	for(auto& future: pub_futures){
		auto vec = future.get();
		pub_times.insert(pub_times.end(), vec.begin(), vec.end());
	}
	ack_times = ack_future.get();

	LOG(INFO) << "Ack size:" << ack_times.size() << " pub size:" << pub_times.size();
	//assert(ack_times.size() == pub_times.size());

	std::sort(pub_times.begin(), pub_times.end());
	std::sort(ack_times.begin(), ack_times.end());

	auto start = pub_times.front();
	auto end = pub_times.back();
	std::chrono::duration<double> elapsed = end - start;
	double seconds = elapsed.count();

	size_t len = ack_times.size();

	std::vector<long long> latencies;
	for(size_t i=0; i<len; i++){
		latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(ack_times[i] - pub_times[i]).count());
	}
	std::sort(latencies.begin(), latencies.end());
	std::ofstream file("/home/domin/.CXL_EMUL/CDF_data.csv");
	file << "Latency (ns),CDF\n";
	for (size_t i = 0; i < latencies.size(); ++i) {
		if (i == 0 || latencies[i] != latencies[i - 1]) {
			double cdf = static_cast<double>(i + 1) / latencies.size();
			file << latencies[i] << "," << cdf << "\n";
		}
	}
	file.close();

	// Calculate bandwidth
	double bandwidthMbps = ((client_order_ * message_size) / seconds) / (1024 * 1024);  // Convert to Megabytes per second

	LOG(INFO) << "Bandwidth:" << bandwidthMbps << " MBps" ;
}

void MultipleClientsSingleThread(size_t num_threads, size_t total_message_size, size_t message_size, int ack_level, bool record_latency){
	LOG(INFO) << "Starting SingleClientMultipleThreads Throughput Test with " << num_threads << " threads, total message size:" << total_message_size;

	size_t client_id = 1;
	auto start = std::chrono::high_resolution_clock::now();
	std::vector<std::future<std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>>>> pub_futures;
	std::vector<std::future<std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>>>> ack_futures;

	for (size_t i = 0; i < num_threads; ++i) {
		pub_futures.emplace_back(std::async(std::launch::async, send_data, message_size, total_message_size, ack_level, client_id, record_latency));
		ack_futures.emplace_back(std::async(std::launch::async, read_ack, total_message_size, message_size, client_id, record_latency));
	}
	std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> pub_times;
	std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> ack_times;
	for(auto& future: pub_futures){
		auto vec = future.get();
		pub_times.insert(pub_times.end(), vec.begin(), vec.end());
	}
	for(auto& future: ack_futures){
		auto vec = future.get();
		ack_times.insert(ack_times.end(), vec.begin(), vec.end());
	}

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = end - start;
	double seconds = elapsed.count();

	// Calculate bandwidth
	double bandwidthMbps = ((client_order_ * message_size) / seconds) / (1024 * 1024);  // Convert to Megabytes per second

	LOG(INFO) << "Bandwidth:" << bandwidthMbps << " MBps" ;
}

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

void ThroughputTestRaw(size_t total_message_size, size_t message_size, int num_threads, int ack_level, int order){
	SingleClientMultipleThreads(num_threads, total_message_size, message_size, ack_level, true);
	//MultipleClientsSingleThread(num_threads, total_message_size, message_size, ack_level);
}

void ThroughputTest(size_t total_message_size, size_t message_size, int num_threads, int ack_level, int order){
	int n = total_message_size/message_size;
	LOG(INFO) << "[Throuput Test] total_message:" << total_message_size << " message_size:" << message_size << " n:" << n << " num_threads:" << num_threads;
	std::string message(message_size, 0);
	char topic[TOPIC_NAME_SIZE];
	memset(topic, 0, TOPIC_NAME_SIZE);
	memcpy(topic, "TestTopic", 9);

	Client c("127.0.0.1", std::to_string(BROKER_PORT), n/4);
	std::cout << "Client Created" << std::endl;
	c.CreateNewTopic(topic, order);
	c.Init(num_threads, 0, topic, ack_level, order, message_size);
	auto start = std::chrono::high_resolution_clock::now();
	for(int i=0; i<n; i++){
		c.Publish(message);
	}
	c.Poll(n);

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = end - start;
	double seconds = elapsed.count();
	double bandwidthMbps = ((message_size*n) / seconds) / (1024 * 1024);  // Convert to Megabytes per second
	std::cout << "Bandwidth: " << bandwidthMbps << " MBps" << std::endl;
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
		("s,total_message_size", "Total size of messages to publish", cxxopts::value<size_t>()->default_value("10066329600"))
		("m,size", "Size of a message", cxxopts::value<size_t>()->default_value("960"))
		("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
		("t,num_thread", "Number of request threads", cxxopts::value<size_t>()->default_value("24"));

	auto result = options.parse(argc, argv);
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads = result["num_thread"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	FLAGS_v = result["log_level"].as<int>();

	if(result["run_cgroup"].as<int>() > 0 && !CheckAvailableCores()){
		LOG(ERROR) << "CGroup core throttle is wrong";
		return -1;
	}

	ThroughputTest(total_message_size, message_size, num_threads, ack_level, order);

	return 0;
}
