#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <emmintrin.h>

#define NUM_THREADS  256 // Number of threads to use
#define ARRAY_SIZE  (1UL<<30) // Adjust the size based on your system's memory bandwidth
double *buf;

#define NT_THRESHOLD 128

std::atomic<size_t> off_{0};

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

void* vbuf;
void atomicMemoryCpy(void* arr, int start, int end) {
	size_t size = 1024;
	int num = ((end - start)*sizeof(double))/size;
	while(0 < num){
		size_t off = off_.fetch_add(size);
		nt_memcpy((uint8_t*)arr + off, (uint8_t*)vbuf + off, size);
		num--;
	}
}
void memoryCpy(double* arr, int start, int end) {
	size_t size = 1024;
	//memcpy(&arr[start], buf, end-start);
	//memcpy_nt(&arr[start], buf, end-start);
	//nt_memcpy(&arr[start], buf, end-start);
	void* addr = &arr[start];
	size_t off = 0;
	while(off < (end-start)){
		//nt_memcpy((uint8_t*)&arr[start]+size, (uint8_t*)buf+size, size);
		memcpy_nt((uint8_t*)&arr[start]+size, (uint8_t*)buf+size, size);
		off += size;
	}
}

void writeMemory(double* arr, int start, int end) {
	for (int i = start; i < end; ++i) {
		arr[i] = 0.0;
	}
}

int main() {
	int fd = open("/dev/dax0.0", O_RDWR);
	if (fd == -1) {
		perror("open() failed");
		return 1;
	}
	//void* dax_addr = mmap(NULL, sizeof(double)*ARRAY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
	void* dax_addr = mmap(NULL, (1UL<<37), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
	if (dax_addr == MAP_FAILED) {
		perror("mmap() failed");
		close(fd);
		return 1;
	}
	double* arr = (double*)dax_addr;
	//double* arr = new double[ARRAY_SIZE];

	buf = (double*)malloc(sizeof(double)*ARRAY_SIZE);
	vbuf = malloc(sizeof(double)*ARRAY_SIZE);

	std::thread threads[NUM_THREADS];
	long long int chunkSize = ARRAY_SIZE / NUM_THREADS;

	auto start = std::chrono::high_resolution_clock::now();

	// Create and start threads
	for (int i = 0; i < NUM_THREADS; ++i) {
		int startIdx = i * chunkSize;
		int endIdx = (i == NUM_THREADS - 1) ? ARRAY_SIZE : startIdx + chunkSize;
		threads[i] = std::thread(memoryCpy, arr, startIdx, endIdx);
		//threads[i] = std::thread(atomicMemoryCpy, dax_addr, startIdx, endIdx);
	}

	// Join threads
	for (int i = 0; i < NUM_THREADS; ++i) {
		threads[i].join();
	}

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> duration = end - start;

	double bytes_written = ARRAY_SIZE * sizeof(double);
	double bandwidth = bytes_written / (duration.count() * 1024 *1024*1024); // Convert bytes to MB

	std::cout << "Maximum memory write bandwidth: " << bandwidth << " GB/s" << std::endl;

	//delete[] arr;
	munmap(dax_addr, sizeof(double)*ARRAY_SIZE);
	close(fd);
	return 0;
}
