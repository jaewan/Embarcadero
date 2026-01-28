#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "../../src/cxl_manager/cxl_datastructure.h"
#include "../../src/common/wire_formats.h"
#include <cstring>
#include <vector>
#include <algorithm>
#include <map>

using namespace Embarcadero;

/**
 * @brief Test BlogMessageHeader structure correctness and validation
 * 
 * Validates:
 * - Cache-line alignment and size
 * - Field offsets match specification
 * - Payload size validation
 * - Stride computation
 * - Stage-specific field ranges
 */
class BlogHeaderValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Allocate aligned memory for headers
        header_mem_ = aligned_alloc(64, 64 * 10); // 10 headers
        memset(header_mem_, 0, 64 * 10);
    }
    
    void TearDown() override {
        free(header_mem_);
    }
    
    void* header_mem_;
};

// Test 1: Structure size and alignment
TEST_F(BlogHeaderValidationTest, StructureSizeAndAlignment) {
    EXPECT_EQ(sizeof(BlogMessageHeader), 64) 
        << "BlogMessageHeader must be exactly 64 bytes (one cache line)";
    EXPECT_EQ(alignof(BlogMessageHeader), 64) 
        << "BlogMessageHeader must be 64-byte aligned";
}

// Test 2: Field offsets match cache-line partitioning
TEST_F(BlogHeaderValidationTest, FieldOffsets) {
    EXPECT_EQ(offsetof(BlogMessageHeader, size), 0) 
        << "Receiver region: size must be at offset 0";
    EXPECT_EQ(offsetof(BlogMessageHeader, received), 4) 
        << "Receiver region: received must be at offset 4";
    EXPECT_EQ(offsetof(BlogMessageHeader, ts), 8) 
        << "Receiver region: ts must be at offset 8";
    
    EXPECT_EQ(offsetof(BlogMessageHeader, counter), 16) 
        << "Delegation region: counter must be at offset 16";
    EXPECT_EQ(offsetof(BlogMessageHeader, flags), 20) 
        << "Delegation region: flags must be at offset 20";
    EXPECT_EQ(offsetof(BlogMessageHeader, processed_ts), 24) 
        << "Delegation region: processed_ts must be at offset 24";
    
    EXPECT_EQ(offsetof(BlogMessageHeader, total_order), 32) 
        << "Sequencer region: total_order must be at offset 32";
    EXPECT_EQ(offsetof(BlogMessageHeader, ordered_ts), 40) 
        << "Sequencer region: ordered_ts must be at offset 40";
    
    EXPECT_EQ(offsetof(BlogMessageHeader, client_id), 48) 
        << "Read-only metadata: client_id must be at offset 48";
    EXPECT_EQ(offsetof(BlogMessageHeader, batch_seq), 56) 
        << "Read-only metadata: batch_seq must be at offset 56";
}

// Test 3: Payload size validation
TEST_F(BlogHeaderValidationTest, PayloadSizeValidation) {
    BlogMessageHeader* hdr = reinterpret_cast<BlogMessageHeader*>(header_mem_);
    
    // Valid sizes (payload_size > 0 required by ValidateV2Payload)
    hdr->size = 1;
    EXPECT_TRUE(wire::ValidateV2Payload(1, 128)) << "1-byte payload should be valid";
    
    hdr->size = 1024;
    size_t stride_1k = wire::ComputeStrideV2(1024);
    EXPECT_TRUE(wire::ValidateV2Payload(1024, stride_1k)) << "1KB payload should be valid";
    
    hdr->size = wire::MAX_MESSAGE_PAYLOAD_SIZE;
    size_t stride_max = wire::ComputeStrideV2(wire::MAX_MESSAGE_PAYLOAD_SIZE);
    EXPECT_TRUE(wire::ValidateV2Payload(wire::MAX_MESSAGE_PAYLOAD_SIZE, stride_max))
        << "Max payload size should be valid";
    
    // Invalid sizes
    EXPECT_FALSE(wire::ValidateV2Payload(0, 64)) << "Zero payload should be invalid (payload_size > 0 required)";
    
    hdr->size = wire::MAX_MESSAGE_PAYLOAD_SIZE + 1;
    EXPECT_FALSE(wire::ValidateV2Payload(wire::MAX_MESSAGE_PAYLOAD_SIZE + 1, 1000000))
        << "Payload exceeding max should be invalid";
    
    hdr->size = 100;
    EXPECT_FALSE(wire::ValidateV2Payload(100, 50)) 
        << "Payload larger than remaining bytes should be invalid";
}

// Test 4: Stride computation
TEST_F(BlogHeaderValidationTest, StrideComputation) {
    // Test various payload sizes
    struct TestCase {
        size_t payload;
        size_t expected_stride;
    };
    
    std::vector<TestCase> test_cases = {
        {0, 64},           // Minimum: header only (64+0 = 64, aligned to 64)
        {1, 128},          // 1 byte payload: 64+1 = 65, aligned to 128
        {64, 128},         // Exactly one cache line: 64+64 = 128, aligned to 128
        {65, 192},         // One byte over: 64+65 = 129, aligned to 192
        {1024, 1088},      // 1KB payload: 64+1024 = 1088, already aligned
        {1025, 1152},      // 1025 bytes: 64+1025 = 1089, aligned to 1152
    };
    
    for (const auto& tc : test_cases) {
        size_t stride = wire::ComputeStrideV2(tc.payload);
        EXPECT_EQ(stride, tc.expected_stride) 
            << "Payload=" << tc.payload << " should produce stride=" << tc.expected_stride;
        EXPECT_EQ(stride % 64, 0) 
            << "Stride must be 64-byte aligned for cache-line efficiency";
    }
}

// Test 5: Stage field initialization
TEST_F(BlogHeaderValidationTest, StageFieldInitialization) {
    BlogMessageHeader* hdr = reinterpret_cast<BlogMessageHeader*>(header_mem_);
    memset(hdr, 0, sizeof(BlogMessageHeader));
    
    // Receiver stage fields (bytes 0-15)
    hdr->size = 1024;
    hdr->received = 0;
    hdr->ts = 1234567890ULL;
    
    EXPECT_EQ(hdr->size, 1024U);
    EXPECT_EQ(hdr->received, 0U);
    EXPECT_EQ(hdr->ts, 1234567890ULL);
    
    // Delegation stage fields (bytes 16-31) - should be zero initially
    EXPECT_EQ(hdr->counter, 0U);
    EXPECT_EQ(hdr->flags, 0U);
    EXPECT_EQ(hdr->processed_ts, 0ULL);
    
    // Sequencer stage fields (bytes 32-47) - should be zero initially
    EXPECT_EQ(hdr->total_order, 0ULL);
    EXPECT_EQ(hdr->ordered_ts, 0ULL);
    
    // Read-only metadata (bytes 48-63)
    hdr->client_id = 42;
    hdr->batch_seq = 5;
    
    EXPECT_EQ(hdr->client_id, 42ULL);
    EXPECT_EQ(hdr->batch_seq, 5U);
}

// Test 6: False sharing prevention (cache-line boundaries)
TEST_F(BlogHeaderValidationTest, CacheLineBoundaries) {
    // Verify that writer regions don't overlap cache lines
    // Receiver writes: bytes 0-15 (cache line 0)
    // Delegation writes: bytes 16-31 (cache line 0, but different 16B region)
    // Sequencer writes: bytes 32-47 (cache line 0, but different 16B region)
    // Read-only: bytes 48-63 (cache line 0, but different 16B region)
    
    // All fields should fit in one cache line (64 bytes)
    EXPECT_LE(sizeof(BlogMessageHeader), 64);
    
    // Verify no field crosses cache line boundary within its region
    EXPECT_LT(offsetof(BlogMessageHeader, received) + sizeof(BlogMessageHeader::received), 16)
        << "Receiver region should not cross 16-byte boundary";
    EXPECT_LT(offsetof(BlogMessageHeader, counter) + sizeof(BlogMessageHeader::counter), 32)
        << "Delegation region should not cross 32-byte boundary";
    EXPECT_LT(offsetof(BlogMessageHeader, total_order) + sizeof(BlogMessageHeader::total_order), 48)
        << "Sequencer region should not cross 48-byte boundary";
}

// Test 7: Batch sequence consistency
TEST_F(BlogHeaderValidationTest, BatchSequenceConsistency) {
    // Create a batch of headers with same client_id and sequential batch_seq
    std::vector<BlogMessageHeader*> headers;
    for (size_t i = 0; i < 5; ++i) {
        BlogMessageHeader* hdr = reinterpret_cast<BlogMessageHeader*>(
            reinterpret_cast<uint8_t*>(header_mem_) + i * 64);
        memset(hdr, 0, sizeof(BlogMessageHeader));
        hdr->client_id = 100;
        hdr->batch_seq = i;
        hdr->size = 512;
        headers.push_back(hdr);
    }
    
    // Verify batch sequence is sequential
    for (size_t i = 0; i < headers.size(); ++i) {
        EXPECT_EQ(headers[i]->client_id, 100ULL) 
            << "All headers should have same client_id";
        EXPECT_EQ(headers[i]->batch_seq, i) 
            << "Batch sequence should be sequential";
    }
}

// Test 8: Total order assignment validation
TEST_F(BlogHeaderValidationTest, TotalOrderAssignment) {
    BlogMessageHeader* hdr = reinterpret_cast<BlogMessageHeader*>(header_mem_);
    memset(hdr, 0, sizeof(BlogMessageHeader));
    
    // Initially unassigned
    EXPECT_EQ(hdr->total_order, 0ULL);
    
    // Assign total order
    hdr->total_order = 42;
    EXPECT_EQ(hdr->total_order, 42ULL);
    
    // Verify it's in sequencer region (bytes 32-47)
    EXPECT_GE(offsetof(BlogMessageHeader, total_order), 32);
    EXPECT_LT(offsetof(BlogMessageHeader, total_order), 48);
}

// Test 9: Header version detection helper
TEST_F(BlogHeaderValidationTest, HeaderVersionHelpers) {
    EXPECT_TRUE(wire::IsValidHeaderVersion(1)) << "V1 should be valid";
    EXPECT_TRUE(wire::IsValidHeaderVersion(2)) << "V2 should be valid";
    EXPECT_FALSE(wire::IsValidHeaderVersion(0)) << "V0 should be invalid";
    EXPECT_FALSE(wire::IsValidHeaderVersion(3)) << "V3 should be invalid";
}

// Test 10: Stride computation edge cases
TEST_F(BlogHeaderValidationTest, StrideEdgeCases) {
    // Very small payloads (stride = Align64(64 + payload))
    EXPECT_EQ(wire::ComputeStrideV2(0), 64);      // 64+0 = 64, aligned to 64
    EXPECT_EQ(wire::ComputeStrideV2(1), 128);    // 64+1 = 65, aligned to 128
    EXPECT_EQ(wire::ComputeStrideV2(63), 128);    // 64+63 = 127, aligned to 128
    EXPECT_EQ(wire::ComputeStrideV2(64), 128);    // 64+64 = 128, already aligned
    
    // Large payloads
    size_t large_payload = wire::MAX_MESSAGE_PAYLOAD_SIZE;
    size_t large_stride = wire::ComputeStrideV2(large_payload);
    EXPECT_GE(large_stride, large_payload + 64) 
        << "Stride must include header size";
    EXPECT_EQ(large_stride % 64, 0) 
        << "Stride must be cache-line aligned";
}

// Test 11: FIFO validation preserves client order across out-of-order arrival
TEST_F(BlogHeaderValidationTest, SequencerFifoPreservesClientOrder) {
    struct FakeBatch {
        size_t batch_seq;
        size_t num_msg;
        size_t total_order;
    };

    std::map<size_t, size_t> next_expected;
    std::map<size_t, std::map<size_t, FakeBatch*>> deferred;
    size_t global_seq = 0;

    auto assign_batch = [&](FakeBatch& batch) {
        batch.total_order = global_seq;
        global_seq += batch.num_msg;
    };

    auto process_skipped = [&](size_t client_id) {
        auto& client_map = deferred[client_id];
        bool progressed = true;
        while (progressed && !client_map.empty()) {
            progressed = false;
            size_t expected = next_expected[client_id];
            auto it = client_map.find(expected);
            if (it != client_map.end()) {
                assign_batch(*it->second);
                client_map.erase(it);
                next_expected[client_id] = expected + 1;
                progressed = true;
            }
        }
    };

    auto handle_batch = [&](size_t client_id, FakeBatch& batch) {
        size_t expected = next_expected[client_id];
        if (batch.batch_seq == expected) {
            assign_batch(batch);
            next_expected[client_id] = expected + 1;
            process_skipped(client_id);
        } else if (batch.batch_seq > expected) {
            deferred[client_id][batch.batch_seq] = &batch;
        }
    };

    FakeBatch b0{0, 2, 0};
    FakeBatch b1{1, 2, 0};

    // Arrival order: seq=1 then seq=0
    handle_batch(42, b1);
    handle_batch(42, b0);

    // FIFO should assign b0 before b1
    EXPECT_EQ(b0.total_order, 0U);
    EXPECT_EQ(b1.total_order, 2U);
}
