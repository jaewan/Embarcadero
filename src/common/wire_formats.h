#pragma once

#include <cstddef>
#include <cstdint>
#include <cassert>

/**
 * [[PAPER_SPEC: Wire format structures for broker-subscriber communication]]
 * This header centralizes wire format definitions to avoid duplication
 * and ensure consistency across sender and receiver.
 */

namespace Embarcadero {
namespace wire {

// ============================================================================
// Configuration constants (bounds for validation)
// ============================================================================

static constexpr size_t MAX_MESSAGE_PAYLOAD_SIZE = 1024 * 1024;  // 1 MB max payload
static constexpr size_t MAX_BATCH_MESSAGES = 10000;               // Max messages per batch
static constexpr size_t MAX_BATCH_TOTAL_ORDER = 100000000;        // Sanity check on total_order

// ============================================================================
// Helper functions for boundary and size calculations
// ============================================================================

/**
 * @brief Align size up to 64-byte boundary (cache-line aligned)
 * @param size Unaligned size
 * @return Size aligned up to next 64-byte boundary
 */
inline constexpr size_t Align64(size_t size) {
    return (size + 63) & ~63UL;
}

/**
 * @brief Compute message stride from BlogMessageHeader::size (payload bytes)
 * @param header_size Size of the header (usually 64B)
 * @param payload_size Payload bytes from hdr->size
 * @return Aligned stride to next message
 * 
 * [[PAPER_SPEC: BlogMessageHeader]] - size field contains payload bytes only
 */
inline constexpr size_t ComputeMessageStride(size_t header_size, size_t payload_size) {
    return Align64(header_size + payload_size);
}

// ============================================================================
// Header version helpers (ORDER=5 uses BatchMetadata.header_version)
// ============================================================================

static constexpr uint16_t HEADER_VERSION_V1 = 1;
static constexpr uint16_t HEADER_VERSION_V2 = 2;
static constexpr size_t V1_HEADER_SIZE = 64;  // MessageHeader size (aligned)
static constexpr size_t V2_HEADER_SIZE = 64;  // BlogMessageHeader size (aligned)

inline constexpr bool IsValidHeaderVersion(uint16_t version) {
    return version == HEADER_VERSION_V1 || version == HEADER_VERSION_V2;
}

inline constexpr size_t MaxV1PaddedSize() {
    return Align64(V1_HEADER_SIZE + MAX_MESSAGE_PAYLOAD_SIZE);
}

inline constexpr size_t ComputeStrideV2(size_t payload_size) {
    return Align64(V2_HEADER_SIZE + payload_size);
}

inline constexpr bool ValidateV1PaddedSize(size_t padded_size, size_t remaining_bytes) {
    return padded_size >= V1_HEADER_SIZE &&
           padded_size <= MaxV1PaddedSize() &&
           (padded_size % 64 == 0) &&
           padded_size <= remaining_bytes;
}

inline constexpr bool ValidateV2Payload(size_t payload_size, size_t remaining_bytes) {
    const size_t stride = ComputeStrideV2(payload_size);
    return payload_size > 0 &&
           payload_size <= MAX_MESSAGE_PAYLOAD_SIZE &&
           stride <= remaining_bytes;
}

inline constexpr size_t ComputeStrideByVersion(uint16_t version,
                                              size_t payload_size,
                                              size_t padded_size) {
    if (version == HEADER_VERSION_V2) {
        return ComputeStrideV2(payload_size);
    }
    if (version == HEADER_VERSION_V1) {
        return padded_size;
    }
    return 0;
}

// ============================================================================
// BatchMetadata: shared wire format (ORDER=5)
// ============================================================================

struct BatchMetadata {
    size_t batch_total_order;      // Starting total_order for this batch
    uint32_t num_messages;         // Number of messages in this batch
    uint16_t header_version;       // Message header format: 1=MessageHeader, 2=BlogMessageHeader
    uint16_t flags;                // Reserved for future flags
};

// Verify size and alignment are consistent across platforms
static_assert(sizeof(BatchMetadata) == 16, "BatchMetadata must be exactly 16 bytes");
static_assert(sizeof(size_t) == 8, "Assumes 64-bit size_t");

}  // namespace wire
}  // namespace Embarcadero
