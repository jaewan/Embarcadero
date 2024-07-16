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

#include <heartbeat.grpc.pb.h>
#include "common/config.h"
#include "../cxl_manager/cxl_manager.h"

using heartbeat_system::HeartBeat;

class Client{
	public:
		Client(std::string head_addr, std::string port):
			head_addr_(head_addr), port_(port), shutdown_(false){
				std::string addr = head_addr+":"+port;
				stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
				client_id_ = GenerateRandomNum();
				nodes_[0] = addr;
				cluster_probe_thread_ = std::thread([this](){
						this->ClusterProbeLoop();
						});
			}
		~Client(){
			LOG(INFO) << "Destructing Client";
			shutdown_ = true;
			cluster_probe_thread_.join();
		};

		void Publish(){
		};
	private:
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
							nodes_[GetBrokerId(addr)] = addr;
							client_info.add_nodes_info(broker_id);
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

		int GetBrokerId(const std::string& input) {
			size_t colonPos = input.find(':');
			if (colonPos == std::string::npos) {
				throw std::invalid_argument("Input string does not contain a colon");
			}
			std::string numberStr = input.substr(colonPos + 1);
			try {
				return std::stoi(numberStr) - PORT;
			} catch (const std::exception& e) {
				throw std::invalid_argument("Failed to convert to integer: " + std::string(e.what()));
			}
		}

		int GenerateRandomNum(){
			// Generate a random number
			std::random_device rd;
			std::mt19937 gen(rd());
			std::uniform_int_distribution<> dis(NUM_MAX_BROKERS, 999999);
			return  dis(gen);
		}

		std::string head_addr_;
		std::string port_;
		bool shutdown_;
		int client_id_;
		std::unique_ptr<HeartBeat::Stub> stub_;
		std::thread cluster_probe_thread_;
		// <broker_id, address::port of network_mgr>
		absl::flat_hash_map<int, std::string> nodes_;
		absl::Mutex mutex_;
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
#ifdef ACK
	event.events = EPOLLOUT | EPOLLIN | EPOLLET; // Edge-triggered for both read and write
	char ack[ACK_SIZE];
#else
	event.events = EPOLLOUT;
#endif
	epoll_ctl(efd, EPOLL_CTL_ADD, sock, &event);

	char *data = (char*)calloc(message_size+64, sizeof(char));

	Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)data;
	header->client_id = CLIENT_ID;
	header->size = message_size;
	header->total_order = 0;
	header->client_order = client_order_.fetch_add(1);
	int padding = message_size % 64;
	if(padding){
		padding = 64 - padding;
	}
	header->paddedSize = message_size + padding + sizeof(Embarcadero::MessageHeader);
	header->segment_header = nullptr;
	header->logical_offset = (size_t)-1; // Sentinel value
	header->next_message = nullptr;

	size_t run_count = total_message_size/message_size;


	ack_port_ = GenerateRandomPORT();
	Embarcadero::EmbarcaderoReq req;
	req.client_id = CLIENT_ID;
	req.client_order = 0;
	memset(req.topic, 0, TOPIC_NAME_SIZE);
	memcpy(req.topic, "TestTopic", 9);
	req.ack = ack_level;
	req.port = ack_port_;
	req.size = message_size + sizeof(Embarcadero::MessageHeader);
	int n, i;
	struct epoll_event events[10]; // Adjust size as needed
	bool running = true;
	size_t sent_bytes = 0;
	VLOG(3) << "Start publishing  on fd" << sock;
	//This is to measure throughput more precisely
	if(!record_latency){
		times.emplace_back(std::chrono::high_resolution_clock::now());
	}
	while (running) {
		n = epoll_wait(efd, events, 10, -1);
		for (i = 0; i < n; i++) {
			if (events[i].events & EPOLLOUT) {
				ssize_t bytesSent = send(sock, (int8_t*)(&req) + sent_bytes, sizeof(req) - sent_bytes, 0);
				if (bytesSent < 0) {
					if (errno != EAGAIN) {
						perror("send failed");
						running = false;
						break;
					}
				} 
				sent_bytes += bytesSent;
				if(sent_bytes == sizeof(req)){
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

	sent_bytes = 0;
	running = true;
	bool stop_sending = false;
	int num_send_called_this_msg = 0;
	while (running) {
		for (; i < n; i++) {
			if (events[i].events & EPOLLOUT && (!stop_sending || header->client_order < run_count)) {
				if(!stop_sending && header->client_order >= run_count){
					stop_sending = true;
					header->client_id = -1;
				}
				if(record_latency)
					times.emplace_back(std::chrono::high_resolution_clock::now());
				ssize_t bytesSent = send(sock, (uint8_t*)data + sent_bytes, req.size - sent_bytes, 0);
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
				if(sent_bytes == req.size){
					sent_bytes = 0;
					num_send_called_this_msg = 0;
					header->client_order = client_order_.fetch_add(1);
				}else{
					if(record_latency && num_send_called_this_msg>1){
						times.pop_back();
					}
				}
			}
#ifdef ACK
			if (events[i].events & EPOLLIN) {
				ssize_t bytesReceived = recv(sock, ack, ACK_SIZE, 0);
				if (bytesReceived <= 0) {
					if (bytesReceived == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
						perror("recv failed or connection closed");
						running = false;
						break;
					}
				} else {
					totalBytesRead_.fetch_add(bytesReceived);
				}
			}
#endif
		}
		if (header->client_order >= run_count && stop_sending) { // Example break condition
#ifdef ACK
			if(totalBytesRead_ >= total_message_size)
#endif
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
	VLOG(3) << "Start reading ack: " << to_read << " on fd" << client_sock;
	while (to_read > 0){
		VLOG(4) << "Before reading" ;
		if((bytesReceived = recv(client_sock, (uint8_t*)data + ((TOTAL_DATA_SIZE/message_size) - to_read) , 1024, 0))){
			if(record_latency){
				auto t = std::chrono::high_resolution_clock::now();
				for(int i =0; i < bytesReceived; i++){
					times.emplace_back(t);
				}
			}
			to_read -= bytesReceived;
			VLOG(4) << "Ack received:" << bytesReceived;
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

int main(int argc, char* argv[]) {
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();
	FLAGS_logtostderr = 1; // log only to console, no files.
	cxxopts::Options options("embarcadero-throughputTest", "Embarcadero Throughput Test");

	options.add_options()
		("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
		("a,ack_level", "Acknowledgement level", cxxopts::value<int>()->default_value("1"))
		("s,total_message_size", "Total size of messages to publish", cxxopts::value<size_t>()->default_value("10066329600"))
		("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
		("m,size", "Size of a message", cxxopts::value<size_t>()->default_value("960"))
		("t,num_thread", "Number of request threads", cxxopts::value<size_t>()->default_value("32"));

	auto result = options.parse(argc, argv);
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads = result["num_thread"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	FLAGS_v = result["log_level"].as<int>();

	if(result["run_cgroup"].as<int>() > 0 && !CheckAvailableCores()){
		LOG(ERROR) << "CGroup core throttle is wrong";
		return -1;
	}
	Client c("127.0.0.1", std::to_string(BROKER_PORT));
	SingleClientMultipleThreads(num_threads, total_message_size, message_size, ack_level, true);
	//MultipleClientsSingleThread(num_threads, total_message_size, message_size, ack_level);

	return 0;
}
