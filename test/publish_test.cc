#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <errno.h>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#define NUM_PUB 1000
const int numThreads = 64;
/*
#define NUM_PUB 1
const int numThreads = 1;
*/

std::atomic<int> order{0};
const int PORT = 1214;
const std::string SERVER_IP = "192.168.60.171";
const size_t MSG_SIZE = 5024;

struct alignas(64) EmbarcaderoReq{
	size_t client_id;
	size_t client_order;
	size_t ack;
	size_t size;
	char topic[32];
};

struct alignas(64) MessageHeader{
	int client_id;
	size_t client_order;
	volatile size_t size;
	volatile size_t total_order;
	volatile size_t paddedSize;
	void* segment_header;
	size_t logical_offset;
	void* next_message;
};

int connectToEmbarcadero(){
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP.c_str(), &server_addr.sin_addr);

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "Failed to connect to server" << std::endl;
        close(sock);
        return -1;
    }
	return sock;
}

void sendMessage() {
	// Initiate the connection
	int sock = connectToEmbarcadero();
	struct EmbarcaderoReq req;
	req.client_id = 0;
	req.client_order = 0;
	memset(req.topic, 0, 32);
	req.topic[0] = '0';
	req.ack = 1;
	req.size = MSG_SIZE + 64;
	int ret = send(sock, &req, sizeof(req), 0);
	if(ret <=0 ){
		std::cout<< "Req failed:" << strerror(errno) << std::endl;
		return;
	}

	// Send messages
	char buf[MSG_SIZE + 64];
	MessageHeader *header =  (MessageHeader*)buf;
	header->client_id = 0;
	header->size = MSG_SIZE;
	int padding = MSG_SIZE % 64;
	if(padding){
		padding = 64 - padding;
	}
	header->paddedSize = MSG_SIZE + padding + sizeof(MessageHeader);
	header->segment_header = nullptr;
	header->logical_offset = (size_t)-1; // Sentinel value
	header->next_message = nullptr;

	for(int i = 0; i<NUM_PUB; i++){
		header->client_order = order.fetch_add(1);
		ret = send(sock, buf, header->paddedSize, 0);
		if(ret <=0 ){
			std::cout<< strerror(errno) << std::endl;
			return;
			break;
		}
	}
	// Finish sending
	header->client_id = -1;
	ret = send(sock, buf, sizeof(MessageHeader), 0);
	if(ret <=0 ){
		std::cout<< "End Message Error: " <<strerror(errno) << std::endl;
		return;
	}

	
	ret = 0;
	while(ret < NUM_PUB){
		ret += read(sock, buf, 1024);
		//std::cout<< "[DEBUG] acked: " <<ret << std::endl;
		if(ret <=0 ){
			std::cout<< strerror(errno) << std::endl;
			return;
			break;
		}
	}
	// Receive Ack
    close(sock);
}

void throughputTest(){
    std::vector<std::thread> threads;
	auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(sendMessage);
    }

	int joined =0;
    for (auto& thread : threads) {
        thread.join();
		joined++;
    }
	auto end = std::chrono::high_resolution_clock::now();
	auto dur = end - start;
	double runtime = std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
	std::cout<<"Runtime:" << runtime << std::endl;
	std::cout<<(double)(MSG_SIZE*numThreads*((double)NUM_PUB/(double)1024))/runtime << "GB/s" << std::endl;
}

void sequenceTest(){
	int sock = connectToEmbarcadero();
	char buf[MSG_SIZE];
	struct EmbarcaderoReq *req = (struct EmbarcaderoReq*)buf;
	req->client_id = 0;
	req->client_order = 0;
	memset(req->topic, 0, 32);
	req->topic[0] = '0';
	req->ack = 1;
	req->size = MSG_SIZE - sizeof(EmbarcaderoReq);
	int ret = send(sock, buf, MSG_SIZE, 0);
	std::cout<< "Client:" << req->client_id<< "Order:" << req->client_order << std::endl;
	if(ret <=0 )
		std::cout<< strerror(errno) << std::endl;
	req->client_id = 1;
	req->client_order = 0;
	std::cout<< "Client:" << req->client_id<< "Order:" << req->client_order << std::endl;
	ret = send(sock, buf, MSG_SIZE, 0);
	if(ret <=0 )
		std::cout<< strerror(errno) << std::endl;
	req->client_id = 0;
	req->client_order = 3;
	std::cout<< "Client:" << req->client_id<< "Order:" << req->client_order << std::endl;
	ret = send(sock, buf, MSG_SIZE, 0);
	if(ret <=0 )
		std::cout<< strerror(errno) << std::endl;
	req->client_id = 0;
	req->client_order = 2;
	std::cout<< "Client:" << req->client_id<< "Order:" << req->client_order << std::endl;
	ret = send(sock, buf, MSG_SIZE, 0);
	if(ret <=0 )
		std::cout<< strerror(errno) << std::endl;
	req->client_id = 0;
	req->client_order = 1;
	std::cout<< "Client:" << req->client_id<< "Order:" << req->client_order << std::endl;
	ret = send(sock, buf, MSG_SIZE, 0);
	if(ret <=0 )
		std::cout<< strerror(errno) << std::endl;

}

int main() {
	throughputTest();

    return 0;
}
