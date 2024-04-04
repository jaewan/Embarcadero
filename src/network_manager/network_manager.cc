#include "network_manager.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace Embarcadero{

NetworkManager::NetworkManager(size_t queueCapacity, int num_io_threads):
						 requestQueue_(queueCapacity),
						 ackQueue_(50),
						 num_io_threads_(num_io_threads){
	// Create Network I/O threads
	threads_.emplace_back(&NetworkManager::MainThread, this);
	threads_.emplace_back(&NetworkManager::AckThread, this);
	threads_.emplace_back(&NetworkManager::AckThread, this);
	for (int i=3; i< num_io_threads_; i++)
		threads_.emplace_back(&NetworkManager::Network_io_thread, this);

	for (int i=0; i< NUM_BUFFERS; i++){
		buffers_[i] = (char*)malloc(BUFFER_SIZE);
		buffers_counters_[i] = 0;
	}


	while(thread_count_.load() != num_io_threads_){}
	std::cout << "[NetworkManager]: \tCreated" << std::endl;
}

NetworkManager::~NetworkManager(){
	stop_threads_ = true;
	std::optional<struct NetworkRequest> sentinel = std::nullopt;
	ackQueue_.blockingWrite(sentinel);
	for (int i=3; i<num_io_threads_; i++)
		requestQueue_.blockingWrite(sentinel);
	for (int i=0; i< NUM_BUFFERS; i++){
		free(buffers_[i]);
	}

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

#define READ_SIZE 1024
#define MSG_SIZE 1000000
struct EmbarcaderoReq{
	size_t client_id;
	size_t client_order;
	char topic[32];
	size_t ack;
	size_t size;
};
char JaeDebugBuf[1024];
std::chrono::high_resolution_clock::time_point start;
void NetworkManager::Network_io_thread(){
	start = std::chrono::high_resolution_clock::now();

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
				while(true){
					int buffer_off = GetBuffer();
					char* buf = buffers_[buffer_off];
					int bytes_read = read(req.client_socket, buf, READ_SIZE);
					if(bytes_read <= 0)
						break;
					struct EmbarcaderoReq *clientReq = (struct EmbarcaderoReq*)buf;
					// Create publish request
					struct PublishRequest pub_req;
					pub_req.client_id = clientReq->client_id;
					pub_req.client_order = clientReq->client_order;
					memcpy(pub_req.topic, clientReq->topic, 32);
					pub_req.acknowledge = clientReq->ack;
					pub_req.payload_address = (uint8_t*)buf + sizeof(EmbarcaderoReq);
					pub_req.counter = &buffers_counters_[buffer_off];
					pub_req.size = clientReq->size;

					cxl_manager_->EnqueueRequest(pub_req);
					disk_manager_->EnqueueRequest(pub_req);
				}
				close(req.client_socket);
				break;
			case Send:
				std::cout << "Send" << std::endl;
				break;
			case Test:
				{
				int buffer_off = GetBuffer();
				char* buf = buffers_[buffer_off];
				memcpy(buf, JaeDebugBuf, 1024);
				// Create publish request
				struct PublishRequest pub_req;
				pub_req.client_id = 0;
				pub_req.client_order = 0;
				pub_req.topic[0] = '0';
				pub_req.acknowledge = true;
				pub_req.payload_address = (void*)buf; 
				pub_req.counter = &buffers_counters_[buffer_off];
				pub_req.size = 1024;

				cxl_manager_->EnqueueRequest(pub_req);
				disk_manager_->EnqueueRequest(pub_req);
				}
				break;
		}
	}
}

void NetworkManager::AckThread(){
	std::optional<struct NetworkRequest> optReq;
	thread_count_.fetch_add(1, std::memory_order_relaxed);
	static std::atomic<int> JaeDebugCount{0};
	static std::atomic<int> JaeDebugDiskCount{0};

	while(!stop_threads_){
		ackQueue_.blockingRead(optReq);
		if(!optReq.has_value()){
			break;
		}
		const struct NetworkRequest &req = optReq.value();
		if(req.client_socket == -1){
			JaeDebugDiskCount.fetch_add(1, std::memory_order_relaxed);
		}
		JaeDebugCount.fetch_add(1, std::memory_order_relaxed);
		if(JaeDebugCount == MSG_SIZE){
			auto end = std::chrono::high_resolution_clock::now();
			auto dur = end - start;
			std::cout<<std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() << std::endl;
			std::cout<< "Disk ack:" << JaeDebugDiskCount << std::endl;
		}
	}
}

int NetworkManager::GetBuffer(){
	static std::atomic<int> counter{0};
	int off = counter.fetch_add(1, std::memory_order_relaxed) % NUM_BUFFERS;
	int zero = 0;
	while(1){
		if(buffers_counters_[off].compare_exchange_weak(zero, 2)){
			return off;
		}
		off = (off+1) % NUM_BUFFERS;
	}
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
