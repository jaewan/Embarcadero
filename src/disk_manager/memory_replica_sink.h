#pragma once
#include <cstddef>
#include <cstring>
#include <memory>

namespace Embarcadero {
// Single-writer volatile overwrite sink. Its completion is a copy ACK only;
// wrapping deliberately discards old bytes and provides no durability.
class MemoryReplicaSink {
public:
    using Allocate = void* (*)(size_t);
    using Release = void (*)(void*);
    MemoryReplicaSink(size_t capacity, Allocate allocate, Release release)
        : storage_(capacity ? allocate(capacity) : nullptr, release), capacity_(capacity) {}
    bool ready() const { return storage_ != nullptr; }
    size_t cursor() const { return cursor_; }
    bool Append(const void* source, size_t bytes) {
        if (!ready() || bytes > capacity_ || (bytes && !source)) return false;
        // Subtraction avoids overflow, including at the exact end of the sink.
        const size_t start = bytes > capacity_ - cursor_ ? 0 : cursor_;
        if (bytes) std::memcpy(static_cast<char*>(storage_.get()) + start, source, bytes);
        cursor_ = start + bytes;
        return true;
    }
private:
    std::unique_ptr<void, Release> storage_;
    size_t capacity_;
    size_t cursor_ = 0;
};
}  // namespace Embarcadero
