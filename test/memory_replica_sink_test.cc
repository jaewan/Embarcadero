#include "disk_manager/memory_replica_sink.h"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
std::array<unsigned char, 18> memory;
int allocations = 0, frees = 0;
void* Allocate(size_t size) {
    ++allocations;
    if (size != 16) throw std::runtime_error("unexpected allocation");
    memory.fill(0xa5);
    return memory.data() + 1;
}
void Release(void* pointer) {
    if (pointer != memory.data() + 1) std::terminate();
    ++frees;
}
void Check(bool value) { if (!value) throw std::runtime_error("sink invariant failed"); }
}
int main() {
    using Embarcadero::MemoryReplicaSink;
    try {
        const std::array<unsigned char, 17> source{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17};
        {
            MemoryReplicaSink sink(16, Allocate, Release);
            Check(sink.ready() && sink.Append(source.data(), 16) && sink.cursor() == 16);
            Check(memory[16] == 16 && memory.front() == 0xa5 && memory.back() == 0xa5);
            Check(sink.Append(source.data(), 3) && sink.cursor() == 3);
            Check(memory[1] == 1 && memory[3] == 3 && memory[4] == 4);
            Check(sink.Append(source.data(), 12) && sink.cursor() == 15);
            Check(sink.Append(source.data(), 2) && sink.cursor() == 2);
            const auto before = memory;
            Check(!sink.Append(source.data(), 17) && sink.cursor() == 2 && memory == before);
            Check(!sink.Append(nullptr, 1) && sink.cursor() == 2 && memory == before);
            Check(memory.front() == 0xa5 && memory.back() == 0xa5 && allocations == 1);
        }
        Check(frees == 1);
        {
            MemoryReplicaSink failed(16, [](size_t) -> void* { return nullptr; }, Release);
            Check(!failed.ready() && !failed.Append(source.data(), 1) && failed.cursor() == 0);
        }
        Check(frees == 1);
        std::cout << "memory replica sink tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
