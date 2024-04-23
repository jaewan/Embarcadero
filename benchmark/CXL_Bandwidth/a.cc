#include <iostream>
#include <cstring>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <emmintrin.h>

#include <thread>
#include <vector>

void memcpy_nt(void* dst, const void* src, size_t size) {
    // Cast the input pointers to the appropriate types
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // Align the destination pointer to 16-byte boundary
    size_t alignment = reinterpret_cast<uintptr_t>(d) & 0xF;
    if (alignment) {
        alignment = 16 - alignment;
        size_t copy_size = (alignment > size) ? size : alignment;
        std::memcpy(d, s, copy_size);
        d += copy_size;
        s += copy_size;
        size -= copy_size;
    }

    // Copy the bulk of the data using non-temporal stores
    size_t block_size = size / 64;
    for (size_t i = 0; i < block_size; ++i) {
        _mm_stream_si64(reinterpret_cast<long long*>(d), *reinterpret_cast<const long long*>(s));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 8), *reinterpret_cast<const long long*>(s + 8));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 16), *reinterpret_cast<const long long*>(s + 16));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 24), *reinterpret_cast<const long long*>(s + 24));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 32), *reinterpret_cast<const long long*>(s + 32));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 40), *reinterpret_cast<const long long*>(s + 40));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 48), *reinterpret_cast<const long long*>(s + 48));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 56), *reinterpret_cast<const long long*>(s + 56));
        d += 64;
        s += 64;
    }

    // Copy the remaining data using standard memcpy
    std::memcpy(d, s, size % 64);
}
struct PublishRequest{
	int client_id;
	size_t client_order; // Must start from 0
	char topic[32];
	bool acknowledge;
	std::atomic<int> *counter;
	void* payload_address;
	size_t size;
};

void* cxl_addr;
std::atomic<size_t> off{0};
char buf[1024];
#define LOOPLEN 10000
void CXLWriteBandwidthTest(){
	PublishRequest req;
	memset(req.topic, 0, 32);
	req.topic[0] = '0';
	req.client_id = 0;
	req.client_order = 1;
	req.size = 1024;
	req.payload_address = malloc(1024);;
	/*
	for(int i=0; i<LOOPLEN; i++){
		t->PulishToCXL(req);
	}
	*/
	for(int i=0; i<LOOPLEN; i++){
		memcpy_nt((uint8_t*)cxl_addr + off.fetch_add(1024), req.payload_address, 1024);
	}
	free(req.payload_address);
}

int main(int argc, char* argv[]){

	int fd = open("/dev/dax0.0", O_RDWR);
	if (fd == -1) {
		perror("open() failed");
		return 1;
	}
	cxl_addr = mmap(NULL, (1UL<<37), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
	if (cxl_addr == MAP_FAILED) {
		perror("mmap() failed");
		close(fd);
		return 1;
	}
	double NUM_THREADS = 256;
    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();
    for (double i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(CXLWriteBandwidthTest);
    }
    // Join threads
    for (double i = 0; i < NUM_THREADS; ++i) {
        threads[i].join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;

    double bytes_written =NUM_THREADS * (double)LOOPLEN/1024 ;
    double bandwidth = bytes_written / (duration.count() *1024); // Convert bytes to MB

    std::cout << "Runtime: " << duration.count() << std::endl;
    std::cout << "Internal Publish bandwidth: " << bandwidth << " GB/s" << std::endl;

	return 0;
}
