#include "segment_manager.h"
#include <glog/logging.h>

namespace Embarcadero {

SegmentManager::SegmentManager(void* cxl_addr, size_t segment_size)
    : cxl_addr_(cxl_addr),
      segment_size_(segment_size) {}

void* SegmentManager::GetNewSegment(size_t size, size_t msg_size, size_t& segment_size, SegmentMetadata& metadata) {
    if (!get_new_segment_callback_) {
        LOG(ERROR) << "GetNewSegment callback not set";
        return nullptr;
    }

    void* new_segment = get_new_segment_callback_(size, msg_size, segment_size, metadata);
    
    if (new_segment) {
        current_segment_start_ = new_segment;
        current_segment_size_ = segment_size;
        segment_end_ = reinterpret_cast<uint8_t*>(new_segment) + segment_size;
    }

    return new_segment;
}

bool SegmentManager::CheckSegmentBoundary(void* log, size_t msg_size, SegmentMetadata& metadata) {
#ifdef MULTISEGMENT
    void* current_end = segment_end_.load();
    
    if (current_end && reinterpret_cast<uint8_t*>(log) + msg_size > current_end) {
        // Message would exceed current segment boundary
        size_t new_segment_size;
        void* new_segment = GetNewSegment(segment_size_, msg_size, new_segment_size, metadata);
        
        if (!new_segment) {
            LOG(ERROR) << "Failed to allocate new segment";
            return false;
        }

        // Update log pointer to new segment
        log = new_segment;
        metadata.is_new_segment = true;
        metadata.segment_start = new_segment;
        metadata.segment_size = new_segment_size;
        
        return true;
    }
#endif
    
    metadata.is_new_segment = false;
    return true;
}

} // namespace Embarcadero
