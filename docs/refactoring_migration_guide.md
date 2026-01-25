# Embarcadero Topic Class Refactoring Migration Guide

## ⚠️ ARCHIVED - Alternative Architecture Reference

**Status:** This document describes an **alternative/experimental architecture** (`TopicRefactored`) that is **NOT** the production implementation.

- **Current Production Path:** The main codebase uses the monolithic `Topic` class (`src/embarlet/topic.h`, `src/embarlet/topic.cc`)
- **Alternative Architecture:** `TopicRefactored` (`src/embarlet/topic_refactored.h`, `src/embarlet/topic_refactored.cc`) demonstrates a modular design with specialized components
- **Migration Status:** ❌ **NOT PLANNED** - Production system uses `Topic` class
- **Purpose:** This guide is preserved for:
  - Reference: Understanding alternative architectural patterns
  - Learning: Example of modular component design
  - Research: Evaluating architectural alternatives for future consideration

**⚠️ DO NOT USE FOR PRODUCTION:** This is an experimental/alternative design. All production code should use the `Topic` class.

**Last Updated:** 2026-01-26 (Marked as archived/alternative)

---

## Overview

This guide helps migrate from the monolithic `Topic` class to the new modular architecture with specialized components.

## Architecture Changes

### Before (Monolithic)
```cpp
class Topic {
    // 900+ lines handling:
    // - Buffer allocation
    // - Message ordering
    // - Replication
    // - Segment management
    // - Message export
    // - Thread management
};
```

### After (Modular)
```cpp
// Specialized components:
BufferManager       // Buffer allocation strategies
MessageOrdering     // Sequencing and ordering
ReplicationManager  // Replication logic
MessageExport       // Subscriber delivery
SegmentManager      // Segment boundaries
CallbackManager     // Modern callback handling
```

## Migration Steps

### 1. Replace Topic Construction

**Old:**
```cpp
auto topic = std::make_unique<Topic>(
    topic_name, cxl_addr, tinode, replica_tinode,
    broker_id, seq_type, order, ack_level, replication_factor
);
```

**New:**
```cpp
auto topic = std::make_unique<TopicRefactored>(
    topic_name, cxl_addr, tinode, replica_tinode,
    broker_id, seq_type, order, ack_level, replication_factor
);
topic->Initialize();  // Required initialization step
topic->Start();       // Start processing threads
```

### 2. Update Callback Registration

**Old (Function Pointers):**
```cpp
topic->GetNewSegmentCallback = &MyClass::GetNewSegment;
topic->GetNumBrokersCallback = &MyClass::GetNumBrokers;
```

**New (std::function):**
```cpp
topic->SetGetNewSegmentCallback(
    [this](size_t size, size_t msg_size, size_t& segment_size, SegmentMetadata& metadata) {
        return this->GetNewSegment(size, msg_size, segment_size, metadata);
    }
);

topic->SetGetNumBrokersCallback([this]() {
    return this->GetNumBrokers();
});
```

### 3. Buffer Allocation Changes

**Old:**
```cpp
// Direct call to GetCXLBuffer with function pointer selection
(this->*GetCXLBufferFunc)(batch_header, log, logical_offset, callback);
```

**New:**
```cpp
// Unified interface
topic->GetCXLBuffer(batch_header, log, logical_offset, callback);
```

### 4. Using Individual Components

For advanced use cases, components can be used independently:

```cpp
// Create segment manager
auto segment_mgr = std::make_shared<SegmentManager>(cxl_addr, segment_size);

// Create buffer manager with segment manager
auto buffer_mgr = std::make_unique<BufferManager>(
    cxl_addr, current_segment, log_addr, batch_headers_addr, broker_id
);
buffer_mgr->SetSegmentManager(segment_mgr);

// Use modern callback system
auto callback_mgr = std::make_unique<CallbackManager>();
callback_mgr->RegisterCallback<CallbackManager::BufferCompletionCallback>(
    "buffer_complete",
    [](size_t start, size_t end) {
        LOG(INFO) << "Buffer completed: " << start << "-" << end;
    }
);
```

### 5. Event-Based Communication

Replace tight coupling with event-based patterns:

```cpp
// Subscribe to events
callback_mgr->Subscribe<BufferAllocationEvent>(
    CallbackManager::EventType::BUFFER_ALLOCATED,
    [](const BufferAllocationEvent& event) {
        LOG(INFO) << "Buffer allocated at: " << event.address;
    }
);

// Publish events
callback_mgr->Publish(
    CallbackManager::EventType::BUFFER_ALLOCATED,
    BufferAllocationEvent{.address = buffer_addr, .size = buffer_size}
);
```

### 6. Testing with Mocks

Create mock implementations for testing:

```cpp
class MockReplicationManager : public IReplicationManager {
public:
    bool Initialize() override { return true; }
    void ReplicateCorfuData(size_t, size_t, void*) override {
        replication_count_++;
    }
    // Test helper
    int GetReplicationCount() const { return replication_count_; }
private:
    int replication_count_ = 0;
};

// Use in tests
auto mock_repl = std::make_unique<MockReplicationManager>();
// Inject mock into system under test
```

## Breaking Changes

1. **Initialization Required**: Must call `Initialize()` before using the topic
2. **Callback Types**: Function pointers replaced with `std::function`
3. **Thread Management**: Explicit `Start()` and `Stop()` calls required
4. **Error Handling**: Exceptions instead of error codes in some cases

## Performance Considerations

- **No Virtual Function Overhead**: Interfaces only used where flexibility needed
- **Same Memory Layout**: CXL memory access patterns unchanged
- **Lock-Free Where Possible**: Atomic operations preserved
- **Zero-Copy**: Buffer management maintains zero-copy semantics

## Gradual Migration Strategy

1. **Phase 1**: Replace Topic with TopicRefactored (drop-in replacement)
2. **Phase 2**: Update callback registration to use lambdas
3. **Phase 3**: Extract component usage for specific subsystems
4. **Phase 4**: Implement custom components for special requirements
5. **Phase 5**: Add comprehensive unit tests using mocks

## Common Issues and Solutions

### Issue: Compilation errors with callbacks
**Solution**: Ensure lambda captures are correct:
```cpp
// Capture 'this' for member function access
[this](...) { return this->MemberFunction(...); }
```

### Issue: Missing initialization
**Solution**: Always call Initialize() after construction:
```cpp
auto topic = std::make_unique<TopicRefactored>(...);
if (!topic->Initialize()) {
    LOG(ERROR) << "Initialization failed";
    return;
}
```

### Issue: Thread synchronization
**Solution**: Use Start()/Stop() for proper lifecycle:
```cpp
topic->Start();  // Start processing
// ... do work ...
topic->Stop();   // Clean shutdown
```

## Benefits After Migration

1. **Testability**: Mock individual components
2. **Maintainability**: Focused classes with single responsibilities  
3. **Flexibility**: Swap implementations easily
4. **Debugging**: Isolated components easier to debug
5. **Reusability**: Components usable in other contexts
6. **Documentation**: Clear interfaces document behavior

## Next Steps

- Review `refactoring_example.cc` for complete examples
- Run unit tests in `test/embarlet/` directory
- Profile performance with new architecture
- Report any migration issues to the team
