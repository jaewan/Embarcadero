#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Embarcadero::cxl_manager {

// Occupies previously reserved bytes between ControlBlock and the CV. Existing
// offsets stay unchanged; old processes must not attach to this layout version.
inline constexpr size_t kRegionDescriptorOffset = 128;
inline constexpr uint64_t kRegionReady = 0x454d424152435835ULL;
struct alignas(64) RegionDescriptor {
    std::atomic<uint64_t> ready{0};
    uint32_t version;
    uint32_t brokers;
    uint64_t region_bytes;
    uint64_t metadata_bytes;
    uint64_t segment_size;
    uint64_t pbr_bytes;
    uint32_t topics;
    uint32_t backend;
    uint64_t mapping_base;
};
static_assert(sizeof(RegionDescriptor) == 64);

}  // namespace Embarcadero::cxl_manager
