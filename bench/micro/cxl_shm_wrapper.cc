#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <string>
#include <iostream>

extern "C" void* bench_map_cxl(size_t size) {
    int fd = shm_open("/CXL_SHARED_FILE", O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        std::cerr << "shm_open failed: " << strerror(errno) << std::endl;
        return nullptr;
    }
    if (ftruncate(fd, size) == -1) {
        std::cerr << "ftruncate failed: " << strerror(errno) << std::endl;
        close(fd);
        return nullptr;
    }
    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << std::endl;
        return nullptr;
    }
    std::memset(addr, 0, size);
    return addr;
}
