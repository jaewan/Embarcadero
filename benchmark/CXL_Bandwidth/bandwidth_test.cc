#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <chrono>
#include <atomic>
#include <vector>
#include <thread>
#include <immintrin.h>

#define NT_THRESHOLD 128
#define SIZE  (1UL<<35)
#define WRITE_SIZE  (1UL<<30)
#define NUM_THREADS 4 
void* dax_addr;
void* dram_addr;

void nt_memcpy(void *__restrict dst, const void * __restrict src, size_t n){
	static size_t CACHE_LINE_SIZE = sysconf (_SC_LEVEL1_DCACHE_LINESIZE);
	if (n < NT_THRESHOLD) {
		memcpy(dst, src, n);
		return;
	}

	size_t n_unaligned = CACHE_LINE_SIZE - (uintptr_t)dst % CACHE_LINE_SIZE;

	if (n_unaligned > n)
		n_unaligned = n;

	memcpy(dst, src, n_unaligned);
	dst = (void*)(((uint8_t*)dst) + n_unaligned);
	src = (void*)(((uint8_t*)src) + n_unaligned);
	n -= n_unaligned;

	size_t num_lines = n / CACHE_LINE_SIZE;

	size_t i;
	for (i = 0; i < num_lines; i++) {
		size_t j;
		for (j = 0; j < CACHE_LINE_SIZE / sizeof(__m128i); j++) {
			__m128i blk = _mm_loadu_si128((const __m128i *)src);
			/* non-temporal store */
			_mm_stream_si128((__m128i *)dst, blk);
			src = (void*)(((uint8_t*)src) + sizeof(__m128i));
			dst = (void*)(((uint8_t*)dst) + sizeof(__m128i));
		}
		n -= CACHE_LINE_SIZE;
	}

	if (num_lines > 0)
		_mm_sfence();

	memcpy(dst, src, n);
}

void single_thread(){
	size_t granularities[8] = {1,2,4,8,16,32,32,32};//{64,128,512,1024,1024*2,1024*4, 1024*10,1024*1024};
	size_t results[2][8];

	for(int i=0; i<8; i++){
		void* buf = malloc(granularities[i]);
		memset(buf,0,granularities[i]);


		//CXL bandwidth test
		auto startTime = std::chrono::steady_clock::now();
		auto endTime = startTime + std::chrono::seconds(5);
		size_t off=0;
		results[0][i] = 0;

		while (std::chrono::steady_clock::now() < endTime) {
			off = (off+granularities[i])%SIZE;
			nt_memcpy((uint8_t*)dax_addr+off, buf, granularities[i]);
			results[0][i]++;
		}

		//DRAM bandwidth test
		startTime = std::chrono::steady_clock::now();
		endTime = startTime + std::chrono::seconds(5);
		off=0;
		results[1][i] = 0;
		while (std::chrono::steady_clock::now() < endTime) {
			off = (off+granularities[i])%SIZE;
			nt_memcpy((uint8_t*)dram_addr+off, buf, granularities[i]);
			results[1][i]++;
		}
		free(buf);
	}

	for(int i=0; i<8; i++){
		std::cout << (results[0][i]/5)*granularities[i] <<  std::endl;
	}
	for(int i=0; i<8; i++){
		std::cout << (results[1][i]/5)*granularities[i] <<  std::endl;
	}
}

std::atomic<int> done{0};
char* buf_;
bool DRAM;

void MemoryWrite(int id ){
	size_t off = (size_t)id * WRITE_SIZE;
	if(DRAM){
		nt_memcpy((uint8_t*)dram_addr + off, buf_, WRITE_SIZE);
	}else{
		nt_memcpy((uint8_t*)dax_addr + off, buf_, WRITE_SIZE);
	}
	//done.fetch_add(1, std::memory_order_relaxed);
}

void MaxBandwidth(){
	buf_ = (char*)malloc(sizeof(char)*WRITE_SIZE);
	std::vector<std::thread> dram_threads(NUM_THREADS);
	std::vector<std::thread> cxl_threads(NUM_THREADS);

	DRAM = true;
	auto start_time = std::chrono::high_resolution_clock::now();
	for(int i=0; i < NUM_THREADS; i++){
		dram_threads.emplace_back(MemoryWrite, i);
	}
	//while(done.load(std::memory_order_relaxed) < NUM_THREADS){}
	for (auto& thread : dram_threads) {
        thread.join();
    }

	auto end_time = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    double total_bytes_written = static_cast<double>(NUM_THREADS * WRITE_SIZE);
    double bandwidth = (total_bytes_written / (1024.0 * 1024.0 * 1024.0)) / (duration / 1000.0); // GB/s

    std::cout << "Maximum memory write bandwidth: " << bandwidth << " GB/s" << std::endl;
	done.store(0);

	DRAM = false;
	start_time = std::chrono::high_resolution_clock::now();
	for(int i=0; i < NUM_THREADS; i++){
		cxl_threads.emplace_back(MemoryWrite, i);
	}
	while(done.load(std::memory_order_relaxed) < NUM_THREADS){}
	end_time = std::chrono::high_resolution_clock::now();
	duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    bandwidth = (total_bytes_written / (1024.0 * 1024.0 * 1024.0)) / (duration / 1000.0); // GB/s

    std::cout << "CXL memory write bandwidth: " << bandwidth << " GB/s" << std::endl;
	for (auto& thread : dram_threads) {
        thread.join();
    }

	free(buf_);
}

int main()
{

	int fd = open("/dev/dax0.0", O_RDWR);
	if (fd == -1) {
		perror("open() failed");
		return 1;
	}

	dax_addr = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (dax_addr == MAP_FAILED) {
		perror("mmap() failed");
		close(fd);
		return 1;
	}

	dram_addr = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED| MAP_ANONYMOUS, -1, 0);
	if (dram_addr == MAP_FAILED) {
		perror("dram mmap() failed");
		return 1;
	}

	MaxBandwidth();

	sleep(1);

	munmap(dax_addr, SIZE);
	munmap(dram_addr, SIZE);
	close(fd);
	return 0;
}
