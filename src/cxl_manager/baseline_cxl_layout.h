#pragma once

#include <cstddef>
#include <cstdint>

#include "cxl_manager/cxl_datastructure.h"
#include "cxl_transport/cxl_mailbox.h"

namespace Embarcadero::cxl_manager {

// Increment whenever any offset or reserved extent changes.  Artifacts record
// this value, so results from incompatible shared-memory layouts cannot mix.
// v4 adds BatchHeader::original_client_batch_seq, required to keep Corfu
// durable ValueId stable after token assignment rewrites batch_seq.
inline constexpr uint32_t kCxlLayoutVersion = 4;
inline constexpr size_t kControlBlockOffset = 0;
inline constexpr size_t kCompletionVectorOffset = 0x1000;
inline constexpr size_t kGOIOffset = 0x2000;
inline constexpr size_t kMaxGOIEntries = 256ULL * 1024 * 1024;
inline constexpr size_t kPageSize = 4096;
inline constexpr size_t kMailboxRecordSize = 512;
inline constexpr uint32_t kMailboxCapacity = 1024;

constexpr size_t AlignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

inline cxl_transport::MailboxParams BaselineMailboxParams(uint32_t max_brokers) {
  cxl_transport::MailboxParams params;
  params.num_brokers = max_brokers;
  params.record_size = kMailboxRecordSize;
  params.up_capacity = kMailboxCapacity;
  params.down_capacity = kMailboxCapacity;
  return params;
}

inline size_t BaselineMailboxBytes(uint32_t max_brokers) {
  return AlignUp(cxl_transport::MailboxSegment::BytesNeeded(BaselineMailboxParams(max_brokers)),
                 kPageSize);
}

inline constexpr size_t GOIEndOffset() {
  return kGOIOffset + kMaxGOIEntries * sizeof(GOIEntry);
}

inline size_t BaselineMailboxOffset() { return AlignUp(GOIEndOffset(), cxl_transport::kCacheLine); }

inline size_t BaseRegionsOffset(uint32_t max_brokers) {
  return BaselineMailboxOffset() + BaselineMailboxBytes(max_brokers);
}

}  // namespace Embarcadero::cxl_manager
