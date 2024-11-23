#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <filesystem>
#include <iostream>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <numeric>

#define SIZE (1UL<<35)

enum CXL_Type {Emul, Real};
static inline void* allocate_shm(int broker_id, CXL_Type cxl_type, size_t cxl_size){
	void *addr = nullptr;
	int cxl_fd;
	bool dev = false;
	if(cxl_type == Real){
		if(std::filesystem::exists("/dev/dax0.0")){
			dev = true;
			cxl_fd = open("/dev/dax0.0", O_RDWR);
		}else{
			if(numa_available() == -1){
				std::cout << "Cannot allocate from real CXL";
				return nullptr;
			}else{
				cxl_fd = shm_open("/CXL_SHARED_FILE", O_CREAT | O_RDWR, 0666);
			}
		}
	}else{
		cxl_fd = shm_open("/CXL_SHARED_FILE", O_CREAT | O_RDWR, 0666);
	}

	if (cxl_fd < 0){
		std::cout<<"Opening CXL error";
		return nullptr;
	}
	if(broker_id == 0 && !dev){
		if (ftruncate(cxl_fd, cxl_size) == -1) {
			std::cout << "ftruncate failed";
			close(cxl_fd);
			return nullptr;
		}
	}
	addr = mmap(NULL, cxl_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, cxl_fd, 0);
	close(cxl_fd);
	if(addr == MAP_FAILED){
		std::cout << "Mapping CXL failed";
		return nullptr;
	}

	if(cxl_type == Real && !dev && broker_id == 0){
		// Create a bitmask for the NUMA node (numa node 2 should be the CXL memory)
		struct bitmask* bitmask = numa_allocate_nodemask();
		numa_bitmask_setbit(bitmask, 2);

		// Bind the memory to the specified NUMA node
		if (mbind(addr, cxl_size, MPOL_BIND, bitmask->maskp, bitmask->size, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
			std::cout<< "mbind failed";
			numa_free_nodemask(bitmask);
			munmap(addr, cxl_size);
			return nullptr;
		}

		numa_free_nodemask(bitmask);
	}

	return addr;
}

// Function to perform sequential write
void sequentialWrite(char* addr, size_t size) {
	for (size_t i = 0; i < size; ++i) {
		addr[i] = 'A';
	}
}

// Function to perform sequential read
void sequentialRead(char* addr, size_t size) {
	volatile char temp;
	for (size_t i = 0; i < size; ++i) {
		temp = addr[i];
	}
}

int main(){
	void* mmaped_region = allocate_shm(0, Real, SIZE);
	char* char_addr = static_cast<char*>(mmaped_region);

	//unsigned int num_threads = std::thread::hardware_concurrency(); // Use available cores
	unsigned int num_threads = 32;
	size_t chunk_size = SIZE / num_threads; // Divide the memory region into chunks for each thread
	size_t remainder = SIZE % num_threads; // Handle any remaining memory

	// --- 1. Maximum Write Bandwidth (Parallel Write) ---
	std::vector<std::thread> threads;
	auto start_write = std::chrono::high_resolution_clock::now();

	// Launch threads to perform parallel writes
	for (unsigned int i = 0; i < num_threads; ++i) {
		size_t current_chunk_size = (i == num_threads - 1) ? chunk_size + remainder : chunk_size;
		threads.emplace_back(sequentialWrite, char_addr + i * chunk_size, current_chunk_size);
	}

	// Wait for all threads to finish
	for (auto& thread : threads) {
		thread.join();
	}

	auto end_write = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> write_duration = end_write - start_write;
	double write_bandwidth = (double)SIZE / (1024 * 1024 * 1024) / write_duration.count();
	std::cout << "Maximum Parallel Write Bandwidth: " << write_bandwidth << " GB/s" << std::endl;

	// --- 2. Maximum Read Bandwidth (Parallel Read) ---
	threads.clear(); // Clear the thread vector for reuse
	auto start_read = std::chrono::high_resolution_clock::now();

	// Launch threads to perform parallel reads
	for (unsigned int i = 0; i < num_threads; ++i) {
		size_t current_chunk_size = (i == num_threads - 1) ? chunk_size + remainder : chunk_size;
		threads.emplace_back(sequentialRead, char_addr + i * chunk_size, current_chunk_size);
	}

	// Wait for all threads to finish
	for (auto& thread : threads) {
		thread.join();
	}

	auto end_read = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> read_duration = end_read - start_read;
	double read_bandwidth = (double)SIZE / (1024 * 1024 * 1024) / read_duration.count();
	std::cout << "Maximum Parallel Read Bandwidth: " << read_bandwidth << " GB/s" << std::endl;

	// --- 3. Parallel Read and Write Bandwidth ---
	// Use the same number of threads for fair comparison: num_threads for read-only + num_threads for write-only
	unsigned int rw_num_threads = 2 * num_threads; // Total threads for parallel read/write
	size_t rw_chunk_size = SIZE / rw_num_threads; // Each thread will handle a smaller chunk of the total memory
	size_t rw_remainder = SIZE % rw_num_threads;  // Handle any leftover bytes for the last thread
	threads.clear(); // Clear the thread vector for reuse

	auto start_parallel_rw = std::chrono::high_resolution_clock::now();

	// Launch half of the threads for reads and the other half for writes
	for (unsigned int i = 0; i < rw_num_threads; ++i) {
		size_t current_chunk_size = (i == rw_num_threads - 1) ? rw_chunk_size + rw_remainder : rw_chunk_size;

		if (i % 2 == 0) {
			// Even-indexed threads perform writes
			threads.emplace_back(sequentialWrite, char_addr + i * rw_chunk_size, current_chunk_size);
		} else {
			// Odd-indexed threads perform reads
			threads.emplace_back(sequentialRead, char_addr + i * rw_chunk_size, current_chunk_size);
		}
	}

	// Wait for all threads to finish
	for (auto& thread : threads) {
		thread.join();
	}

	auto end_parallel_rw = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> parallel_rw_duration = end_parallel_rw - start_parallel_rw;

	// Calculate separate read and write bandwidths
	double parallel_write_bandwidth = (double)(SIZE / 2) / (1024 * 1024 * 1024) / parallel_rw_duration.count(); // Only half the memory is written
	double parallel_read_bandwidth = (double)(SIZE / 2) / (1024 * 1024 * 1024) / parallel_rw_duration.count();  // Only half the memory is read

	std::cout << "Parallel Write Bandwidth (during read/write): " << parallel_write_bandwidth << " GB/s" << std::endl;
	std::cout << "Parallel Read Bandwidth (during read/write): " << parallel_read_bandwidth << " GB/s" << std::endl;

	// --- Cleanup ---
	munmap(mmaped_region, SIZE);

	return 0;
}
