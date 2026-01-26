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

// ============================================================================
// Configuration constants (bounds for validation)
// ============================================================================

static constexpr size_t MAX_MESSAGE_PAYLOAD_SIZE = 1024 * 1024;  // 1 MB max payload
static constexpr size_t MAX_BATCH_MESSAGES = 10000;               // Max messages per batch
static constexpr size_t MAX_BATCH_TOTAL_ORDER = 100000000;        // Sanity check on total_order

}  // namespace wire
}  // namespace Embarcadero
