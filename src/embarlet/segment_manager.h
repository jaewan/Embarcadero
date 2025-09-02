#pragma once

#include <atomic>
#include <functional>
#include "../common/common.h"
#include "interfaces.h"

namespace Embarcadero {

/**
 * SegmentManager handles segment allocation and boundary checking
 * Extracted from Topic class to separate segment management concerns
 */
class SegmentManager : public ISegmentManager {
public:
    using GetNewSegmentFunc = std::function<void*(size_t, size_t, size_t&, SegmentMetadata&)>;

    SegmentManager(void* cxl_addr, size_t segment_size);
    ~SegmentManager() = default;

    // ISegmentManager interface
    void* GetNewSegment(size_t size, size_t msg_size, size_t& segment_size, SegmentMetadata& metadata) override;
    bool CheckSegmentBoundary(void* log, size_t msg_size, SegmentMetadata& metadata) override;

    // Set callback for getting new segments
    void SetGetNewSegmentCallback(GetNewSegmentFunc func) {
        get_new_segment_callback_ = func;
    }

    // Get current segment information
    void* GetCurrentSegmentStart() const { return current_segment_start_; }
    size_t GetCurrentSegmentSize() const { return current_segment_size_; }

private:
    void* cxl_addr_;
    size_t segment_size_;
    
    // Current segment tracking
    std::atomic<void*> current_segment_start_{nullptr};
    std::atomic<size_t> current_segment_size_{0};
    std::atomic<void*> segment_end_{nullptr};
    
    // Callback for getting new segments
    GetNewSegmentFunc get_new_segment_callback_;
};

} // namespace Embarcadero
