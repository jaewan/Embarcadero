#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <numeric>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>

const int PORT = 8080;
const int NUM_BROKERS = 20;
const int TEST_DURATION_SECONDS = 10;
const int BUFFER_SIZE = 65536; // 64KB buffer

std::atomic<long> total_bytes_sent_all_threads(0);

void connect_and_send(const std::string& broker_ip) {
    int sock = 0;
    struct sockaddr_in serv_addr;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Socket creation error for " << broker_ip << std::endl;
        return;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, broker_ip.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address/ Address not supported for " << broker_ip << std::endl;
        close(sock);
        return;
    }

    // Retry connecting for a few seconds
    int connection_attempts = 5;
    while (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        connection_attempts--;
        if(connection_attempts == 0) {
            std::cerr << "Connection Failed to " << broker_ip << ". Giving up." << std::endl;
            close(sock);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::cout << "Connected to " << broker_ip << std::endl;

    std::vector<char> data_buffer(BUFFER_SIZE, 'a');
    
    auto start_time = std::chrono::steady_clock::now();
    long thread_local_bytes_sent = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (elapsed >= TEST_DURATION_SECONDS) {
            break;
        }

        int bytes_sent = send(sock, data_buffer.data(), data_buffer.size(), 0);
        if (bytes_sent < 0) {
            perror("send failed");
            break;
        }
        thread_local_bytes_sent += bytes_sent;
    }

    total_bytes_sent_all_threads += thread_local_bytes_sent;
    shutdown(sock, SHUT_WR); // Signal server we are done sending
    close(sock);
    
    double speed_gbps = (thread_local_bytes_sent * 8.0) / (TEST_DURATION_SECONDS * 1e9);
    std::cout << "Finished sending to " << broker_ip << ". Sent " << thread_local_bytes_sent 
              << " bytes. Throughput: " << speed_gbps << " Gbps." << std::endl;
}

int main() {
    std::vector<std::thread> threads;
    std::vector<std::string> broker_ips;

    for (int i = 1; i <= NUM_BROKERS; ++i) {
        broker_ips.push_back("10.0.0." + std::to_string(i));
    }

    auto benchmark_start_time = std::chrono::high_resolution_clock::now();

    for (const auto& ip : broker_ips) {
        threads.emplace_back(connect_and_send, ip);
    }

    for (auto& th : threads) {
        th.join();
    }

    auto benchmark_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = benchmark_end_time - benchmark_start_time;

    double total_gigabits = (total_bytes_sent_all_threads * 8.0) / 1e9;
    double aggregate_throughput_gbps = total_gigabits / elapsed.count();
    
    std::cout << "\n-----------------------------------------------------" << std::endl;
    std::cout << "Benchmark Complete" << std::endl;
    std::cout << "Total duration: " << elapsed.count() << " seconds" << std::endl;
    std::cout << "Total data sent: " << total_bytes_sent_all_threads << " bytes" << std::endl;
    std::cout << "Aggregate throughput: " << aggregate_throughput_gbps << " Gbps" << std::endl;
    std::cout << "-----------------------------------------------------" << std::endl;


    return 0;
}
