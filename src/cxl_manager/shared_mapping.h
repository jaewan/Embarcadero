#pragma once

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "cxl_manager/region_descriptor.h"

namespace Embarcadero::cxl_manager {

class SharedMapping {
 public:
    SharedMapping(void* address, size_t bytes) : address_(address), bytes_(bytes) {}
    ~SharedMapping() { if (address_) munmap(address_, bytes_); }
    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;
    void* get() const { return address_; }
    void* release() { auto* result = address_; address_ = nullptr; return result; }
 private:
    void* address_;
    size_t bytes_;
};

inline void ValidateMappingAddress(uintptr_t base, size_t bytes) {
    const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    if (!base || !bytes || base % page || bytes > std::numeric_limits<uintptr_t>::max() - base)
        throw std::invalid_argument("shared-region base must be nonzero, page aligned, and fit the region extent");
}

inline std::optional<uintptr_t> ParseMappingAddress(const char* value) {
    if (!value || !*value) return std::nullopt;
    if (*value == '-' || *value == '+' || std::isspace(static_cast<unsigned char>(*value)))
        throw std::invalid_argument("invalid EMBARCADERO_CXL_BASE_ADDR: expected an unsigned address");
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 0);
    if (errno || end == value || *end || !parsed || parsed > UINTPTR_MAX)
        throw std::invalid_argument("invalid EMBARCADERO_CXL_BASE_ADDR: expected a nonzero address");
    return static_cast<uintptr_t>(parsed);
}

// Never use MAP_FIXED. Older kernels may ignore NOREPLACE and return a different
// address: unmap that result and reject it. The hint-only path has the same safe
// behavior on platforms without NOREPLACE and is explicitly exercised in tests.
inline void* MapExactShared(int fd, size_t bytes, uintptr_t base, bool populate,
                            bool use_noreplace = true) {
    ValidateMappingAddress(base, bytes);
    int flags = MAP_SHARED | (populate ? MAP_POPULATE : 0);
#ifdef MAP_FIXED_NOREPLACE
    if (use_noreplace) flags |= MAP_FIXED_NOREPLACE;
#else
    (void)use_noreplace;
#endif
    void* result = mmap(reinterpret_cast<void*>(base), bytes, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (result != MAP_FAILED && result != reinterpret_cast<void*>(base)) {
        munmap(result, bytes);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return result;
}

// Requires an immutable descriptor published by the current head. The caller's
// registration readiness gate excludes reinitialization of a live region; this
// is not an active-head replacement protocol. Only this bounded read-only probe
// is mapped before readiness and geometry validation, never dependent metadata.
template <typename Validate, typename Invalidate>
uintptr_t DiscoverPublishedBase(int fd, size_t bytes, std::optional<uintptr_t> explicit_base,
        Validate validate, Invalidate invalidate,
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    if (bytes < kRegionDescriptorOffset + sizeof(RegionDescriptor))
        throw std::invalid_argument("shared region is too small for its descriptor");
    // Device-DAX mappings can require 2 MiB granularity; tiny memfd tests use
    // their complete extent. No region payload is inspected by this probe.
    const auto probe_bytes = std::min(bytes, size_t{2} << 20);
    auto* probe = mmap(nullptr, probe_bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (probe == MAP_FAILED)
        throw std::runtime_error(std::string("cannot read shared-region descriptor: ") + strerror(errno));
    SharedMapping owner(probe, probe_bytes);
    auto* descriptor = reinterpret_cast<const RegionDescriptor*>(
        static_cast<const char*>(probe) + kRegionDescriptorOffset);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        invalidate(descriptor);
        if (descriptor->ready.load(std::memory_order_acquire) == kRegionReady) break;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("shared-region descriptor is not ready: start the head first and use its region name; stale layouts require a fresh quiescent region");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    validate(*descriptor);
    const auto base = descriptor->mapping_base;
    ValidateMappingAddress(base, bytes);
    if (explicit_base && *explicit_base != base) {
        std::ostringstream message;
        message << "EMBARCADERO_CXL_BASE_ADDR 0x" << std::hex << *explicit_base
                << " conflicts with head-published shared-region base 0x" << base
                << "; remove the follower override or use the head's address";
        throw std::runtime_error(message.str());
    }
    return base;
}

inline void* SelectAndMapShared(int fd, size_t bytes, bool populate,
                               const std::vector<uintptr_t>& candidates,
                               bool use_noreplace = true) {
    std::ostringstream failures;
    for (auto base : candidates) {
        auto* mapped = MapExactShared(fd, bytes, base, populate, use_noreplace);
        if (mapped != MAP_FAILED) return mapped;
        const int error = errno;
        failures << " 0x" << std::hex << base << std::dec << ": " << strerror(error) << ';';
    }
    throw std::runtime_error("cannot map shared region without replacing existing mappings; attempted" +
        failures.str() + " select an unoccupied EMBARCADERO_CXL_BASE_ADDR on the head and restart the quiescent cluster with the same region; a follower cannot select an independent fallback");
}

}  // namespace Embarcadero::cxl_manager
