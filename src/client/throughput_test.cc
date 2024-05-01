#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>
#include <mimalloc.h>

#include "common/config.h"
#include "../cxl_manager/cxl_manager.h"

#define ACK_SIZE 1024
#define SERVER_ADDR "127.0.0.1"
#define CLIENT_ID 1

std::atomic<size_t> totalBytesRead_(0);
std::atomic<size_t> client_order_(0);

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

void send_data(size_t message_size, size_t total_message_size, int ack_level) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("Socket creation failed");
		return;
	}

	make_socket_non_blocking(sock);

	// Set the SO_REUSEADDR option
	int flag = 1; // Enable the option
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
			perror("setsockopt(SO_REUSEADDR) failed");
			close(sock);
			return ;
	}

	if(setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)) != 0){
		perror("setsockopt error");
		close(sock);
		return;
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
			return;
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


	Embarcadero::EmbarcaderoReq req;
	req.client_id = CLIENT_ID;
	req.client_order = 0;
	memset(req.topic, 0, 32);
	req.topic[0] = '0';
	req.ack = ack_level;
	req.size = message_size + sizeof(Embarcadero::MessageHeader);
	int n, i;
	struct epoll_event events[10]; // Adjust size as needed
	bool running = true;
	size_t sent_bytes = 0;
	VLOG(3) << "Start publishing  on fd" << sock;
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
	while (running) {
		for (; i < n; i++) {
			if (events[i].events & EPOLLOUT && (!stop_sending || header->client_order < run_count)) {
				if(!stop_sending && header->client_order >= run_count){
					stop_sending = true;
					header->client_id = -1;
				}
				ssize_t bytesSent = send(sock, (uint8_t*)data + sent_bytes, req.size - sent_bytes, 0);
				if (bytesSent < 0) {
					if (errno != EAGAIN) {
						perror("send failed");
						running = false;
						break;
					}
				} 
				sent_bytes += bytesSent;
				if(sent_bytes == req.size){
					sent_bytes = 0;
					header->client_order = client_order_.fetch_add(1);
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
	close(sock);
	close(efd);
	free(data);
}

void read_ack(size_t TOTAL_DATA_SIZE, size_t message_size){
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
		std::chrono::time_point<std::chrono::high_resolution_clock> DEBUG_end_time;
    if (server_sock < 0) {
        perror("Socket creation failed");
        return ;
    }

    int flag = 1;
		if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
				perror("setsockopt(SO_REUSEADDR) failed");
				close(server_sock);
				return ;
		}
    setsockopt(server_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT + CLIENT_ID);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        return ;
    }

    if (listen(server_sock, SOMAXCONN) < 0) {
        perror("Listen failed");
        close(server_sock);
        return ;
    }

		sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
		if (client_sock < 0) {
			perror("Accept failed");
			return;
		}
		/*
    int efd = epoll_create1(0);
    struct epoll_event event;
    event.data.fd = client_sock;
    event.events = EPOLLIN ; // Edge-triggered for both read and write
    epoll_ctl(efd, EPOLL_CTL_ADD, client_sock, &event);

    struct epoll_event events[10]; // Adjust size as needed
		*/

    char *data = (char*)calloc(TOTAL_DATA_SIZE/message_size, sizeof(char));
    ssize_t bytesReceived;
		int to_read = TOTAL_DATA_SIZE/message_size;//sizeof(std::chrono::time_point<std::chrono::high_resolution_clock>);
		VLOG(3) << "Start reading ack: " << to_read << " on fd" << client_sock;
    while (to_read > 0){
		/*
			int n = epoll_wait(efd, events, 10, -1);
			for (int i = 0; i < n; i++) {
				if(events[i].events & EPOLLIN){
				*/
						VLOG(4) << "Before reading" ;
					if(bytesReceived = recv(client_sock, (uint8_t*)data + (TOTAL_DATA_SIZE/message_size - to_read) , 1024, 0)){
					//if(bytesReceived = recv(client_sock, &DEBUG_end_time, sizeof(DEBUG_end_time), 0)){
						to_read -= bytesReceived;
						VLOG(4) << "Ack received:" << bytesReceived;
					}else{
						perror("Read error");
					}
				//}
		//	}
    }
		free(data);
    close(client_sock);
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
        ("m,size", "Size of a message", cxxopts::value<size_t>()->default_value("960"))
        ("t,num_thread", "Number of request threads", cxxopts::value<size_t>()->default_value("32"));

	auto result = options.parse(argc, argv);
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads = result["num_thread"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	FLAGS_v = result["log_level"].as<int>();

	LOG(INFO) << "Starting Throughput Test with " << num_threads << " threads, total message size:" << total_message_size;

	auto start = std::chrono::high_resolution_clock::now();
	std::vector<std::thread> threads;
	if(ack_level > 0){
		threads.emplace_back(read_ack, total_message_size, message_size);
	}
	for (size_t i = 0; i < num_threads; ++i) {
		threads.emplace_back(send_data, message_size, total_message_size, ack_level);
	}

	for (auto &t : threads) {
		t.join();
	}
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = end - start;
	double seconds = elapsed.count();

	// Calculate bandwidth
	double bandwidthMbps = ((client_order_ * message_size) / seconds) / (1024 * 1024);  // Convert to Megabytes per second

	LOG(INFO) << "Bandwidth:" << bandwidthMbps << " MBps" ;

	return 0;
}
