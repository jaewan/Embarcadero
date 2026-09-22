#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace Embarcadero {
// Deterministic across processes; stops at NUL so a std::string::c_str() is safe.
inline size_t TopicSlot(const char* topic, size_t slots, size_t max_name_bytes = 256) {
    if (!topic || !*topic || !slots) throw std::invalid_argument("invalid topic identity");
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < max_name_bytes && topic[i] != '\0'; ++i) {
        hash ^= static_cast<unsigned char>(topic[i]);
        hash *= 1099511628211ULL;
    }
    return hash % slots;
}
}  // namespace Embarcadero
