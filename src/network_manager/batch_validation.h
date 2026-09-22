#pragma once

#include <cstring>
#include "common/wire_formats.h"
#include "cxl_manager/cxl_datastructure.h"

namespace Embarcadero::network {
inline bool ValidBatchEnvelope(const BatchHeader& batch, size_t max_bytes, size_t client_id) {
    return batch.client_id == client_id && batch.total_size > 0 && batch.total_size <= max_bytes &&
        batch.num_msg > 0 && batch.num_msg <= batch.total_size / 64;
}

// This walk is independent of CXL cache policy and runs before any publication.
inline bool ValidateBatchBody(const void* buffer, size_t bytes, size_t count, bool v2) {
    if (!buffer || !bytes || !count || count > bytes / 64) return false;
    auto* cursor = static_cast<const uint8_t*>(buffer);
    size_t remaining = bytes;
    for (size_t i = 0; i < count; ++i) {
        if (remaining < 64) return false;
        size_t stride;
        if (v2) {
            BlogMessageHeader h;
            std::memcpy(&h, cursor, sizeof(h));
            if (!wire::ValidateV2Payload(h.size, remaining)) return false;
            stride = wire::ComputeStrideV2(h.size);
        } else {
            MessageHeader h;
            std::memcpy(&h, cursor, sizeof(h));
            if (!wire::ValidateV1PaddedSize(h.paddedSize, remaining)) return false;
            if (!h.size || h.size > wire::MAX_MESSAGE_PAYLOAD_SIZE ||
                h.size > h.paddedSize - sizeof(MessageHeader) ||
                (h.next_msg_diff != 0 && h.next_msg_diff != h.paddedSize)) return false;
            stride = h.paddedSize;
        }
        remaining -= stride;
        cursor += stride;
    }
    return remaining == 0;
}
}  // namespace Embarcadero::network
