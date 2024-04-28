#include <iostream>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>
#include <errno.h>

#define PORT 1214
#define DATA_SIZE 1024
#define BACKLOG_SIZE 64
#define NUM_THREADS 1 

//#define EPOLL 1
#define MAX_EVENTS 10

std::atomic<bool> running(true);
std::atomic<ssize_t> totalBytesReceived(0);

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

void handle_client(int efd, int client_sock) {
	std::cout << "[DEBUG] handle_client called" << std::endl;
#ifdef EPOLL
	make_socket_non_blocking(client_sock);
	struct epoll_event event;
	event.data.fd =client_sock;
	event.events = EPOLLIN ; // Edge-triggered for both read and write
	epoll_ctl(efd, EPOLL_CTL_ADD, client_sock, &event);

	struct epoll_event events[MAX_EVENTS]; // Adjust size as needed
	std::cout << "[DEBUG] Created epoll and made socket:" << client_sock << " non blocking" << std::endl;
#endif

	char data[DATA_SIZE];
	ssize_t bytesReceived;
	while (running){
#ifdef EPOLL
		int n = epoll_wait(efd, events, MAX_EVENTS, -1);
		for (int i = 0; i < n; i++) {
			if(events[i].events & EPOLLIN && events[i].data.fd == client_sock){
#endif
				bytesReceived = recv(client_sock, data, DATA_SIZE, 0);
				if(bytesReceived > 0)
					totalBytesReceived += bytesReceived;
				else{
					break;
					std::cout << "!!!!!! [DEBUG] read error:" << strerror(errno) << std::endl;
				}
				//std::cout << "[DEBUG] read:" << bytesReceived << std::endl;
#ifdef EPOLL
			}
		}
#endif
	}
	close(client_sock);
}

void accept_clients(int server_sock) {
	while (running) {
		sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
		if (client_sock < 0) {
			if (running) {
				perror("Accept failed");
			}
			continue;
		}

		//std::thread client_thread(handle_client, client_sock);
		//client_thread.detach();
	}
}

int main() {
	int server_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (server_sock < 0) {
		perror("Socket creation failed");
		return 1;
	}

	int flag = 1;
	setsockopt(server_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

	sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
		perror("Bind failed");
		close(server_sock);
		return 1;
	}

	if (listen(server_sock, BACKLOG_SIZE) < 0) {
		perror("Listen failed");
		close(server_sock);
		return 1;
	}
	std::vector<std::thread> threads;
	// Add server socket to epoll
	int efd = epoll_create1(0);
	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = server_sock;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, server_sock, &event) == -1) perror("epoll_ctl");
	struct epoll_event events[MAX_EVENTS];
	while(true){
		int n = epoll_wait(efd, events, MAX_EVENTS, -1);
		for(int i=0; i<n; i++){
			struct sockaddr_in client_addr;
			socklen_t client_addr_len = sizeof(client_addr);
			int client_socket = accept(server_sock, (struct sockaddr*)&client_addr, &client_addr_len);
			threads.emplace_back(handle_client, efd, client_socket);
		}
	}

	/*
	for (int i = 0; i < NUM_THREADS; ++i) {
		threads.emplace_back(accept_clients, server_sock);
	}
	*/

	std::cout << "Press ENTER to stop the server..." << std::endl;
	std::cin.get();

	running = false;

	for (auto &t : threads) {
		t.join();
	}

	close(server_sock);

	std::cout << "Total data received: " << totalBytesReceived << " bytes" << std::endl;

	return 0;
}
