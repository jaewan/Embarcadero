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

//#define ACK 1

// Constants: You should define these appropriately
constexpr int PORT = 1214;
constexpr char SERVER_ADDR[] = "127.0.0.1";
constexpr int DATA_SIZE = 1024; // Size of the data packet to send
constexpr int ACK_SIZE = 1024;  // Expected size of acknowledgment packet
constexpr int NUM_THREADS = 1;

std::atomic<ssize_t> totalBytesSent(0);
std::atomic<ssize_t> totalBytesRead(0);

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

void send_data(int tid) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return;
    }

    make_socket_non_blocking(sock);

    int flag = 1;
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
#else
    event.events = EPOLLOUT ; 
#endif
    epoll_ctl(efd, EPOLL_CTL_ADD, sock, &event);

    char data[DATA_SIZE];
    memset(data, 'A', DATA_SIZE);
    char ack[ACK_SIZE];

    struct epoll_event events[10]; // Adjust size as needed
    bool running = true;
    while (running) {
        int n = epoll_wait(efd, events, 10, -1);
        for (int i = 0; i < n; i++) {
            if (events[i].events & EPOLLOUT && totalBytesSent < 1000000000) {
                ssize_t bytesSent = send(sock, data, DATA_SIZE, 0);
                if (bytesSent < 0) {
                    if (errno != EAGAIN) {
                        perror("send failed");
                        running = false;
                        break;
                    }
                } else {
                    totalBytesSent += bytesSent;
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
									totalBytesRead.fetch_add(bytesReceived);
                }
            }
#endif
        }
        if (totalBytesSent >= 1000000000) { // Example break condition
            running = false;
        }
    }

    close(sock);
    close(efd);
}

int main() {
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(send_data, i);
    }

    for (auto &t : threads) {
        t.join();
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    double seconds = elapsed.count();

    // Calculate bandwidth
    double bandwidthMbps = (totalBytesSent / seconds) / (1024 * 1024);  // Convert to Megabytes per second

    std::cout << "Acked: " << totalBytesRead <<  " sent:" << (double)totalBytesSent/1024  << std::endl;
    std::cout << "Total data sent: " << totalBytesSent << " bytes" << std::endl;
    std::cout << "Time taken: " << seconds << " seconds" << std::endl;
    std::cout << "Bandwidth: " << bandwidthMbps << " MBps" << std::endl;

    return 0;
}
