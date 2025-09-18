#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <atomic>
#include <csignal>

const int PORT = 8080;
const int BUFFER_SIZE = 65536; // 64KB buffer

std::atomic<bool> keep_running(true);

void signal_handler(int signum) {
    std::cerr << "Signal (" << signum << ") received, shutting down." << std::endl;
    keep_running = false;
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    long total_bytes_received = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    while (keep_running) {
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE, 0);
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                std::cout << "Client disconnected." << std::endl;
            } else {
                perror("recv failed");
            }
            break;
        }
        total_bytes_received += bytes_received;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    if (elapsed.count() > 0) {
        double speed_mbps = (total_bytes_received * 8.0) / (elapsed.count() * 1024 * 1024);
        std::cout << "Received " << total_bytes_received << " bytes in " << elapsed.count() << " seconds. "
                  << "Average speed: " << speed_mbps << " Mbps." << std::endl;
    }

    close(client_socket);
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    std::cout << "Broker is listening on port " << PORT << std::endl;

    while (keep_running) {
        int new_socket = accept(server_fd, nullptr, nullptr);
        if (new_socket < 0) {
            if (keep_running) perror("accept");
            break;
        }
        
        std::cout << "New connection accepted. Handling client." << std::endl;
        // For this simple test, we handle one client and then exit.
        // For a real broker, you'd likely use a thread.
        handle_client(new_socket);
        break; // Exit after one connection for this benchmark.
    }

    close(server_fd);
    std::cout << "Broker shutting down." << std::endl;
    return 0;
}
