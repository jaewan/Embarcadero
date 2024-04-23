#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

const size_t BUFFER_SIZE = 1024 * 1024 * 64; // 64 MB buffer
const size_t NUM_THREADS = 8; // number of threads

void measure_bandwidth(char* buffer, size_t size) {
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < size; i += sizeof(char*)) {
        *(char**)&buffer[i] = &buffer[0];
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double bandwidth = (double)size / duration * 1000000.0 / (1024.0 * 1024.0); // MB/s
    std::cout << "Bandwidth: " << bandwidth << " MB/s" << std::endl;
}

int main() {
    // Allocate memory using mmap
	int fd = open("/dev/dax0.0", O_RDWR);
	if (fd == -1) {
		perror("open() failed");
		return 1;
	}

    char* buffer = (char*)mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    //char* buffer = (char*)mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buffer == MAP_FAILED) {
        std::cerr << "Failed to allocate memory using mmap" << std::endl;
        return 1;
    }

    // Measure bandwidth using multiple threads
    std::vector<std::thread> threads;
    for (size_t i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back([&, i]() {
            measure_bandwidth(buffer + (i * (BUFFER_SIZE / NUM_THREADS)), BUFFER_SIZE / NUM_THREADS);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Free the memory
    munmap(buffer, BUFFER_SIZE);

    return 0;
}
