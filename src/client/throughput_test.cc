#include <iostream>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <vector>
#include <cstring>
#include <random>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>

#include <grpcpp/grpcpp.h>
#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>
#include <mimalloc.h>
#include "absl/synchronization/mutex.h"
#include "folly/ProducerConsumerQueue.h"

#include "common/config.h"
#include "../cxl_manager/cxl_manager.h"
#include "corfu_client.h"

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY    0x4000000
#endif

#include <heartbeat.grpc.pb.h>

#define BATCH_OPTIMIZATION 1

using heartbeat_system::HeartBeat;
using heartbeat_system::SequencerType;

heartbeat_system::SequencerType parseSequencerType(const std::string& value) {
	if (value == "EMBARCADERO") return heartbeat_system::SequencerType::EMBARCADERO;
	if (value == "KAFKA") return heartbeat_system::SequencerType::KAFKA;
	if (value == "SCALOG") return heartbeat_system::SequencerType::SCALOG;
	if (value == "CORFU") return heartbeat_system::SequencerType::CORFU;
	throw std::runtime_error("Invalid SequencerType: " + value);
}

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
class Buffer{
	public:
		Buffer(size_t num_buf, int client_id, size_t message_size, int order=0):
			bufs_(num_buf), order_(order){
				// 4K is a buffer space as message can go over
				//size_t buf_size_ = (total_buf_size / num_buf) + 4096; 

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
			for(size_t i = 0; i < num_buf_; i++){
				munmap(bufs_[i].buffer, bufs_[i].len);
			}
		}

		bool AddBuffers(size_t n, size_t buf_size){
			size_t idx = num_buf_.fetch_add(n);
			if(idx + n > bufs_.size()){
				LOG(ERROR) << "!!!! Buffer allocation OOM. Try increasing the num buffer !!!!";
				return false;
			}

			for (size_t i = 0; i < n; i++) {
				size_t allocated;
				void* new_buffer = mmap_large_buffer(buf_size, allocated);
				if (new_buffer == nullptr) {
					return false;
				}
				bufs_[idx + i].buffer = new_buffer;
				bufs_[idx + i].len = allocated;
#ifdef BATCH_OPTIMIZATION
				bufs_[idx + i].tail = sizeof(Embarcadero::BatchHeader);
#endif
			}
			return true;
		}

#ifdef BATCH_OPTIMIZATION
		bool Write(size_t client_order, char* msg, size_t len, size_t paddedSize){
			static const size_t header_size = sizeof(Embarcadero::MessageHeader);

			void* buffer;
			size_t head, tail;
			{
			while(!bufs_[write_buf_id_].mu.TryLock()){
				write_buf_id_ = (write_buf_id_+1) % num_buf_;
			}
			size_t lockedIdx = write_buf_id_;
			buffer = bufs_[write_buf_id_].buffer;;
			head = bufs_[write_buf_id_].writer_head;
			tail = bufs_[write_buf_id_].tail;
			// Buffer Full, circle the buffer
			if(tail + header_size + paddedSize + paddedSize/*buffer*/ > bufs_[lockedIdx].len){
				VLOG(3) << "Buffer:" << write_buf_id_ << " full. Circle";
				// Seal what is written now to move to next buffer
				Embarcadero::BatchHeader *batch_header = (Embarcadero::BatchHeader*)((uint8_t*)bufs_[write_buf_id_].buffer + head);
				batch_header->next_reader_head = 0;
				batch_header->batch_seq = batch_seq_.fetch_add(1);
				batch_header->total_size = bufs_[write_buf_id_].tail - head - sizeof(Embarcadero::BatchHeader);
				batch_header->num_msg = bufs_[write_buf_id_].num_msg;

				bufs_[write_buf_id_].num_msg = 0;
				bufs_[write_buf_id_].writer_head = 0;
				bufs_[write_buf_id_].tail = sizeof(Embarcadero::BatchHeader);

				write_buf_id_ = (write_buf_id_+1) % num_buf_; 
				bufs_[lockedIdx].mu.Unlock();
				return Write(client_order, msg, len, paddedSize);;
			}
			bufs_[write_buf_id_].tail += paddedSize;
			bufs_[write_buf_id_].num_msg++;
			// Seal if written messages > BATCH_SIZE
			if((bufs_[write_buf_id_].tail - head) >= BATCH_SIZE){
				Embarcadero::BatchHeader *batch_header = (Embarcadero::BatchHeader*)((uint8_t*)bufs_[write_buf_id_].buffer + head);
				batch_header->next_reader_head = bufs_[write_buf_id_].tail;
				batch_header->batch_seq = batch_seq_.fetch_add(1);
				batch_header->total_size = bufs_[write_buf_id_].tail - head - sizeof(Embarcadero::BatchHeader);
				batch_header->num_msg = bufs_[write_buf_id_].num_msg;

				bufs_[write_buf_id_].num_msg = 0;
				bufs_[write_buf_id_].writer_head = bufs_[write_buf_id_].tail;
				bufs_[write_buf_id_].tail += sizeof(Embarcadero::BatchHeader);

				write_buf_id_ = (write_buf_id_+1) % num_buf_; 
			} 
			bufs_[lockedIdx].mu.Unlock();
			}

			header_.paddedSize = paddedSize;
			header_.size = len;
			header_.client_order = client_order;
			memcpy((void*)((uint8_t*)buffer + tail), &header_, header_size);
			memcpy((void*)((uint8_t*)buffer + tail + header_size), msg, len);
			return true;
		}

		void* Read(int bufIdx){
			Embarcadero::BatchHeader *batch_header = (Embarcadero::BatchHeader*)((uint8_t*)bufs_[bufIdx].buffer + bufs_[bufIdx].reader_head);
			if(batch_header->total_size != 0 && batch_header->num_msg != 0){
				bufs_[bufIdx].reader_head = batch_header->next_reader_head;
				return (void*)batch_header;
			}else{
				//Queue is empty
				size_t head, tail, num_msg, batch_seq;
				{
				absl::MutexLock lock(&bufs_[bufIdx].mu);
				if(batch_header->batch_seq != 0 && batch_header->total_size != 0 && batch_header->num_msg != 0){
					bufs_[bufIdx].reader_head = batch_header->next_reader_head;
					return (void*)batch_header;
				}
				head = bufs_[bufIdx].writer_head;
				tail = bufs_[bufIdx].tail;
				num_msg = bufs_[bufIdx].num_msg;
				if(num_msg == 0){
					return nullptr;
				}
				bufs_[bufIdx].reader_head = tail;
				bufs_[bufIdx].writer_head = tail;
				bufs_[bufIdx].tail = tail + sizeof(Embarcadero::BatchHeader);
				bufs_[bufIdx].num_msg = 0;
				batch_seq = batch_seq_.fetch_add(1);
				}
				Embarcadero::BatchHeader *batch_header = (Embarcadero::BatchHeader*)((uint8_t*)bufs_[bufIdx].buffer + head);
				batch_header->batch_seq = batch_seq;
				batch_header->total_size = tail - head - sizeof(Embarcadero::BatchHeader);
				batch_header->num_msg = num_msg;
				batch_header->next_reader_head = tail;
				return (void*)batch_header;
			}
		}
#else
		bool Write(int bufIdx, size_t client_order, char* msg, size_t len, size_t paddedSize){
			static const size_t header_size = sizeof(Embarcadero::MessageHeader);
			header_.paddedSize = paddedSize;
			header_.size = len;
			header_.client_order = client_order;
			if(bufs_[bufIdx].tail + header_size + paddedSize > bufs_[bufIdx].len){
				LOG(ERROR) << "tail:" << bufs_[bufIdx].tail << " write size:" << paddedSize << " will go over buffer:" << bufs_[bufIdx].len;
				return false;
			}
			memcpy((void*)((uint8_t*)bufs_[bufIdx].buffer + bufs_[bufIdx].tail), &header_, header_size);
			memcpy((void*)((uint8_t*)bufs_[bufIdx].buffer + bufs_[bufIdx].tail + header_size), msg, len);
			//std::atomic_thread_fence(std::memory_order_release);
			bufs_[bufIdx].tail += paddedSize;
			return true;
		}

		void* Read(int bufIdx, size_t &len ){
			if(order_ == 3){
				while(!shutdown_ && bufs_[bufIdx].tail - bufs_[bufIdx].reader_head < BATCH_SIZE){
					std::this_thread::yield();
				}
				size_t head = bufs_[bufIdx].reader_head;
				//std::atomic_thread_fence(std::memory_order_acquire);
				len = bufs_[bufIdx].tail - head;
				if(len==0){
					return nullptr;
				}
				len = BATCH_SIZE;
				bufs_[bufIdx].reader_head += BATCH_SIZE;
				return (void*)((uint8_t*)bufs_[bufIdx].buffer + head);
			}else{
				while(!shutdown_ && bufs_[bufIdx].tail <= bufs_[bufIdx].reader_head){
					std::this_thread::yield();
				}
				size_t head = bufs_[bufIdx].reader_head;
				//std::atomic_thread_fence(std::memory_order_acquire);
				size_t tail = bufs_[bufIdx].tail;
				len = tail - head;
				bufs_[bufIdx].reader_head = tail;
				return (void*)((uint8_t*)bufs_[bufIdx].buffer + head);
			}
		}
#endif

		void ReturnReads(){
			shutdown_ = true;
		}

	private:
		struct alignas(64) Buf{
			void* buffer;
			size_t len;
			size_t writer_head;
			size_t reader_head;
			size_t tail;
			size_t num_msg;
			absl::Mutex mu;
			Buf() : writer_head(0),reader_head(0), tail(0), num_msg(0){}
		};
		size_t write_buf_id_ = 0;
		std::vector<Buf> bufs_;
		int order_;
		std::atomic<size_t> num_buf_{0};
		std::atomic<size_t> batch_seq_{0};
		bool shutdown_ {false};
		Embarcadero::MessageHeader header_;
		absl::flat_hash_set<size_t> full_idx_;
};

class Publisher{
	public:
		Publisher(char topic[TOPIC_NAME_SIZE], std::string head_addr, std::string port, int num_threads_per_broker,
		size_t message_size, size_t queueSize, int order, 
		SequencerType seq_type = heartbeat_system::SequencerType::EMBARCADERO):
			head_addr_(head_addr), port_(port),
			client_id_(GenerateRandomNum()), num_threads_per_broker_(num_threads_per_broker), message_size_(message_size),
			queueSize_(queueSize / num_threads_per_broker),
			pubQue_(num_threads_per_broker_*NUM_MAX_BROKERS, client_id_, message_size, order),
			seq_type_(seq_type),
			sent_bytes_per_broker_(NUM_MAX_BROKERS){
				memcpy(topic_, topic, TOPIC_NAME_SIZE);
				std::string addr = head_addr+":"+port;
				stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
				nodes_[0] = head_addr+":"+std::to_string(PORT);
				brokers_.emplace_back(0);
				VLOG(3) << "Publisher Constructed";
			}

		~Publisher(){
			publish_finished_ = true;
			shutdown_ = true;
			context_.TryCancel();

			for(auto &t : threads_){
				if(t.joinable()){
					t.join();
				}
			}
			if(cluster_probe_thread_.joinable()){
				cluster_probe_thread_.join();
			}
			if(ack_thread_.joinable()){
				ack_thread_.join();
			}
			if(real_time_throughput_measure_thread_ .joinable()){
				real_time_throughput_measure_thread_.join();
			}
			VLOG(3) << "Publisher Destructed";
		};

		void Init(int ack_level){
			ack_level_ = ack_level;
			ack_port_ = GenerateRandomNum();
			if(ack_level >= 2){
				ack_thread_ = std::thread([this](){
					this->EpollAckThread();
					});
				while(thread_count_.load() !=  1){std::this_thread::yield();}
				thread_count_.store(0);
			}
			cluster_probe_thread_ = std::thread([this](){
					this->SubscribeToClusterStatus();
					});
			while(!connected_){
				std::this_thread::yield();
			}
			if(seq_type_ == heartbeat_system::SequencerType::CORFU){
			corfu_client_ = std::make_unique<CorfuSequencerClient>(
                //std::string("localhost:") + std::to_string(CORFU_SEQ_PORT));
                std::string("192.168.60.173:") + std::to_string(CORFU_SEQ_PORT));
			}
			while(thread_count_.load() != num_threads_.load()){std::this_thread::yield();}
			return;
		}

		void Publish(char* message, size_t len){
			const static size_t header_size = sizeof(Embarcadero::MessageHeader);
			size_t padded = len % header_size;
			if(padded){
				padded = 64 - padded;
			}
			padded = len + padded + header_size;
#ifdef BATCH_OPTIMIZATION
			pubQue_.Write(client_order_, message, len, padded);
#else
			const static size_t batch_size = BATCH_SIZE;
			static size_t i = 0;
			static size_t j = 0;
			size_t n = batch_size/(padded);
			if(n == 0)
				n = 1;
			pubQue_.Write(i, client_order_, message, len, padded);
			j++;
			if(j == n){
				i = (i+1)%num_threads_;
				j = 0;
			}
#endif
			client_order_++;
		}

		void Poll(size_t n){
			publish_finished_ = true;
			pubQue_.ReturnReads();
			while(client_order_ < n){
				std::this_thread::yield();
			}
			shutdown_ = true;
			context_.TryCancel();
			for(auto &t : threads_){
				if(t.joinable())
					t.join();
			}
			return;
		}

		void DEBUG_check_send_finish(){
			publish_finished_ = true;
			pubQue_.ReturnReads();
			for(auto &t : threads_){
				if(t.joinable()){
					t.join();
				}
			}
			return;
		}

		void FailBrokers(size_t total_message_size, double failure_percentage, std::function<bool()> killbrokers){
			measure_real_time_throughput_ = true;
			size_t num_brokers = nodes_.size();
			for(size_t i = 0; i < num_brokers; i++){
				sent_bytes_per_broker_[i].store(0);
			}

			kill_brokers_thread_ = std::thread([=,this](){
				size_t bytes_to_kill_brokers = total_message_size * failure_percentage;
				while(total_sent_bytes_ < bytes_to_kill_brokers){
					std::this_thread::yield();
				}
				killbrokers();
			});
			real_time_throughput_measure_thread_ = std::thread([=,this](){
				std::vector<size_t> prev_throughputs(num_brokers);
				std::vector<std::vector<size_t>> throughputs(num_brokers);

				std::string filename("/home/domin/Jae/Embarcadero/data/failure/real_time_throughput.csv");
				std::ofstream throughputFile(filename);
				if(!throughputFile.is_open()){
					LOG(ERROR) << "Failed to open file for writing";
					return ;
				}
				for (size_t i = 0; i < num_brokers; i++){
					throughputFile << i << ",";
				}
				throughputFile <<"RealTimeThroughput\n";

				while(!shutdown_){
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					size_t sum =0;
					for(size_t i = 0; i < num_brokers; i++){
						size_t bytes = sent_bytes_per_broker_[i].load(std::memory_order_relaxed);
						size_t real_time_throughput = (bytes-prev_throughputs[i]);
						throughputs[i].emplace_back(real_time_throughput);
						throughputFile << (real_time_throughput*200/(1024*1024)) << ",";
						sum += (real_time_throughput*200/(1024*1024));
						prev_throughputs[i] = bytes;
					}
					throughputFile << sum << "\n";
				}
				kill_brokers_thread_.join();
			});
		}


	private:
		std::string head_addr_;
		std::string port_;
		int client_id_;
		size_t num_threads_per_broker_;
		std::atomic<int> num_threads_{0};
		size_t message_size_;
		size_t queueSize_;
		Buffer pubQue_;
		SequencerType  seq_type_;
		std::unique_ptr<CorfuSequencerClient> corfu_client_;

		bool shutdown_{false};
		bool publish_finished_ {false};
		bool connected_{false};
		size_t client_order_ = 0;
		// Used to measure real time throughput of failure bench
		// Since it is updated by multi-thread, no dynamic addition allowed
		std::atomic<size_t> total_sent_bytes_{0};
		std::vector<std::atomic<size_t>> sent_bytes_per_broker_;
		bool measure_real_time_throughput_ = false;
		std::thread real_time_throughput_measure_thread_;
		std::thread kill_brokers_thread_;;
		// Context for clusterprobe
		grpc::ClientContext context_;
		std::unique_ptr<HeartBeat::Stub> stub_;
		std::thread cluster_probe_thread_;
		// <broker_id, address::port of network_mgr>
		absl::flat_hash_map<int, std::string> nodes_;
		absl::Mutex mutex_;
		std::vector<int> brokers_;
		char topic_[TOPIC_NAME_SIZE];
		int ack_level_;
		int ack_port_;
		std::vector<std::thread> threads_;
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
				LOG(ERROR) << "setsockopt(SO_REUSEADDR) failed\n";
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
				LOG(ERROR) << "Bind failed:" << strerror(errno);
				close(server_sock);
				return;
			}

			if (listen(server_sock, SOMAXCONN) < 0) {
				LOG(ERROR) << "Listen failed\n";
				close(server_sock);
				return;
			}

			int epoll_fd = epoll_create1(0);
			if (epoll_fd == -1) {
				LOG(ERROR) << "Failed to create epoll file descriptor\n";
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

			int max_events = NUM_MAX_BROKERS;
			std::vector<epoll_event> events(max_events);
			char buffer[1024*1024];
			size_t total_received = 0;
			int EPOLL_TIMEOUT = 1; // 1 millisecond timeout
			std::vector<int> client_sockets;

			thread_count_.fetch_add(1);

			while (!shutdown_ || total_received < client_order_) {
				int num_events = epoll_wait(epoll_fd, events.data(), max_events, EPOLL_TIMEOUT);
				for (int i = 0; i < num_events; i++) {
					if (events[i].data.fd == server_sock) {
						// New connection
						sockaddr_in client_addr;
						socklen_t client_addr_len = sizeof(client_addr);
						int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
						if (client_sock == -1) {
							LOG(ERROR) << "Accept failed";
							continue;
						}
						//Make client_sock non-blocking
						int flags = fcntl(client_sock, F_GETFL, 0);
						fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);
						event.events = EPOLLIN | EPOLLET;
						event.data.fd = client_sock;
						if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &event) == -1) {
							LOG(ERROR) << "Failed to add client socket to epoll";
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
							LOG(ERROR) << "Broker may have been disconnected. " << strerror(errno) << " Total bytes received: " << total_received;
							continue;
							/*
							epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
							close(client_sock);
							client_sockets.erase(std::remove(client_sockets.begin(), client_sockets.end(), client_sock), client_sockets.end());
							*/
						} else if (bytes_received == -1) {
							if (errno != EAGAIN && errno != EWOULDBLOCK) {
								// Error occurred
								LOG(ERROR) << "recv error: " << strerror(errno);
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

		void PublishThread(int broker_id, int pubQuesIdx){
			int sock = -1;
			int efd = -1;
			auto connect_to_server = [&](size_t brokerId) -> bool {
				close(sock);
				close(efd);

				std::string addr;
				size_t num_brokers;
				{
				absl::MutexLock lock(&mutex_);
				auto[_addr, _addressPort] = ParseAddressPort(nodes_[brokerId]);
				addr = _addr;
				num_brokers = nodes_.size();
				}
				sock = GetNonblockingSock(addr.data(), PORT + brokerId);
				efd = epoll_create1(0);
				if (efd < 0) {
					LOG(ERROR) << "epoll_create1 failed: " << strerror(errno);
					close(sock);
					return false;
				}

				struct epoll_event event;
				event.data.fd = sock;
				event.events = EPOLLOUT;
				if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &event) != 0) {
					LOG(ERROR) << "epoll_ctl failed: " << strerror(errno);
					close(sock);
					close(efd);
					return false;
				}

				// *********** Initiate Shake ***********

				Embarcadero::EmbarcaderoReq shake;
				shake.client_req = Embarcadero::Publish;
				shake.client_id = client_id_;
				memcpy(shake.topic, topic_, TOPIC_NAME_SIZE);
				shake.ack = ack_level_;
				shake.port = ack_port_;
				shake.num_msg = num_brokers; // shake.num_msg used as num brokers at pub

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
									return false;
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
				return true;
			}; // End of connect_to_server lambda

			if(!connect_to_server(broker_id))
				return;

			// *********** Sending Messages ***********
			thread_count_.fetch_add(1);
			size_t batch_seq = pubQuesIdx;

			while(!shutdown_){
				size_t len;
				int bytesSent = 0;
#ifdef BATCH_OPTIMIZATION
				Embarcadero::BatchHeader *batch_header = (Embarcadero::BatchHeader*)pubQue_.Read(pubQuesIdx);
				if(batch_header == nullptr || batch_header->total_size == 0){
					if(publish_finished_){
						break;
					}else{
						//std::this_thread::yield();
						std::this_thread::sleep_for(std::chrono::microseconds(1));
						continue;
					}
				}
				void *msg = (uint8_t*)batch_header + sizeof(Embarcadero::BatchHeader);
				len = batch_header->total_size;
				auto send_batch_header = [&]() -> void{
					if(seq_type_ == heartbeat_system::SequencerType::CORFU){
						batch_header->broker_id = broker_id;
						corfu_client_->GetTotalOrder(batch_header);
						Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)msg;
						size_t total_order = batch_header->total_order;
						for(size_t i = 0; i < batch_header->num_msg; i++){
							header->total_order = total_order++;
						}
					}
					bytesSent = send(sock, (uint8_t*)(batch_header), sizeof(Embarcadero::BatchHeader), 0);
					while(bytesSent < (ssize_t)sizeof(Embarcadero::BatchHeader)){
						if(bytesSent < 0 ){
							LOG(ERROR) << "Batch send failed!! " << strerror(errno);
							return;
						}
						bytesSent += send(sock, (uint8_t*)(batch_header) + bytesSent, sizeof(Embarcadero::BatchHeader) - bytesSent, 0);
					}
				};
#else
				void *msg = pubQue_.Read(pubQuesIdx, len);
				if(len == 0){
					break;
				}
				Embarcadero::BatchHeader batch_header;
				batch_header.total_size = len;
				//TODO(Jae) This assumes static message sizes. Must make it count the messages to allow dynamic msg sizes
				batch_header.num_msg = len/((Embarcadero::MessageHeader *)msg)->paddedSize;
				batch_header.batch_seq = batch_seq;
				auto send_batch_header = [&]() -> void{
					bytesSent = send(sock, (uint8_t*)(&batch_header), sizeof(Embarcadero::BatchHeader), 0);
					while(bytesSent < (ssize_t)sizeof(Embarcadero::BatchHeader)){
						if(bytesSent < 0 ){
							LOG(ERROR) << "Batch send failed!!";
							return;
						}
						bytesSent += send(sock, (uint8_t*)(&batch_header) + bytesSent, sizeof(Embarcadero::BatchHeader) - bytesSent, 0);
					}
				};
#endif
				send_batch_header();

				size_t sent_bytes = 0;
				size_t zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;

				while(sent_bytes < len){
					size_t remaining_bytes = len - sent_bytes;
					size_t to_send = std::min(remaining_bytes, zero_copy_send_limit);
					// First attempts to send messages for efficiency. 
					// epoll_wait at failure where there's not enough buffer space
					/*
					if(to_send < 1UL<<16)
						bytesSent = send(sock, (uint8_t*)msg + sent_bytes, to_send, 0);
					else
						bytesSent = send(sock, (uint8_t*)msg + sent_bytes, to_send, MSG_ZEROCOPY);
						*/
						bytesSent = send(sock, (uint8_t*)msg + sent_bytes, to_send, 0);

					if (bytesSent > 0) {
						sent_bytes_per_broker_[broker_id].fetch_add(bytesSent);
						total_sent_bytes_.fetch_add(bytesSent);
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
						zero_copy_send_limit = std::max(zero_copy_send_limit / 2, 1UL<<6); // Cap backoff at 1000ms
					} else if (bytesSent < 0) {
						int brokerId;
						{
							// Remove the crashed broker.
							absl::MutexLock lock(&mutex_);
							auto it = std::find(brokers_.begin(), brokers_.end(), broker_id);
							if(it != brokers_.end()){
								brokers_.erase(it);
								nodes_.erase(broker_id);
							}
							brokerId = brokers_[(pubQuesIdx%num_threads_per_broker_)%brokers_.size()];
						}
						if(!connect_to_server(brokerId)){
							LOG(ERROR) << "Send failed: " << strerror(errno);
							return;
						}
						send_batch_header();
						sent_bytes = 0;

						VLOG(3)  << "pubQuesIdx:" << pubQuesIdx << " to broker:" << broker_id << " detected failure. Redirect to :" << brokerId;
						broker_id = brokerId;
					}
				}
#ifdef BATCH_OPTIMIZATION
				memset(batch_header,0, batch_header->total_size + sizeof(Embarcadero::BatchHeader));
#endif

				// Update here for fault tolerance.
				// At broker failure, we should send the same batch to another broker
				batch_seq += num_threads_.load();
			}
			close(sock);
			close(efd);
		}

		void SubscribeToClusterStatus(){
			heartbeat_system::ClientInfo client_info;
			heartbeat_system::ClusterStatus cluster_status;
			{
			absl::MutexLock lock(&mutex_);
			for (const auto &it: nodes_){
				client_info.add_nodes_info(it.first);
			}
			}
			std::unique_ptr<grpc::ClientReader<ClusterStatus>> reader(
					stub_->SubscribeToCluster(&context_, client_info));
			while(!shutdown_){
				if(reader->Read(&cluster_status)){
					const auto& new_nodes = cluster_status.new_nodes();
					if(!new_nodes.empty()){
						absl::MutexLock lock(&mutex_);
						if(!connected_){
							int num_brokers = 1 + new_nodes.size();
							queueSize_ /= num_brokers;
						}
						for(const auto& addr:new_nodes){
							int broker_id = GetBrokerId(addr);
							nodes_[broker_id] = addr;
							brokers_.emplace_back(broker_id);
							if(!AddPublisherThreads(num_threads_per_broker_, broker_id))
								return;
							//Make sure the brokers are sorted to have threads send in round robin in broker_id seq
							//This is needed for order3
							std::sort(brokers_.begin(), brokers_.end());
						}
					}
					if(!connected_){
						// TODO(Jae) receive head node from this rpc to make it cleaner 
						// Connect to head node.
						if(!AddPublisherThreads(num_threads_per_broker_, brokers_[0]))
							return;
						// set here to make Init() to return after first connections are made
						connected_ = true;
					}
				}
			}
			grpc::Status status = reader->Finish();
		}

		// Embarcadero cluster stateless mode. Client polls the Cluster Status.
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
							VLOG(3) << "New Node reported:" << broker_id << " addr:" << addr;
							nodes_[broker_id] = addr;
							brokers_.emplace_back(broker_id);
							//Make sure the brokers are sorted to have threads send in round robin in broker_id seq
							//This is needed for order3
							std::sort(brokers_.begin(), brokers_.end());
							client_info.add_nodes_info(broker_id);
						}
					}
				}else{
					LOG(ERROR) << "Head is dead, try reaching other brokers for a newly elected head";
				}
				std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL));
			}
		}

	bool AddPublisherThreads(size_t num_threads, int broker_id){
		if(pubQue_.AddBuffers(num_threads_per_broker_, queueSize_)){
			for (size_t i=0; i < num_threads; i++){
				int n = num_threads_.fetch_add(1);
				threads_.emplace_back(&Publisher::PublishThread, this, broker_id, n);
			}
		}else
			return false;
		return true;
	}
};

//TODO(Jae) Broker Failure is not handled, dynamic node addition not handled
class Subscriber{
	public:
		Subscriber(std::string head_addr, std::string port, char topic[TOPIC_NAME_SIZE], bool measure_latency=false):
			head_addr_(head_addr), port_(port), shutdown_(false), connected_(false), measure_latency_(measure_latency),
			buffer_size_((1UL<<33)), messages_idx_(0), client_id_(GenerateRandomNum()){
				messages_.resize(2);
				memcpy(topic_, topic, TOPIC_NAME_SIZE);
				std::string addr = head_addr+":"+port;
				stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
				nodes_[0] = head_addr+":"+std::to_string(PORT);
				cluster_probe_thread_ = std::thread([this](){
						this->SubscribeToClusterStatus();
						});
				while(!connected_){
					std::this_thread::yield();
				}
				VLOG(3) << "Subscriber Constructed";
			}

		~Subscriber(){
			shutdown_ = true;
			context_.TryCancel();
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
			VLOG(3) << "Subscriber Destructed";
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
			DEBUG_do_not_check_order_ = true;
			if(DEBUG_do_not_check_order_)
				return true;
			int idx = 0;
			for(auto &msg_pair : messages_[idx]){
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
					off += header->paddedSize;
					while(off > m->timestamps[recv_latency_idx].first){
						recv_latency_idx++;
					}
					long long send_nanoseconds_since_epoch;
					memcpy(&send_nanoseconds_since_epoch, (void*)((uint8_t*)header + sizeof(Embarcadero::MessageHeader)), sizeof(long long));
					auto received_time_point = std::chrono::time_point<std::chrono::steady_clock>(
							std::chrono::nanoseconds(send_nanoseconds_since_epoch));

					auto latency = m->timestamps[recv_latency_idx].second - received_time_point;;
					latencies.emplace_back(std::chrono::duration_cast<std::chrono::nanoseconds>(latency).count());
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
		grpc::ClientContext context_;
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
		int client_id_;
		bool DEBUG_do_not_check_order_ = false;

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
						//int idx = 0;
						struct msgIdx *m = fd_to_msg[fd].second; // &messages_[idx][fd_to_msg_idx[fd]].second;
						void* buf = fd_to_msg[fd].first;// messages_[idx][fd_to_msg_idx[fd]].first;
						// This ensures the receive never goes out of the boundary
						// bit it may cause the incomplete recv
						size_t to_read = buffer_size_ - m->offset;
						if(to_read == 0){
							LOG(ERROR) << "Subscriber buffer is full. Overwriting from head. Increase buffer_size_ or do not check message correctness";
							DEBUG_do_not_check_order_ = true;
							m->offset = 0;
							to_read = buffer_size_;
						}
						int bytes_received = recv(fd, (uint8_t*)buf + m->offset, to_read, 0);
						if (bytes_received > 0) {
							DEBUG_count_.fetch_add(bytes_received);
							m->offset += bytes_received;
							if(measure_latency_){
								m->timestamps.emplace_back(m->offset, std::chrono::steady_clock::now());
							}
						} else if (bytes_received == 0) {
							LOG(ERROR) << "epoll_fd:" << epoll_fd << " Server sock: " << fd << " disconnected:" << strerror(errno);
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
			int epoll_fd = epoll_create1(0);
			if (epoll_fd < 0) {
				LOG(ERROR) << "Failed to create epoll instance";
				return ;
			}
			for (int i=0; i < NUM_SUB_CONNECTIONS; i++){
				int sock = GetNonblockingSock(addr.data(), PORT + broker_id, false);

				std::pair<void*, msgIdx*> msg(static_cast<void*>(calloc(buffer_size_, sizeof(char))), (msgIdx*)malloc(sizeof(msgIdx)));
				// This is for client retrieval, double buffer
				//std::pair<void*, msgIdx> msg1(static_cast<void*>(malloc(buffer_size_)), msgIdx(broker_id));
				messages_[0].push_back(msg);
				//messages_[1].push_back(msg1);
				fd_to_msg.insert({sock, msg});;

				//Create a connection by Sending a Sub request
				Embarcadero::EmbarcaderoReq shake;
				shake.num_msg = 0;
				shake.client_id = client_id_;
				shake.last_addr = 0;
				shake.client_req = Embarcadero::Subscribe;
				memcpy(shake.topic, topic_, TOPIC_NAME_SIZE);
				int ret = send(sock, &shake, sizeof(shake), 0);
				if(ret < (int)sizeof(shake)){
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
			}
			subscribe_threads_.emplace_back(&Subscriber::SubscribeThread, this, epoll_fd, fd_to_msg);
		}

		void SubscribeToClusterStatus(){
			heartbeat_system::ClientInfo client_info;
			heartbeat_system::ClusterStatus cluster_status;
			for (const auto &it: nodes_){
				client_info.add_nodes_info(it.first);
			}
			std::unique_ptr<grpc::ClientReader<ClusterStatus>> reader(
					stub_->SubscribeToCluster(&context_, client_info));
			CreateAConnection(0, nodes_[0]);
			while(!shutdown_){
				if(reader->Read(&cluster_status)){
					const auto& new_nodes = cluster_status.new_nodes();
					if(!new_nodes.empty()){
						absl::MutexLock lock(&mutex_);
						for(const auto& addr:new_nodes){
							int broker_id = GetBrokerId(addr);
							nodes_[broker_id] = addr;
							CreateAConnection(broker_id, addr);
						}
					}
					connected_ = true;
				}
			}
			grpc::Status status = reader->Finish();
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

// Fail num_brokers_to_fail when failure_percentage of messages were sent
double FailurePublishThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE], 
		 std::function<bool()> killbrokers){
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	double failure_percentage = result["failure_percentage"].as<double>();

	size_t n = total_message_size/message_size;

	LOG(INFO) << "[Failure Publish Throughput Test] total_message:" << total_message_size << 
		" message_size:" << message_size << " failure percentage:" << failure_percentage;
	char* message = (char*)malloc(sizeof(char)*message_size);

	size_t q_size = total_message_size + (total_message_size/message_size)*64 + 4096;
	q_size = std::max(q_size, static_cast<size_t>(1024));

	Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), num_threads_per_broker, message_size, q_size, order);
	p.Init(ack_level);
	p.FailBrokers(total_message_size, failure_percentage, killbrokers);
	auto start = std::chrono::high_resolution_clock::now();

	for(size_t i=0; i<n; i++){
		p.Publish(message, message_size);
	}
	p.DEBUG_check_send_finish();
	p.Poll(n);

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = end - start;
	double seconds = elapsed.count();
	double bandwidthMbps = ((message_size*n) / seconds) / (1024 * 1024);  // Convert to Megabytes per second
	LOG(INFO) << "Bandwidth: " << bandwidthMbps << " MBps";
	free(message);

	return bandwidthMbps;
}

double PublishThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE], 
														 std::atomic<int> &synchronizer){
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());

	size_t n = total_message_size/message_size;
	LOG(INFO) << "[Throuput Test] total_message:" << total_message_size << " message_size:" << message_size << " n:" << n << 
		" num_threads_per_broker:" << num_threads_per_broker;
	char* message = (char*)malloc(sizeof(char)*message_size);

	// + 4096 as buffer
	size_t q_size = total_message_size + (total_message_size/message_size)*64 + 4096;
	q_size = std::max(q_size, static_cast<size_t>(1024));

	Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), num_threads_per_broker, message_size, q_size, order, seq_type);
	p.Init(ack_level);
	// **************    Wait until other threads are initialized ************** //
	synchronizer.fetch_sub(1);
	while(synchronizer.load() != 0){
		std::this_thread::yield();
	}
	auto start = std::chrono::high_resolution_clock::now();

	for(size_t i=0; i<n; i++){
		p.Publish(message, message_size);
	}
	VLOG(3) << "[DEBUG] Finished publishing from client";
	p.DEBUG_check_send_finish();
	p.Poll(n);

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = end - start;
	double seconds = elapsed.count();
	double bandwidthMbps = ((message_size*n) / seconds) / (1024 * 1024);  // Convert to Megabytes per second

	LOG(INFO) << "Bandwidth: " << bandwidthMbps << " MBps";
	free(message);
	return bandwidthMbps;
}

double SubscribeThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]){
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	int order = result["order_level"].as<int>();

	LOG(INFO) << "[Subscribe Throuput Test] ";
	auto start = std::chrono::high_resolution_clock::now();
	Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic);
	s.DEBUG_wait(total_message_size, message_size);
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = end - start;
	double seconds = elapsed.count();
	double bandwidthMbps = (total_message_size/(1024*1024))/seconds;
	LOG(INFO) << bandwidthMbps << "MB/s";
	if(!s.DEBUG_check_order(order)){
		LOG(ERROR) << "Order check failed!!";
	}
	return bandwidthMbps;
}

std::pair<double, double> E2EThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]){
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());

	size_t n = total_message_size/message_size;
	LOG(INFO) << "[E2E Throuput Test] total_message:" << total_message_size << " message_size:" << message_size << " n:" << n << 
		" num_threads_per_broker:" << num_threads_per_broker;
	char* message = (char*)malloc(sizeof(char)*message_size);

	size_t q_size = total_message_size + (total_message_size/message_size)*64 + 4096;
	q_size = std::max(q_size, static_cast<size_t>(1024));

	Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), num_threads_per_broker, message_size, q_size, order, seq_type);
	p.Init(ack_level);
	Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, false);

	auto start = std::chrono::high_resolution_clock::now();
	for(size_t i=0; i<n; i++){
		p.Publish(message, message_size);
	}

	LOG(INFO) << "Finished publishing from client";

	p.DEBUG_check_send_finish();

	LOG(INFO) << "Polling for messages to be received";

	p.Poll(n);
	auto pub_end = std::chrono::high_resolution_clock::now();

	LOG(INFO) << "Waiting for messages to be received";

	s.DEBUG_wait(total_message_size, message_size);
	auto end = std::chrono::high_resolution_clock::now();
	double pubBandwidthMbps = ((message_size*n)/(std::chrono::duration<double>(pub_end - start)).count())/(1024*1024);
	auto e2eBandwidthMbps = ((message_size*n)/(std::chrono::duration<double>(end - start)).count())/(1024*1024);

	LOG(INFO) << "Checking order";

	s.DEBUG_check_order(order);

	free(message);

	LOG(INFO) << "Pub Bandwidth: " << pubBandwidthMbps << " MB/s";
	LOG(INFO) << "E2E Bandwidth: " << e2eBandwidthMbps << " MB/s";
	return std::make_pair(pubBandwidthMbps, e2eBandwidthMbps);
}

std::pair<double, double> LatencyTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]){
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());

	size_t n = total_message_size/message_size;
	LOG(INFO) << "[Latency Test] total_message:" << total_message_size << " message_size:" << message_size << " n:" << n << " num_threads_per_broker:" << num_threads_per_broker;
	char message[message_size];

	size_t q_size = total_message_size + (total_message_size/message_size)*64 + 4096;
	q_size = std::max(q_size, static_cast<size_t>(1024));
	Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), num_threads_per_broker, message_size, q_size, order, seq_type);
	Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, true);
	p.Init(ack_level);

	auto start = std::chrono::high_resolution_clock::now();
	for(size_t i=0; i<n; i++){
		auto timestamp = std::chrono::steady_clock::now();
		long long nanoseconds_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
				timestamp.time_since_epoch()).count();
		memcpy(message, &nanoseconds_since_epoch, sizeof(long long));
		p.Publish(message, message_size);
	}
	p.Poll(n);
	auto pub_end = std::chrono::high_resolution_clock::now();
	s.DEBUG_wait(total_message_size, message_size);
	auto end = std::chrono::high_resolution_clock::now();

	auto pubBandwidthMbps = (total_message_size/(1024*1024))/std::chrono::duration<double>(pub_end - start).count();
	auto e2eBandwidthMbps = (total_message_size/(1024*1024))/std::chrono::duration<double>(end - start).count();
	LOG(INFO) << "Pub Bandwidth: " << pubBandwidthMbps << " MB/s";
	LOG(INFO) << "E2E Bandwidth: " << e2eBandwidthMbps << " MB/s";
	s.DEBUG_check_order(order);
	s.StoreLatency();

	return std::make_pair(pubBandwidthMbps, e2eBandwidthMbps);
}

bool CreateNewTopic(std::unique_ptr<HeartBeat::Stub>& stub, char topic[TOPIC_NAME_SIZE], 
		int order, SequencerType seq_type, int replication_factor, bool replicate_tinode){
	grpc::ClientContext context;
	heartbeat_system::CreateTopicRequest create_topic_req;;
	heartbeat_system::CreateTopicResponse create_topic_reply;;
	create_topic_req.set_topic(topic);
	if(seq_type == SequencerType::CORFU){
		create_topic_req.set_order(0);
	}else{
		create_topic_req.set_order(order);
	}
	create_topic_req.set_replication_factor(replication_factor);
	create_topic_req.set_replicate_tinode(replicate_tinode);
	create_topic_req.set_sequencer_type(seq_type);
	grpc::Status status = stub->CreateNewTopic(&context, create_topic_req, &create_topic_reply);
	if(status.ok()){
		return create_topic_reply.success();
	}
	return false;
}

bool KillBrokers(std::unique_ptr<HeartBeat::Stub>& stub, int num_brokers){
	grpc::ClientContext context;
	heartbeat_system::KillBrokersRequest req;;
	heartbeat_system::KillBrokersResponse reply;;
	req.set_num_brokers(num_brokers);

	grpc::Status status = stub->KillBrokers(&context, req, &reply);
	if(status.ok()){
		return reply.success();
	}
	return false;
}

class ResultWriter{
	public:
		//ResultWriter(const cxxopts::ParseResult& result):result_path("../../data/"){
		ResultWriter(const cxxopts::ParseResult& result):result_path("/home/domin/Jae/Embarcadero/data/"){
			message_size = result["size"].as<size_t>();
			total_message_size = result["total_message_size"].as<size_t>();
			num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
			ack_level = result["ack_level"].as<int>();
			order = result["order_level"].as<int>();
			replication_factor =result["replication_factor"].as<int>();
			replicate_tinode = result.count("replicate_tinode");
			record_result_ = result.count("record_results");
			num_clients = result["parallel_client"].as<int>();
			num_brokers_to_kill = result["num_brokers_to_kill"].as<int>();
			failure_percentage = result["failure_percentage"].as<double>();
			seq_type = result["sequencer"].as<std::string>();
			int test_num = result["test_number"].as<int>();
			
			if(replication_factor > 0){
				result_path += "replication/";
				if(test_num == 2){
					LOG(ERROR) << "Replication and latency are separate test.";
					exit(1);
				}
			}else if(test_num != 2 && test_num != 4){
				result_path += "throughput/";
			}

			switch(test_num){
				case 0:
					result_path += "pubsub/result.csv";
					break;
				case 1:
					result_path += "e2e/result.csv";
					break;
				case 2:
					result_path += "latency/e2e/result.csv";
					break;
				case 3:
					result_path += "multiclient/result.csv";
					break;
				case 4:
					result_path += "failure/result.csv";
					break;
				case 5:
					result_path += "pub/result.csv";
					break;
				case 6:
					result_path += "sub/result.csv";
					break;
			}
		}
		~ResultWriter(){
			if(record_result_){
				std::ofstream file;
				file.open(result_path, std::ios::app);
				if(!file.is_open()){
					LOG(ERROR) << "Error: Could not open file:" << result_path << " : " << strerror(errno);
					return;
				}
				file << message_size << ",";
				file << total_message_size << ",";
				file << num_threads_per_broker << ",";
				file << ack_level << ",";
				file << order << ",";
				file << replication_factor << ",";
				file << replicate_tinode << ",";
				file << num_clients << ",";
				file << num_brokers_to_kill << ",";
				file << failure_percentage << ",";
				file << seq_type << ",";
				file << pubBandwidthMbps << ",";
				file << subBandwidthMbps << ",";
				file << e2eBandwidthMbps << "\n";

				file.close();
			}
		}

		void SetPubResult(double res){
			pubBandwidthMbps = res;
		}
		void SetSubResult(double res){
			subBandwidthMbps = res;
		}
		void SetE2EResult(double res){
			e2eBandwidthMbps = res;
		}

	private:
		size_t message_size;
		size_t total_message_size;
		size_t num_threads_per_broker;
		int ack_level;
		int order;
		int replication_factor;
		bool replicate_tinode;
		bool record_result_;
		int num_clients;
		int num_brokers_to_kill;
		double failure_percentage;
		std::string  seq_type;

		std::string result_path;
		double pubBandwidthMbps = 0;
		double subBandwidthMbps = 0;
		double e2eBandwidthMbps = 0;
};

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
		//("s,total_message_size", "Total size of messages to publish", cxxopts::value<size_t>()->default_value("536870912"))
		("m,size", "Size of a message", cxxopts::value<size_t>()->default_value("1024"))
		("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
		("r,replication_factor", "Replication factor", cxxopts::value<int>()->default_value("0"))
		("replicate_tinode", "Replicate Tinode for Disaggregated memory fault tolerance")
		("record_results", "Record Results in a csv file")
		("t,test_number", "Test to run. 0:pub/sub 1:E2E 2:Latency 3:Parallel", cxxopts::value<int>()->default_value("0"))
		("p,parallel_client", "Number of parallel clients", cxxopts::value<int>()->default_value("1"))
		("num_brokers_to_kill", "Number of brokers to kill during execution", cxxopts::value<int>()->default_value("0"))
		("failure_percentage", "When to fail brokers, after what percentages of messages sent", cxxopts::value<double>()->default_value("0"))
		("n,num_threads_per_broker", "Number of request threads_per_broker", cxxopts::value<size_t>()->default_value("4"));

	auto result = options.parse(argc, argv);
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int order = result["order_level"].as<int>();
	int replication_factor =result["replication_factor"].as<int>();
	bool replicate_tinode = result.count("replicate_tinode");
	int num_clients = result["parallel_client"].as<int>();
	int num_brokers_to_kill = result["num_brokers_to_kill"].as<int>();
	std::atomic<int> synchronizer{num_clients};
	int test_num = result["test_number"].as<int>();
	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());
	FLAGS_v = result["log_level"].as<int>();

	if(result["run_cgroup"].as<int>() > 0 && !CheckAvailableCores()){
		LOG(ERROR) << "CGroup core throttle is wrong";
		return -1;
	}
	if(order == 3){
		size_t padding = message_size % 64;
		if(padding){
			padding = 64 - padding;
		}
		size_t paddedSize = message_size + padding + sizeof(Embarcadero::MessageHeader);
		if(BATCH_SIZE % (paddedSize)){
			LOG(ERROR) << "Adjusting Batch size of message size!!";
			return 0;
		}
		/*
		 * 128  : 2^12 * (128 + 64) = 786432   total_message size = 10737942528  total_message_size : 2^22 * 20
		 * 512  : 2^10 * (512 + 64) = 589824   defulat
		 * 1024 : 2^9 * (1024 + 64) = 557056   default
		 * 4096 : 2^7 * (4096 + 64) = 532480   default
		 * 64K  : 2^3 * (2^16 + 64) = 524800	10737418240
		 * 1M   : 2^-1 * (2^20+ 64) = 1048640
		 */

		size_t n = total_message_size/message_size;
		size_t total_payload = n*paddedSize;
		padding = total_payload % (BATCH_SIZE);
		if(padding){
			padding = (num_threads_per_broker*BATCH_SIZE) - padding;
			LOG(INFO) << "Adjusting total message size from " << total_message_size << " to " << total_message_size+padding << 
				" :" <<(total_message_size+padding)%(num_threads_per_broker*BATCH_SIZE); 
			total_message_size += padding;
		}
	}

	std::unique_ptr<HeartBeat::Stub> stub = HeartBeat::NewStub(grpc::CreateChannel("127.0.0.1:"+std::to_string(BROKER_PORT), grpc::InsecureChannelCredentials()));
	char topic[TOPIC_NAME_SIZE];
	memset(topic, 0, TOPIC_NAME_SIZE);
	memcpy(topic, "TestTopic", 9);

	ResultWriter writer(result);

	switch(test_num){
		case 0:
			{
				CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode);
				LOG(INFO) << "Running Publish and Subscribe: "<< total_message_size;
				double pub_bandwidthMb = PublishThroughputTest(result, topic, synchronizer);
				sleep(3);
				double sub_bandwidthMb = SubscribeThroughputTest(result, topic);
				writer.SetPubResult(pub_bandwidthMb);
				writer.SetSubResult(sub_bandwidthMb);
			}
			break;
		case 1:
			{
				LOG(INFO) << "Running E2E Throughput";
				CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode);
				std::pair<double, double> bandwidths = E2EThroughputTest(result, topic);
				writer.SetPubResult(bandwidths.first);
				writer.SetE2EResult(bandwidths.second);
			}
			break;
		case 2:
			{
				LOG(INFO) << "Running E2E Latency Test";
				CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode);
				std::pair<double, double> bandwidths = LatencyTest(result, topic);
				writer.SetPubResult(bandwidths.first);
				writer.SetSubResult(bandwidths.second);
			}
			break;
		case 3:
			LOG(INFO) << "Running Parallel Publish Test num_clients:" << num_clients << ":" << num_threads_per_broker;
			{
				CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode);
				std::vector<std::thread> threads;
				std::vector<std::promise<double>> promises(num_clients); // Vector of promises
				std::vector<std::future<double>> futures;                // Vector of futures

				// Prepare the futures
				for (int i = 0; i < num_clients; ++i) {
					futures.push_back(promises[i].get_future());  // Get future from each promise
				}

				// Launch the threads
				for (int i = 0; i < num_clients; i++) {
					threads.emplace_back([&result,&topic,&synchronizer,&promises, i]() {
							double res = PublishThroughputTest(result, topic, synchronizer);
							promises[i].set_value(res); // Set the result in the promise
							});
				}

				double aggregate_bandwidth = 0;

				// Wait for the threads to finish and collect the results
				for (int i = 0; i < num_clients; ++i) {
					if (threads[i].joinable()) {
						threads[i].join();  // Wait for the thread to finish
						aggregate_bandwidth += futures[i].get();  // Get the result from the future
					}
				}
				writer.SetPubResult(aggregate_bandwidth);

				std::cout << "Aggregate Bandwidth:" << aggregate_bandwidth;
			}
			break;
		case 4:
			LOG(INFO) << "Running Broker failure at publish ";
			{
				if(num_brokers_to_kill == 0){
					LOG(WARNING) << "Number of broker fail in FailureTest is 0, are you sure about it?";
				}

				auto killbrokers = [&stub, num_brokers_to_kill](){
					return KillBrokers(stub, num_brokers_to_kill);
				};
				CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode);
				double pub_bandwidthMb = FailurePublishThroughputTest(result, topic, killbrokers);
				writer.SetPubResult(pub_bandwidthMb);
			}
			break;
		case 5:
			LOG(INFO) << "Running Publish : "<< total_message_size;
			{
				CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode);
				double pub_bandwidthMb = PublishThroughputTest(result, topic, synchronizer);
				writer.SetPubResult(pub_bandwidthMb);
			}
			break;
		case 6:
			LOG(INFO) << "Running Subscribe ";
			{
				double sub_bandwidthMb = SubscribeThroughputTest(result, topic);
				writer.SetSubResult(sub_bandwidthMb);
			}
			break;
		default:
			LOG(ERROR) << "Invalid test number option:" << result["test_number"].as<int>();
			break;
	}

	writer.~ResultWriter();
	//*****************  Shuting down Embarlet ************************
	google::protobuf::Empty request, response;
	grpc::ClientContext context;
	LOG(INFO) << "Calling TerminateCluster";
	stub->TerminateCluster(&context, request, &response);

	return 0;
}
