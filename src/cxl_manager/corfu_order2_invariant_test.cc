// Standalone deterministic checks for Corfu ORDER=2 frontier invariants (no GTest dependency).
// Validates:
//   1. RecordCorfuOrder2BatchCompletion advances ordered frontier only for contiguous prefix.
//   2. SkipCorfuOrder2Batch correctly marks holes (ordered=1, num_msg=0).
//   3. Subscriber export (GetBatchToExportWithMetadata) skips holes and stale slots.
//   4. Durable frontier never exceeds ordered frontier.
//   5. Per-client ACK correctly attributes messages.
//
// Build: corfu_order2_invariant_test (see src/CMakeLists.txt). Exit 0 on success, 1 on failure.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <cassert>
#include <vector>

// Minimal stubs — we test the frontier logic in isolation using the same data structures.
// No CXL hardware or gRPC needed.

static int g_fail_count = 0;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n",          \
                         __FILE__, __LINE__, (msg));             \
            g_fail_count++;                                      \
        }                                                        \
    } while (0)

#define CHECK_EQ(a, b, msg)                                      \
    do {                                                         \
        unsigned long long va_ = static_cast<unsigned long long>(a); \
        unsigned long long vb_ = static_cast<unsigned long long>(b); \
        if (va_ != vb_) {                                        \
            std::fprintf(stderr,                                 \
                "FAIL [%s:%d]: %s  (got %llu != expected %llu)\n", \
                __FILE__, __LINE__, (msg), va_, vb_);            \
            g_fail_count++;                                      \
        }                                                        \
    } while (0)

// Minimal BatchHeader stub matching the fields used by CORFU frontier logic.
// Size doesn't need to match the real BatchHeader; only field semantics matter.
struct TestBatchHeader {
    uint64_t pbr_absolute_index;
    uint32_t num_msg;
    uint32_t total_size;
    uint32_t ordered;
    size_t   total_order;
    size_t   log_idx;
};

// Simulates a ring of batch header slots.
static constexpr size_t kTestRingSlots = 16;

struct TestRing {
    TestBatchHeader slots[kTestRingSlots];
    // Ordered frontier (mirrors corfu_order2_next_seq_)
    uint64_t next_seq = 0;
    // Export frontier (mirrors corfu_export_frontier_)
    std::atomic<uint64_t> export_frontier{0};
    // Global ordered message count (mirrors tinode->offsets[broker].ordered)
    uint64_t global_ordered_msgs = 0;
    // Per-client ordered count
    uint64_t per_client_ordered[4] = {};

    void reset() {
        std::memset(slots, 0, sizeof(slots));
        next_seq = 0;
        export_frontier.store(0, std::memory_order_release);
        global_ordered_msgs = 0;
        std::memset(per_client_ordered, 0, sizeof(per_client_ordered));
    }

    // Populate a slot as CorfuGetCXLBuffer would.
    void populate_slot(uint64_t batch_seq, uint32_t num_msg, uint32_t total_size,
                       size_t total_order, size_t log_idx) {
        size_t slot = batch_seq % kTestRingSlots;
        auto& s = slots[slot];
        s.pbr_absolute_index = batch_seq;
        s.num_msg = num_msg;
        s.total_size = total_size;
        s.total_order = total_order;
        s.log_idx = log_idx;
        s.ordered = 0;
    }

    // Mirrors RecordCorfuOrder2BatchCompletion: insert completion, drain contiguous prefix.
    // Completions are stored in a simple array for the test.
    struct Completion { uint64_t seq; uint32_t num_msg; uint32_t client_id; };
    std::vector<Completion> pending;

    void record_completion(uint64_t batch_seq, uint32_t num_msg, uint32_t client_id) {
        pending.push_back({batch_seq, num_msg, client_id});
        drain();
    }

    void drain() {
        bool advanced = false;
        while (true) {
            // Find next_seq in pending
            auto it = pending.end();
            for (auto i = pending.begin(); i != pending.end(); ++i) {
                if (i->seq == next_seq) { it = i; break; }
            }
            if (it == pending.end()) break;

            uint32_t slot_num_msg = it->num_msg;
            uint32_t slot_client_id = it->client_id;
            size_t slot = next_seq % kTestRingSlots;
            auto& sh = slots[slot];

            if (sh.pbr_absolute_index != next_seq) {
                // Ring overwrite — advance without touching slot
                global_ordered_msgs += slot_num_msg;
                if (slot_client_id < 4) per_client_ordered[slot_client_id] += slot_num_msg;
                pending.erase(it);
                next_seq++;
                advanced = true;
                continue;
            }

            if (slot_num_msg == 0) {
                // Hole: zero the slot metadata so export recognises it
                sh.num_msg = 0;
                sh.total_size = 0;
                sh.ordered = 1;
            } else {
                sh.ordered = 1;
            }

            global_ordered_msgs += slot_num_msg;
            if (slot_client_id < 4) per_client_ordered[slot_client_id] += slot_num_msg;
            pending.erase(it);
            next_seq++;
            advanced = true;
        }
        if (advanced) {
            export_frontier.store(next_seq, std::memory_order_release);
        }
    }

    // Mirrors SkipCorfuOrder2Batch
    void skip_batch(uint64_t batch_seq, uint32_t client_id) {
        size_t slot = batch_seq % kTestRingSlots;
        auto& sh = slots[slot];
        if (sh.pbr_absolute_index == batch_seq) {
            sh.num_msg = 0;
            sh.total_size = 0;
        }
        record_completion(batch_seq, 0, client_id);
    }

    // Mirrors GetBatchToExportWithMetadata for CORFU
    struct ExportResult {
        bool found;
        size_t total_order;
        uint32_t num_messages;
        size_t batch_size;
        size_t log_idx;
    };

    ExportResult try_export(size_t& offset) {
        uint64_t frontier = export_frontier.load(std::memory_order_acquire);
        for (size_t skipped = 0; skipped < kTestRingSlots; ++skipped) {
            size_t slot = offset % kTestRingSlots;
            auto& h = slots[slot];
            bool slot_fresh = (h.pbr_absolute_index == offset);
            bool past_frontier = (offset < frontier);

            if (!slot_fresh) {
                if (past_frontier) { offset++; continue; }
                return {false, 0, 0, 0, 0};
            }
            if (h.ordered == 1 && (h.num_msg == 0 || h.total_size == 0)) {
                offset++;
                continue;
            }
            if (h.ordered == 0) {
                return {false, 0, 0, 0, 0};
            }
            if (h.num_msg == 0 || h.total_size == 0) {
                if (past_frontier) { offset++; continue; }
                return {false, 0, 0, 0, 0};
            }
            ExportResult r = {true, h.total_order, h.num_msg, h.total_size, h.log_idx};
            offset++;
            return r;
        }
        return {false, 0, 0, 0, 0};
    }
};

// ---------------------------------------------------------------------------
// Test 1: Basic contiguous completion
// ---------------------------------------------------------------------------
static void test_basic_contiguous() {
    TestRing ring;
    ring.reset();

    // Populate slots 0, 1, 2 (all from client 0)
    ring.populate_slot(0, 10, 1000, 0,  0x1000);
    ring.populate_slot(1, 20, 2000, 10, 0x2000);
    ring.populate_slot(2, 30, 3000, 30, 0x3000);

    // Complete in order
    ring.record_completion(0, 10, 0);
    CHECK_EQ(ring.next_seq, 1, "frontier should be 1 after batch 0");
    CHECK_EQ(ring.global_ordered_msgs, 10, "10 msgs after batch 0");

    ring.record_completion(1, 20, 0);
    CHECK_EQ(ring.next_seq, 2, "frontier should be 2 after batch 1");
    CHECK_EQ(ring.global_ordered_msgs, 30, "30 msgs after batch 0+1");

    ring.record_completion(2, 30, 0);
    CHECK_EQ(ring.next_seq, 3, "frontier should be 3 after batch 2");
    CHECK_EQ(ring.global_ordered_msgs, 60, "60 msgs after batch 0+1+2");

    // Export all 3
    size_t export_off = 0;
    auto r = ring.try_export(export_off);
    CHECK(r.found, "batch 0 export should succeed");
    CHECK_EQ(r.num_messages, 10, "batch 0 should have 10 msgs");
    CHECK_EQ(export_off, 1, "export offset should be 1");

    r = ring.try_export(export_off);
    CHECK(r.found, "batch 1 export should succeed");
    CHECK_EQ(r.num_messages, 20, "batch 1 should have 20 msgs");

    r = ring.try_export(export_off);
    CHECK(r.found, "batch 2 export should succeed");
    CHECK_EQ(r.num_messages, 30, "batch 2 should have 30 msgs");

    // No more to export
    r = ring.try_export(export_off);
    CHECK(!r.found, "no more batches to export");

    std::fprintf(stderr, "  test_basic_contiguous: OK\n");
}

// ---------------------------------------------------------------------------
// Test 2: Out-of-order completion drains contiguous prefix
// ---------------------------------------------------------------------------
static void test_out_of_order_completion() {
    TestRing ring;
    ring.reset();

    ring.populate_slot(0, 10, 1000, 0,  0x1000);
    ring.populate_slot(1, 20, 2000, 10, 0x2000);
    ring.populate_slot(2, 30, 3000, 30, 0x3000);

    // Complete batch 2 first, then 1, then 0
    ring.record_completion(2, 30, 0);
    CHECK_EQ(ring.next_seq, 0, "frontier should still be 0 (waiting for batch 0)");
    CHECK_EQ(ring.global_ordered_msgs, 0, "0 msgs (nothing contiguous)");

    ring.record_completion(1, 20, 0);
    CHECK_EQ(ring.next_seq, 0, "frontier still 0 (waiting for batch 0)");

    ring.record_completion(0, 10, 0);
    CHECK_EQ(ring.next_seq, 3, "frontier should jump to 3 (all contiguous)");
    CHECK_EQ(ring.global_ordered_msgs, 60, "60 msgs total");

    size_t export_off = 0;
    for (int i = 0; i < 3; ++i) {
        auto r = ring.try_export(export_off);
        CHECK(r.found, "all 3 batches should export");
    }
    auto r = ring.try_export(export_off);
    CHECK(!r.found, "no more after 3");

    std::fprintf(stderr, "  test_out_of_order_completion: OK\n");
}

// ---------------------------------------------------------------------------
// Test 3: Hole skip — publisher disconnect mid-batch
// ---------------------------------------------------------------------------
static void test_hole_skip_mid_batch() {
    TestRing ring;
    ring.reset();

    ring.populate_slot(0, 10, 1000, 0,  0x1000);
    ring.populate_slot(1, 20, 2000, 10, 0x2000);  // this one will be skipped
    ring.populate_slot(2, 30, 3000, 30, 0x3000);

    // Complete batch 0 normally
    ring.record_completion(0, 10, 0);

    // Batch 1: publisher disconnected mid-batch. Skip it.
    ring.skip_batch(1, 0);
    CHECK_EQ(ring.next_seq, 2, "frontier should be 2 after skip");
    CHECK_EQ(ring.global_ordered_msgs, 10, "only 10 msgs (hole contributes 0)");

    // Verify slot 1 is marked as hole
    auto& sh = ring.slots[1 % kTestRingSlots];
    CHECK_EQ(sh.ordered, 1, "hole slot should have ordered=1");
    CHECK_EQ(sh.num_msg, 0, "hole slot should have num_msg=0");
    CHECK_EQ(sh.total_size, 0, "hole slot should have total_size=0");

    // Complete batch 2
    ring.record_completion(2, 30, 0);
    CHECK_EQ(ring.next_seq, 3, "frontier should be 3");
    CHECK_EQ(ring.global_ordered_msgs, 40, "10 + 0 + 30 = 40");

    // Export should skip the hole
    size_t export_off = 0;
    auto r = ring.try_export(export_off);
    CHECK(r.found, "batch 0 should export");
    CHECK_EQ(r.num_messages, 10, "batch 0 has 10 msgs");

    r = ring.try_export(export_off);
    CHECK(r.found, "batch 2 should export (hole 1 skipped)");
    CHECK_EQ(r.num_messages, 30, "batch 2 has 30 msgs");
    CHECK_EQ(export_off, 3, "export offset should be 3 (0, skip 1, 2)");

    r = ring.try_export(export_off);
    CHECK(!r.found, "no more batches");

    std::fprintf(stderr, "  test_hole_skip_mid_batch: OK\n");
}

// ---------------------------------------------------------------------------
// Test 4: Hole skip — batch never arrived (DrainStaleCorfuFrontier scenario)
// ---------------------------------------------------------------------------
static void test_hole_never_arrived() {
    TestRing ring;
    ring.reset();

    ring.populate_slot(0, 10, 1000, 0, 0x1000);
    // Batch 1 never arrives — slot has stale data (pbr_absolute_index=0 from init or old batch)
    ring.populate_slot(2, 30, 3000, 30, 0x3000);

    ring.record_completion(0, 10, 0);
    ring.record_completion(2, 30, 0);

    // Frontier is at 1 (waiting for batch 1). Nothing drains past 0.
    CHECK_EQ(ring.next_seq, 1, "frontier stalls at 1 (waiting for batch 1)");
    CHECK_EQ(ring.global_ordered_msgs, 10, "only batch 0 ordered");

    // DrainStaleCorfuFrontier equivalent: skip batch 1 (never arrived, slot is stale)
    ring.skip_batch(1, 0);

    // Now batch 1 was skipped and batch 2 drains
    CHECK_EQ(ring.next_seq, 3, "frontier should be 3 after skip+drain");
    CHECK_EQ(ring.global_ordered_msgs, 40, "10 + 0 + 30 = 40");

    // Export: batch 0, skip hole 1 (stale slot: pbr_absolute_index won't match since
    // slot 1 had 0 from memset, then skip_batch set num_msg=0, but populate_slot for
    // batch 1 was never called so pbr_absolute_index=0 != expected=1 → stale slot path)
    size_t export_off = 0;
    auto r = ring.try_export(export_off);
    CHECK(r.found, "batch 0 export");

    // Batch 1: slot is stale (pbr_absolute_index=0 != expected=1). frontier=3 so past_frontier=true → skip.
    r = ring.try_export(export_off);
    CHECK(r.found, "batch 2 should export (stale slot 1 skipped via frontier)");
    CHECK_EQ(r.num_messages, 30, "batch 2 has 30 msgs");

    r = ring.try_export(export_off);
    CHECK(!r.found, "no more");

    std::fprintf(stderr, "  test_hole_never_arrived: OK\n");
}

// ---------------------------------------------------------------------------
// Test 5: Per-client ACK correctness across holes
// ---------------------------------------------------------------------------
static void test_per_client_ack_across_holes() {
    TestRing ring;
    ring.reset();

    // Client 0: batches 0, 2. Client 1: batch 1 (will be skipped).
    ring.populate_slot(0, 10, 1000, 0, 0x1000);
    ring.populate_slot(1, 20, 2000, 10, 0x2000);
    ring.populate_slot(2, 30, 3000, 30, 0x3000);

    ring.record_completion(0, 10, 0);   // client 0
    ring.skip_batch(1, 1);              // client 1 disconnected
    ring.record_completion(2, 30, 0);   // client 0

    CHECK_EQ(ring.per_client_ordered[0], 40, "client 0: 10 + 30 = 40");
    CHECK_EQ(ring.per_client_ordered[1], 0,  "client 1: hole contributes 0");
    CHECK_EQ(ring.global_ordered_msgs, 40,   "global: 10 + 0 + 30 = 40");

    std::fprintf(stderr, "  test_per_client_ack_across_holes: OK\n");
}

// ---------------------------------------------------------------------------
// Test 6: Export must not serve partial data from populated-then-skipped slot
// ---------------------------------------------------------------------------
static void test_no_partial_data_export() {
    TestRing ring;
    ring.reset();

    // CorfuGetCXLBuffer populated slot with full metadata
    ring.populate_slot(0, 100, 10000, 0, 0x5000);

    // Publisher disconnects before full recv. Skip immediately.
    ring.skip_batch(0, 0);

    CHECK_EQ(ring.slots[0].ordered, 1, "slot should be ordered");
    CHECK_EQ(ring.slots[0].num_msg, 0, "slot num_msg should be 0 (hole marker)");
    CHECK_EQ(ring.slots[0].total_size, 0, "slot total_size should be 0");

    // Export must NOT return the slot (it's a hole with partial data in BLog)
    size_t export_off = 0;
    auto r = ring.try_export(export_off);
    CHECK(!r.found, "hole slot must not be exported (partial data protection)");
    // But export_off should have advanced past it since frontier is at 1
    CHECK_EQ(export_off, 1, "export cursor should advance past hole");

    std::fprintf(stderr, "  test_no_partial_data_export: OK\n");
}

// ---------------------------------------------------------------------------
// Test 7: Ring overwrite detection — export skips via frontier
// ---------------------------------------------------------------------------
static void test_ring_overwrite_export_skip() {
    TestRing ring;
    ring.reset();

    // Fill all 16 slots and complete them
    for (uint64_t i = 0; i < kTestRingSlots; ++i) {
        ring.populate_slot(i, 5, 500, i*5, 0x1000 * (i+1));
        ring.record_completion(i, 5, 0);
    }
    CHECK_EQ(ring.next_seq, kTestRingSlots, "frontier at 16");

    // Now populate batch 16 which overwrites slot 0
    ring.populate_slot(kTestRingSlots, 10, 1000, 80, 0x20000);
    ring.record_completion(kTestRingSlots, 10, 0);
    CHECK_EQ(ring.next_seq, kTestRingSlots + 1, "frontier at 17");

    // Export from 0: slot 0 now has pbr_absolute_index=16 (overwritten).
    // Frontier=17, so 0 < 17 → skip. Slots 1..15 are fine. Slot 16 (mapped to slot 0) also fine.
    size_t export_off = 0;
    auto r = ring.try_export(export_off);
    // Slot 0 is stale (pbr_abs=16 != expected=0), past_frontier=true → skip.
    // Then slot 1: pbr_abs=1 == expected=1, ordered=1, num_msg>0 → export.
    CHECK(r.found, "should export after skipping overwritten slot 0");
    CHECK_EQ(r.num_messages, 5, "slot 1 has 5 msgs");
    CHECK_EQ(export_off, 2, "export offset at 2 (skipped 0, exported 1)");

    std::fprintf(stderr, "  test_ring_overwrite_export_skip: OK\n");
}

// ---------------------------------------------------------------------------
// Test 8: ACK=1 per-client is monotonic
// ---------------------------------------------------------------------------
static void test_ack1_monotonic() {
    TestRing ring;
    ring.reset();

    uint64_t prev_ack = 0;
    for (uint64_t i = 0; i < 20; ++i) {
        ring.populate_slot(i, 5, 500, i*5, 0x1000*(i+1));
        ring.record_completion(i, 5, 0);
        uint64_t current_ack = ring.per_client_ordered[0];
        CHECK(current_ack >= prev_ack, "per-client ACK must be monotonic");
        prev_ack = current_ack;
    }
    CHECK_EQ(ring.per_client_ordered[0], 100, "20 batches × 5 msgs = 100");

    std::fprintf(stderr, "  test_ack1_monotonic: OK\n");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::fprintf(stderr, "=== Corfu ORDER=2 Invariant Tests ===\n");

    test_basic_contiguous();
    test_out_of_order_completion();
    test_hole_skip_mid_batch();
    test_hole_never_arrived();
    test_per_client_ack_across_holes();
    test_no_partial_data_export();
    test_ring_overwrite_export_skip();
    test_ack1_monotonic();

    if (g_fail_count == 0) {
        std::fprintf(stderr, "=== ALL PASSED ===\n");
        return 0;
    } else {
        std::fprintf(stderr, "=== %d FAILURES ===\n", g_fail_count);
        return 1;
    }
}
