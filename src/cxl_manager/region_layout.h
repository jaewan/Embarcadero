#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>

#include "common/configuration.h"
#include "cxl_manager/region_descriptor.h"
#include "cxl_manager/baseline_cxl_layout.h"

namespace Embarcadero::cxl_manager {

// The existing layout, checked once before mapping. No mutable process-global
// geometry: each region and each allocator fixture owns its own calculation.
struct RegionLayout {
    size_t region_bytes;
    size_t base_regions_offset;
    size_t tinode_bytes;
    size_t bitmap_bytes;
    size_t batch_headers_bytes;
    size_t session_table_bytes;
    size_t metadata_bytes;
    size_t payload_bytes;
    size_t segment_size;
    size_t segment_count;
    size_t bitmap_words;
};

static_assert(sizeof(ControlBlock) <= kRegionDescriptorOffset);
static_assert(kRegionDescriptorOffset + sizeof(RegionDescriptor) <= kCompletionVectorOffset);

inline bool RegionDescriptorMatches(const RegionDescriptor& d, const RegionLayout& r,
        size_t brokers, size_t topics, size_t pbr_bytes, uint64_t backend, uint64_t mapping_base = 0) {
    return d.version == kCxlLayoutVersion && d.region_bytes == r.region_bytes &&
        d.metadata_bytes == r.metadata_bytes && d.segment_size == r.segment_size &&
        d.pbr_bytes == pbr_bytes && d.brokers == brokers && d.topics == topics &&
        d.backend == backend && d.mapping_base == mapping_base;
}

inline size_t CheckedLayoutAdd(size_t a, size_t b) {
    if (b > std::numeric_limits<size_t>::max() - a)
        throw std::invalid_argument("region layout addition overflow");
    return a + b;
}
inline size_t CheckedLayoutMultiply(size_t a, size_t b) {
    if (a && b > std::numeric_limits<size_t>::max() / a)
        throw std::invalid_argument("region layout multiplication overflow");
    return a * b;
}

inline std::optional<size_t> ClaimSegment(uint64_t* bitmap, size_t count, size_t& hint) {
    if (!bitmap || !count) return std::nullopt;
    const size_t words = count / 64 + (count % 64 != 0);
    for (size_t offset = 0; offset < words; ++offset) {
        const size_t word = (hint % words + offset) % words;
        const auto valid = (word + 1 == words && count % 64)
            ? ((uint64_t{1} << (count % 64)) - 1) : UINT64_MAX;
        auto occupied = __atomic_load_n(&bitmap[word], __ATOMIC_ACQUIRE);
        while ((~occupied & valid) != 0) {
            const unsigned bit = __builtin_ctzll(~occupied & valid);
            const uint64_t desired = occupied | (uint64_t{1} << bit);
            if (__atomic_compare_exchange_n(&bitmap[word], &occupied, desired, true,
                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                hint = word;
                return word * 64 + bit;
            }
        }
    }
    return std::nullopt;
}

inline RegionLayout CalculateRegionLayout(size_t region_bytes, size_t segment_size,
        size_t max_brokers, size_t max_topics, size_t pbr_bytes, size_t prefix_bytes) {
    constexpr size_t line = 64;
    if (!max_brokers || max_brokers > NUM_MAX_BROKERS || !max_topics ||
        segment_size <= line || segment_size % line || pbr_bytes < 2 * sizeof(BatchHeader) ||
        pbr_bytes % sizeof(BatchHeader) || prefix_bytes % line)
        throw std::invalid_argument("invalid region layout geometry");
    RegionLayout r{};
    r.region_bytes = region_bytes;
    r.segment_size = segment_size;
    r.base_regions_offset = prefix_bytes;
    const auto inode = CheckedLayoutMultiply(sizeof(TInode), max_topics);
    r.tinode_bytes = CheckedLayoutMultiply(CheckedLayoutAdd(inode, line - 1) / line, line);
    r.bitmap_bytes = CheckedLayoutMultiply(line, max_topics);
    r.batch_headers_bytes = CheckedLayoutMultiply(CheckedLayoutMultiply(max_brokers, pbr_bytes), max_topics);
    r.session_table_bytes = CheckedLayoutMultiply(CheckedLayoutMultiply(kMaxSessions, sizeof(SessionEntry)), max_topics);
    r.metadata_bytes = prefix_bytes;
    for (const auto bytes : {r.tinode_bytes, r.bitmap_bytes, r.batch_headers_bytes, r.session_table_bytes})
        r.metadata_bytes = CheckedLayoutAdd(r.metadata_bytes, bytes);
    if (r.metadata_bytes > region_bytes || segment_size > region_bytes - r.metadata_bytes)
        throw std::invalid_argument("region cannot contain metadata and one payload segment");
    r.payload_bytes = (region_bytes - r.metadata_bytes) / line * line;
    r.segment_count = r.payload_bytes / segment_size;
    r.bitmap_words = CheckedLayoutAdd(r.segment_count, 63) / 64;
    if (CheckedLayoutMultiply(r.bitmap_words, sizeof(uint64_t)) > r.bitmap_bytes)
        throw std::invalid_argument("segment bitmap cannot represent the payload pool");
    return r;
}

inline RegionLayout CalculateRegionLayout(const EmbarcaderoConfig& config) {
    const auto brokers = config.broker.max_brokers.get();
    const auto topics = config.storage.max_topics.get();
    if (brokers < 1 || brokers > NUM_MAX_BROKERS || topics < 1)
        throw std::invalid_argument("invalid broker/topic capacity");
    return CalculateRegionLayout(config.cxl.size.get(), config.storage.segment_size.get(),
        brokers, topics, config.storage.batch_headers_size.get(), BaseRegionsOffset(brokers));
}

}  // namespace Embarcadero::cxl_manager
