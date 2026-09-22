// Fuzz/property test for Subscriber's ORDER=5 multi-connection total-order
// reconstruction (subscriber.cc: ParseAndStageOrderedBytes / StageOrderedMessages /
// TryPopOrderedMessageLocked).
//
// Context: intermittent live-cluster stalls (docs/experiments/YCSB_DISTRIBUTED_KV_PLAN.md
// Sec 6e) show every broker's sequencer/export path fully caught up (sum of per-broker
// `ordered` counters exactly equals the client's stuck target), while the client's
// applied count freezes forever one (or a few) total_order positions short. That is a
// permanent head-of-line block in the client's reorder buffer: some position is never
// filled. ORDER=5 never stamps per-message total_order on the wire (disabled at
// topic.cc ~8532 for performance); the client derives each message's position purely by
// counting within a batch, seeded from BatchMetadata.batch_total_order. This test
// stresses that derivation against adversarial recv()-chunk boundaries and interleaved,
// out-of-order arrival across multiple simulated broker connections -- without any
// cluster, CXL, or network -- to see whether it can lose or misplace a message on its
// own.
//
// Each synthetic message's payload embeds its intended global position as an 8-byte
// marker, so a bug that mis-derives a position shows up as a content mismatch, not just
// a missing message.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <future>
#include <numeric>
#include <random>
#include <vector>

#include "../src/client/subscriber.h"
#include "../src/client/delivery_completion.h"
#include "../benchmarks/kv_store/distributed_kv_store.h"

struct DistributedKVStoreTestPeer {
    using DeliveryFailure = DistributedKVStore::DeliveryFailure;
};
#include "../src/cxl_manager/cxl_datastructure.h"
#include "../src/common/order_level.h"
#include "../src/common/wire_formats.h"

namespace {

constexpr size_t kHeaderSize = sizeof(Embarcadero::BlogMessageHeader);

// Appends one BatchMetadata + `count` BlogMessageHeader-framed messages, starting at
// global position `start_order`. Message i's payload's first 8 bytes = start_order + i
// (the marker the test verifies on pop). total_order in the header is left 0, matching
// real ORDER=5 traffic (never stamped -- see topic.cc AssignOrder5, "DISABLED CODE").
void AppendBatch(std::vector<uint8_t>* buf, size_t start_order, uint32_t count,
                  size_t payload_size, std::mt19937_64& rng, uint16_t flags = 0) {
	Embarcadero::wire::BatchMetadata meta{};
	meta.batch_total_order = start_order;
	meta.num_messages = count;
	meta.header_version = Embarcadero::wire::HEADER_VERSION_V2;
	meta.flags = flags;
	const uint8_t* meta_bytes = reinterpret_cast<const uint8_t*>(&meta);
	buf->insert(buf->end(), meta_bytes, meta_bytes + sizeof(meta));

	for (uint32_t i = 0; i < count; ++i) {
		Embarcadero::BlogMessageHeader hdr{};
		hdr.size = static_cast<uint32_t>(payload_size);
		hdr.total_order = 0;  // never stamped for ORDER=5; client derives it
		const uint8_t* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
		buf->insert(buf->end(), hdr_bytes, hdr_bytes + sizeof(hdr));

		std::vector<uint8_t> payload(payload_size, 0);
		if (payload_size >= sizeof(uint64_t)) {
			const uint64_t marker = start_order + i;
			std::memcpy(payload.data(), &marker, sizeof(marker));
		}
		for (size_t b = sizeof(uint64_t); b < payload_size; ++b) {
			payload[b] = static_cast<uint8_t>(rng());
		}
		buf->insert(buf->end(), payload.begin(), payload.end());
		const size_t stride = Embarcadero::wire::ComputeStrideV2(payload_size);
		const size_t written = sizeof(hdr) + payload_size;
		if (stride > written) {
			buf->insert(buf->end(), stride - written, 0);
		}
	}
}

// Builds a full stream of `total_messages` starting at `start_order`, split into
// batches of `batch_size` (last batch may be smaller), with the given payload size.
std::vector<uint8_t> BuildStream(size_t start_order, size_t total_messages,
                                  uint32_t batch_size, size_t payload_size,
                                  std::mt19937_64& rng) {
	std::vector<uint8_t> buf;
	size_t remaining = total_messages;
	size_t cursor = start_order;
	while (remaining > 0) {
		const uint32_t count = static_cast<uint32_t>(std::min<size_t>(batch_size, remaining));
		AppendBatch(&buf, cursor, count, payload_size, rng);
		cursor += count;
		remaining -= count;
	}
	return buf;
}

}  // namespace

struct SubscriberTestPeer {
	using StreamParseState = Subscriber::StreamParseState;

	static void ParseChunk(Subscriber& sub, StreamParseState& state, const uint8_t* data,
	                        size_t len) {
		sub.ParseAndStageOrderedBytes(state, data, len, nullptr,
		                               std::chrono::steady_clock::now(), 0, len);
	}

	// Pops one in-order message if ready; returns its 8-byte payload marker.
	static bool TryPopMarker(Subscriber& sub, uint64_t* out_marker) {
		absl::MutexLock lock(&sub.consume_mutex_);
		void* ptr = sub.TryPopOrderedMessageLocked();
		if (!ptr) return false;
		std::memcpy(out_marker, static_cast<uint8_t*>(ptr) + kHeaderSize, sizeof(*out_marker));
		return true;
	}

	static size_t NextExpectedOrder(Subscriber& sub) {
		absl::MutexLock lock(&sub.consume_mutex_);
		return sub.next_expected_order_;
	}

	static size_t PendingBufferedSlots(Subscriber& sub) {
		absl::MutexLock lock(&sub.consume_mutex_);
		return sub.pending_messages_.size();
	}

	// Resets reorder-buffer state so a single long-lived Subscriber instance (thread
	// spawn is not free) can be reused across many independent fuzz iterations.
	static void ResetOrderState(Subscriber& sub) {
		absl::MutexLock lock(&sub.consume_mutex_);
		sub.next_expected_order_ = 0;
		sub.pending_messages_base_order_ = 0;
		sub.pending_messages_.clear();
		sub.last_returned_.reset();
		sub.last_returned_batch_.clear();
        sub.ordered_received_messages_ = 0;
        sub.ordered_duplicate_messages_ = 0;
        sub.ordered_parse_errors_.store(0);
        sub.ordered_export_gaps_reported_.store(0);
		sub.retention_exhausted_.store(false);
	}
	static size_t AllocatedMessages(Subscriber& sub) {
		absl::MutexLock lock(&sub.owned_message_pool_mutex_);
		return sub.allocated_owned_messages_;
	}
};

namespace {

// Drains everything currently poppable and returns the markers in pop order.
std::vector<uint64_t> DrainAvailable(Subscriber& sub) {
	std::vector<uint64_t> out;
	uint64_t marker = 0;
	while (SubscriberTestPeer::TryPopMarker(sub, &marker)) {
		out.push_back(marker);
	}
	return out;
}

// Splits `data` into pieces at the given offsets (sorted, within bounds) and feeds each
// piece through ParseChunk in order, using one StreamParseState (one simulated
// connection).
void FeedSplit(Subscriber& sub, SubscriberTestPeer::StreamParseState* state,
                const std::vector<uint8_t>& data, const std::vector<size_t>& cut_points) {
	size_t prev = 0;
	for (size_t cut : cut_points) {
		if (cut <= prev || cut > data.size()) continue;
		SubscriberTestPeer::ParseChunk(sub, *state, data.data() + prev, cut - prev);
		prev = cut;
	}
	if (prev < data.size()) {
		SubscriberTestPeer::ParseChunk(sub, *state, data.data() + prev, data.size() - prev);
	}
}

Subscriber& SharedSubscriber() {
	// One instance for the whole binary: construction spawns a background gRPC probe
	// thread against an address nothing listens on (retries harmlessly until
	// destruction); real work here is pure in-process parsing, so sharing avoids
	// thread-spawn churn across hundreds of fuzz iterations.
	static char topic[TOPIC_NAME_SIZE] = "ReorderFuzzTopic";
	static Subscriber sub("127.0.0.1", "1", topic, /*measure_latency=*/false,
	                      Embarcadero::kOrderStrong);
	return sub;
}

std::vector<uint64_t> ExpectedSequence(size_t n) {
	std::vector<uint64_t> expected(n);
	std::iota(expected.begin(), expected.end(), 0);
	return expected;
}

}  // namespace

TEST(Order5SubscriberReorder, SingleConnection_ExhaustiveByteSplits) {
	Subscriber& sub = SharedSubscriber();
	std::mt19937_64 rng(1);
	constexpr size_t kTotal = 9;
	constexpr size_t kPayload = 16;
	auto stream = BuildStream(/*start_order=*/0, kTotal, /*batch_size=*/3, kPayload, rng);

	// Every single split point, plus the fully-atomic (no split) and maximally-shattered
	// (one byte per call) extremes.
	std::vector<std::vector<size_t>> split_patterns;
	split_patterns.push_back({});  // no split: one call with the whole buffer
	for (size_t k = 1; k < stream.size(); ++k) {
		split_patterns.push_back({k});
	}
	{
		std::vector<size_t> all_bytes;
		for (size_t k = 1; k < stream.size(); ++k) all_bytes.push_back(k);
		split_patterns.push_back(all_bytes);
	}

	for (size_t p = 0; p < split_patterns.size(); ++p) {
		SubscriberTestPeer::ResetOrderState(sub);
		SubscriberTestPeer::StreamParseState state{};
		FeedSplit(sub, &state, stream, split_patterns[p]);
		auto delivered = DrainAvailable(sub);
		ASSERT_EQ(delivered, ExpectedSequence(kTotal))
			<< "split pattern #" << p << " (cut points: "
			<< ::testing::PrintToString(split_patterns[p]) << ")";
		EXPECT_EQ(SubscriberTestPeer::PendingBufferedSlots(sub), 0u);
	}
}

TEST(Order5SubscriberReorder, SingleConnection_RandomFineGrainedSplits) {
	Subscriber& sub = SharedSubscriber();
	constexpr size_t kTotal = 60;
	constexpr int kIterations = 300;

	for (int iter = 0; iter < kIterations; ++iter) {
		std::mt19937_64 rng(static_cast<uint64_t>(iter) * 2654435761u + 1);
		// Payload sizes deliberately straddle the 64-byte stride-alignment boundary
		// (0, small, exactly-aligning, and larger) since that's where a stride/pos
		// miscalculation would first show up.
		const size_t payload_sizes[] = {8, 56, 64, 100, 127};
		const size_t payload_size = payload_sizes[rng() % 5];
		const uint32_t batch_size = static_cast<uint32_t>(1 + rng() % 7);
		auto stream = BuildStream(0, kTotal, batch_size, payload_size, rng);

		std::vector<size_t> cuts;
		size_t pos = 0;
		while (pos < stream.size()) {
			const size_t piece = 1 + rng() % 5;  // 1-5 byte fragments: maximally adversarial
			pos = std::min(stream.size(), pos + piece);
			if (pos < stream.size()) cuts.push_back(pos);
		}

		SubscriberTestPeer::ResetOrderState(sub);
		SubscriberTestPeer::StreamParseState state{};
		FeedSplit(sub, &state, stream, cuts);
		auto delivered = DrainAvailable(sub);
		ASSERT_EQ(delivered, ExpectedSequence(kTotal))
			<< "iter=" << iter << " payload_size=" << payload_size
			<< " batch_size=" << batch_size;
		EXPECT_EQ(SubscriberTestPeer::PendingBufferedSlots(sub), 0u);
	}
}

TEST(Order5SubscriberReorder, MultiConnectionMerge_InterleavedArrival) {
	Subscriber& sub = SharedSubscriber();
	constexpr size_t kTotal = 200;
	constexpr int kConnections = 4;
	constexpr int kIterations = 150;

	for (int iter = 0; iter < kIterations; ++iter) {
		std::mt19937_64 rng(static_cast<uint64_t>(iter) * 6364136223846793005ULL + 7);

		// Split the global sequence across kConnections contiguous ranges of random
		// size (mirrors round-robin-ish assignment across brokers without assuming
		// the real system's exact striping -- the merge logic must not care).
		std::vector<size_t> boundaries = {0};
		{
			std::vector<size_t> cuts;
			for (int c = 1; c < kConnections; ++c) {
				cuts.push_back(1 + rng() % (kTotal - 1));
			}
			std::sort(cuts.begin(), cuts.end());
			cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
			boundaries.insert(boundaries.end(), cuts.begin(), cuts.end());
		}
		boundaries.push_back(kTotal);

		const size_t payload_size = 100;
		std::vector<std::vector<uint8_t>> conn_streams;
		std::vector<SubscriberTestPeer::StreamParseState> conn_states(boundaries.size() - 1);
		for (size_t c = 0; c + 1 < boundaries.size(); ++c) {
			const size_t start = boundaries[c];
			const size_t count = boundaries[c + 1] - start;
			if (count == 0) {
				conn_streams.emplace_back();
				continue;
			}
			const uint32_t batch_size = static_cast<uint32_t>(1 + rng() % 11);
			conn_streams.push_back(BuildStream(start, count, batch_size, payload_size, rng));
		}

		// Feed all connections' streams in small, randomly interleaved fragments --
		// simulating independent sockets whose bytes arrive in an arbitrary relative
		// order, including "later" positions' bytes landing before "earlier" ones'.
		SubscriberTestPeer::ResetOrderState(sub);
		std::vector<size_t> conn_pos(conn_streams.size(), 0);
		size_t active = conn_streams.size();
		while (active > 0) {
			size_t c = rng() % conn_streams.size();
			if (conn_pos[c] >= conn_streams[c].size()) continue;
			const size_t remaining = conn_streams[c].size() - conn_pos[c];
			const size_t piece = std::min(remaining, static_cast<size_t>(1 + rng() % 9));
			SubscriberTestPeer::ParseChunk(sub, conn_states[c],
			                               conn_streams[c].data() + conn_pos[c], piece);
			conn_pos[c] += piece;
			if (conn_pos[c] >= conn_streams[c].size()) active--;
		}

		auto delivered = DrainAvailable(sub);
		ASSERT_EQ(delivered, ExpectedSequence(kTotal))
			<< "iter=" << iter << " boundaries="
			<< ::testing::PrintToString(boundaries);
		EXPECT_EQ(SubscriberTestPeer::PendingBufferedSlots(sub), 0u);
	}
}

// Reproduces the mechanism behind docs/experiments/YCSB_DISTRIBUTED_KV_PLAN.md Sec 6e:
// a broker reports BATCH_META_FLAG_EXPORT_GAP (network_manager.cc SubscribeNetworkThread /
// topic.cc GetBatchToExportWithMetadata's ORDER5_EXPORT_OVERRUN path) when a lagging
// subscriber connection's export cursor falls behind far enough that the broker's ring
// already overwrote the data it needed -- the broker skips forward and flags the first
// post-gap batch so the gap is reported, not silent. Subscriber::ProcessSequencer5Data
// (subscriber.cc ~2153) DOES read this flag and re-anchor, but that function only runs
// under EMBAR_VALIDATE_ORDER -- a diagnostic-only path. The actual runtime consume path
// (ParseAndStageOrderedBytes / StageOrderedMessages, feed_ordered_consume_stream in
// ReceiveWorkerThread) never references wire::BATCH_META_FLAG_EXPORT_GAP at all: a gap
// batch just gets buffered at its (now unreachable) total_order, and next_expected_order_
// waits forever for positions the broker has already discarded -- permanent head-of-line
// block, broker-side counters unaffected, exactly matching the live-captured symptom.
TEST(Order5SubscriberReorder, ExportGapMustReanchorNotWedgeForever) {
	Subscriber& sub = SharedSubscriber();
	std::mt19937_64 rng(42);
	constexpr size_t kBeforeGap = 10;
	constexpr size_t kGapSize = 50;    // positions skipped: [kBeforeGap, kBeforeGap+kGapSize)
	constexpr size_t kAfterGap = 10;
	constexpr size_t kPayload = 32;

	SubscriberTestPeer::ResetOrderState(sub);
	SubscriberTestPeer::StreamParseState state{};

	auto pre_gap = BuildStream(0, kBeforeGap, /*batch_size=*/5, kPayload, rng);
	SubscriberTestPeer::ParseChunk(sub, state, pre_gap.data(), pre_gap.size());

	// Deliverable up to the gap: this part must always work regardless of the bug below.
	auto delivered_before = DrainAvailable(sub);
	ASSERT_EQ(delivered_before, ExpectedSequence(kBeforeGap));

	// The post-gap batch: broker resumes at kBeforeGap+kGapSize and flags it.
	std::vector<uint8_t> gap_batch;
	AppendBatch(&gap_batch, kBeforeGap + kGapSize, static_cast<uint32_t>(kAfterGap), kPayload,
	            rng, Embarcadero::wire::BATCH_META_FLAG_EXPORT_GAP);
	SubscriberTestPeer::ParseChunk(sub, state, gap_batch.data(), gap_batch.size());

	auto delivered_after = DrainAvailable(sub);
	// Correct behavior: re-anchor past the (unrecoverable, already-overwritten) gap and
	// keep delivering. Buggy current behavior: delivered_after is empty forever --
	// next_expected_order_ is stuck at kBeforeGap, waiting for positions the broker will
	// never send again, while kAfterGap messages sit unreachable in pending_messages_.
	std::vector<uint64_t> expected_after(kAfterGap);
	std::iota(expected_after.begin(), expected_after.end(),
	          static_cast<uint64_t>(kBeforeGap + kGapSize));
	EXPECT_EQ(delivered_after, expected_after)
		<< "next_expected_order_=" << SubscriberTestPeer::NextExpectedOrder(sub)
		<< " (stuck at pre-gap position -- gap was never re-anchored) "
		<< "pending_buffered_slots=" << SubscriberTestPeer::PendingBufferedSlots(sub);
}

// The re-anchor must not clobber data a DIFFERENT (faster) connection already buffered
// ahead of the gap. Simulates: connection C's later-position data races ahead and is
// already sitting in the shared pending_messages_ deque; connection B's gap-flagged
// batch (reporting positions between the still-unfulfilled prefix and C's data) then
// arrives. The jump must re-base the deque, not clear() it -- C's already-received data
// must still be delivered afterward, not silently dropped alongside the genuinely lost
// pre-gap range.
TEST(Order5SubscriberReorder, ExportGapReanchorPreservesAlreadyBufferedFutureData) {
	Subscriber& sub = SharedSubscriber();
	std::mt19937_64 rng(7);
	// next_expected_order_ starts at 0 (reset below); positions [0, kGapStart) are the
	// permanently-lost range the gap batch reports skipping. kFutureStart is deliberately
	// contiguous with the gap batch's end so the only intentional hole in this scenario
	// is the reported one -- a second, unflagged hole would legitimately block delivery
	// on its own and isn't what this test is checking.
	constexpr size_t kGapStart = 20;     // gap-flagged batch resumes here
	constexpr size_t kGapCount = 10;     // positions 20-29
	constexpr size_t kFutureStart = kGapStart + kGapCount;  // 30: connection C's data
	constexpr size_t kFutureCount = 10;  // positions 30-39
	constexpr size_t kPayload = 24;

	SubscriberTestPeer::ResetOrderState(sub);
	SubscriberTestPeer::StreamParseState state_c{};
	SubscriberTestPeer::StreamParseState state_b{};

	// Connection C's future data arrives FIRST, racing ahead while next_expected_order_
	// is still stuck at 0 waiting on connection B's not-yet-arrived (and, unknown to the
	// client yet, permanently gapped) prefix.
	auto future_stream = BuildStream(kFutureStart, kFutureCount, /*batch_size=*/5, kPayload, rng);
	SubscriberTestPeer::ParseChunk(sub, state_c, future_stream.data(), future_stream.size());
	ASSERT_TRUE(DrainAvailable(sub).empty()) << "nothing should be deliverable yet (position 0 still missing)";

	// Connection B's gap-flagged batch: broker skipped [kBeforeGap, kGapStart) and resumes
	// at kGapStart.
	std::vector<uint8_t> gap_batch;
	AppendBatch(&gap_batch, kGapStart, static_cast<uint32_t>(kGapCount), kPayload, rng,
	            Embarcadero::wire::BATCH_META_FLAG_EXPORT_GAP);
	SubscriberTestPeer::ParseChunk(sub, state_b, gap_batch.data(), gap_batch.size());

	auto delivered = DrainAvailable(sub);
	std::vector<uint64_t> expected;
	for (uint64_t v = kGapStart; v < kGapStart + kGapCount; ++v) expected.push_back(v);
	for (uint64_t v = kFutureStart; v < kFutureStart + kFutureCount; ++v) expected.push_back(v);
	EXPECT_EQ(delivered, expected)
		<< "connection C's already-buffered future data must survive the re-anchor, "
		<< "not be discarded alongside the genuinely-lost pre-gap prefix";
}

namespace {
std::vector<uint8_t> AuditFrames(uint32_t count) {
    Embarcadero::wire::BatchMetadata meta{};
    meta.header_version = Embarcadero::wire::HEADER_VERSION_V2;
    meta.num_messages = count;
    constexpr size_t payload = 32;
    const size_t stride = Embarcadero::wire::ComputeStrideV2(payload);
    std::vector<uint8_t> bytes(sizeof(meta) + count * stride, 0);
    std::memcpy(bytes.data(), &meta, sizeof(meta));
    for (uint32_t i = 0; i < count; ++i) {
        Embarcadero::BlogMessageHeader header{};
        header.size = payload;
        auto* frame = bytes.data() + sizeof(meta) + i * stride;
        std::memcpy(frame, &header, sizeof(header));
        std::memset(frame + sizeof(header), 'a', payload);
    }
    return bytes;
}
}

TEST(Order5DeliveryAudit, ValidatesFragmentedProductionConsumerAndPayload) {
    auto& sub = SharedSubscriber();
    SubscriberTestPeer::ResetOrderState(sub);
    SubscriberTestPeer::StreamParseState state;
    const auto bytes = AuditFrames(4);
    for (size_t i = 0; i < bytes.size(); ++i)
        SubscriberTestPeer::ParseChunk(sub, state, bytes.data() + i, 1);
    const std::string payload(32, 'a');
    EXPECT_TRUE(sub.AuditOrderedDelivery(4, payload.data(), payload.size(), 100));
}

TEST(Order5DeliveryAudit, RejectsMissingMessageAndCorruptedPayload) {
    auto& sub = SharedSubscriber();
    const std::string payload(32, 'a');
    for (bool corruption : {false, true}) {
        SubscriberTestPeer::ResetOrderState(sub);
        SubscriberTestPeer::StreamParseState state;
        auto bytes = AuditFrames(1);
        if (corruption) bytes[sizeof(Embarcadero::wire::BatchMetadata) + kHeaderSize] = 'b';
        SubscriberTestPeer::ParseChunk(sub, state, bytes.data(), bytes.size());
        EXPECT_FALSE(sub.AuditOrderedDelivery(corruption ? 1 : 2, payload.data(), payload.size(), 30));
    }
}

TEST(Order5DeliveryAudit, RejectsDuplicateEvenWhenReorderBufferSuppressesIt) {
    auto& sub = SharedSubscriber();
    SubscriberTestPeer::ResetOrderState(sub);
    SubscriberTestPeer::StreamParseState state;
    const auto bytes = AuditFrames(1);
    SubscriberTestPeer::ParseChunk(sub, state, bytes.data(), bytes.size());
    SubscriberTestPeer::ParseChunk(sub, state, bytes.data(), bytes.size());
    const std::string payload(32, 'a');
    EXPECT_FALSE(sub.AuditOrderedDelivery(1, payload.data(), payload.size(), 100));
}

TEST(Order5DeliveryAudit, RejectsMalformedPrefixEvenWhenParserResynchronizes) {
    auto& sub = SharedSubscriber();
    SubscriberTestPeer::ResetOrderState(sub);
    SubscriberTestPeer::StreamParseState state;
    auto bytes = AuditFrames(1);
    bytes.insert(bytes.begin(), 8, 0xff);
    SubscriberTestPeer::ParseChunk(sub, state, bytes.data(), bytes.size());
    const std::string payload(32, 'a');
    EXPECT_FALSE(sub.AuditOrderedDelivery(1, payload.data(), payload.size(), 100));
}

TEST(Order5DeliveryAudit, IndexedPayloadDetectsReorderedIdenticalMessageBodies) {
    auto& sub = SharedSubscriber();
    const std::string payload(32, 'a');
    for (int variant = 0; variant < 3; ++variant) {
        SubscriberTestPeer::ResetOrderState(sub);
        SubscriberTestPeer::StreamParseState state;
        auto bytes = AuditFrames(2);
        const auto stride = Embarcadero::wire::ComputeStrideV2(payload.size());
        // Rest of both payloads is identical; only true message identity catches this.
        for (size_t i = 0; i < 2; ++i) {
            const uint64_t index = variant == 0 ? i : variant == 1 ? 1 - i : 0;
            std::memcpy(bytes.data() + sizeof(Embarcadero::wire::BatchMetadata) +
                        i * stride + kHeaderSize, &index, sizeof(index));
        }
        SubscriberTestPeer::ParseChunk(sub, state, bytes.data(), bytes.size());
        EXPECT_EQ(sub.AuditOrderedDelivery(2, payload.data(), payload.size(), 100, true), variant == 0);
    }
}

namespace {
std::unique_ptr<Subscriber> BoundedSubscriber(OrderedRetentionLimits limits) {
    char topic[TOPIC_NAME_SIZE] = "BoundedAudit";
    return std::make_unique<Subscriber>("127.0.0.1", "1", topic, false,
                                       Embarcadero::kOrderStrong, limits);
}

std::vector<uint8_t> IndexedAuditFrames(size_t start, uint32_t count) {
    auto bytes = AuditFrames(count);
    Embarcadero::wire::BatchMetadata meta{};
    std::memcpy(&meta, bytes.data(), sizeof(meta));
    meta.batch_total_order = start;
    std::memcpy(bytes.data(), &meta, sizeof(meta));
    const size_t stride = Embarcadero::wire::ComputeStrideV2(32);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t sequence = start + i;
        std::memcpy(bytes.data() + sizeof(meta) + i * stride + kHeaderSize,
                    &sequence, sizeof(sequence));
    }
    return bytes;
}
}

TEST(Order5Retention, MissingEarlierOrderFailsClosedAtByteBudgetWithoutWaiting) {
    const auto later = IndexedAuditFrames(1, 1);
    auto sub = BoundedSubscriber({later.size() * 2 - 1, 64});
    SubscriberTestPeer::StreamParseState first, second;
    SubscriberTestPeer::ParseChunk(*sub, first, later.data(), later.size());
    EXPECT_FALSE(sub->GetOrderedDeliveryStatus().retention_exhausted);
    const auto next = IndexedAuditFrames(2, 1);
    SubscriberTestPeer::ParseChunk(*sub, second, next.data(), next.size());
    auto status = sub->GetOrderedDeliveryStatus();
    EXPECT_TRUE(status.retention_exhausted);
    EXPECT_EQ(status.retained_bytes, later.size());
    EXPECT_LE(status.peak_retained_bytes, later.size() * 2 - 1);
    const std::string payload(32, 'a');
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(sub->AuditOrderedDelivery(3, payload.data(), payload.size(), 10000, true));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));
    // A subsequent earlier frame cannot resume a terminally failed stream.
    const auto missing = IndexedAuditFrames(0, 1);
    SubscriberTestPeer::ParseChunk(*sub, first, missing.data(), missing.size());
    EXPECT_EQ(sub->GetOrderedDeliveryStatus().parsed_messages, 1u);
}

TEST(Order5Retention, DescriptorPoolAndSparseReorderDistanceAreBoundedSeparately) {
    {
        auto sub = BoundedSubscriber({1 << 20, 1});
        SubscriberTestPeer::StreamParseState state;
        const auto bytes = IndexedAuditFrames(0, 2);
        SubscriberTestPeer::ParseChunk(*sub, state, bytes.data(), bytes.size());
        EXPECT_TRUE(sub->GetOrderedDeliveryStatus().retention_exhausted);
        EXPECT_LE(SubscriberTestPeer::AllocatedMessages(*sub), 1u);
        EXPECT_EQ(sub->GetOrderedDeliveryStatus().retained_bytes, 0u);
    }
    {
        auto sub = BoundedSubscriber({1 << 20, 8});
        SubscriberTestPeer::StreamParseState state;
        const auto distant = IndexedAuditFrames(1000000, 1);
        SubscriberTestPeer::ParseChunk(*sub, state, distant.data(), distant.size());
        EXPECT_TRUE(sub->GetOrderedDeliveryStatus().retention_exhausted);
        EXPECT_EQ(SubscriberTestPeer::PendingBufferedSlots(*sub), 0u);
        EXPECT_EQ(sub->GetOrderedDeliveryStatus().retained_bytes, 0u);
    }
}

TEST(Order5Retention, CarryGrowthChargesBothAllocationsAndOwnerOutlivesSubscriber) {
    SubscriberTestPeer::StreamParseState state;
    auto sub = BoundedSubscriber({9000, 64});
    std::mt19937_64 rng(17);
    const auto bytes = BuildStream(0, 1, 1, 6000, rng);
    // Single-byte input keeps the input chunk tiny. Growth from4096 to8192
    // must fail with the old4096 allocation still charged against9000.
    for (size_t i = 0; i < bytes.size(); ++i) {
        SubscriberTestPeer::ParseChunk(*sub, state, bytes.data() + i, 1);
        if (sub->GetOrderedDeliveryStatus().retention_exhausted) break;
    }
    const auto status = sub->GetOrderedDeliveryStatus();
    EXPECT_TRUE(status.retention_exhausted);
    EXPECT_EQ(status.retained_bytes, 4096u);
    EXPECT_LE(status.peak_retained_bytes, 9000u);
    // StreamParseState can retain its budget owner beyond Subscriber lifetime.
    sub.reset();
    state = {};
}

TEST(Order5Retention, ConsumerViewsKeepChunkChargedUntilNextConsume) {
    auto sub = BoundedSubscriber({1 << 20, 64});
    SubscriberTestPeer::StreamParseState state;
    const auto bytes = IndexedAuditFrames(0, 4);
    SubscriberTestPeer::ParseChunk(*sub, state, bytes.data(), bytes.size());
    std::vector<Subscriber::OrderedMessageView> views;
    ASSERT_EQ(sub->ConsumeOrderedBatch(&views, 4, 100), 4u);
    EXPECT_EQ(sub->GetOrderedDeliveryStatus().retained_bytes, bytes.size());
    uint64_t index = UINT64_MAX;
    std::memcpy(&index, static_cast<uint8_t*>(views.back().data) + kHeaderSize, sizeof(index));
    EXPECT_EQ(index, 3u);
    EXPECT_EQ(sub->ConsumeOrderedBatch(&views, 1, 0), 0u);
    EXPECT_EQ(sub->GetOrderedDeliveryStatus().retained_bytes, 0u);
    EXPECT_LE(SubscriberTestPeer::AllocatedMessages(*sub), 64u);
}

TEST(Order5DeliveryAudit, ConcurrentIndexedConsumptionReusesBoundedRetainedStorage) {
    using namespace std::chrono_literals;
    constexpr size_t count = 2048;
    constexpr size_t budget = 16 << 10;
    auto sub = BoundedSubscriber({budget, 256});
    SubscriberTestPeer::StreamParseState earlier, later;
    const std::string seed(32, 'a');
    std::atomic<bool> cancelled{false};
    auto audit = std::async(std::launch::async, [&] {
        return sub->AuditOrderedDelivery(count, seed.data(), seed.size(), 5000, true, &cancelled);
    });
    bool progress = true;
    for (size_t start = 0; start < count && progress; start += 32) {
        // Two simulated broker connections overtake one another every pair.
        const auto second = IndexedAuditFrames(start + 16, 16);
        const auto first = IndexedAuditFrames(start, 16);
        SubscriberTestPeer::ParseChunk(*sub, later, second.data(), second.size());
        SubscriberTestPeer::ParseChunk(*sub, earlier, first.data(), first.size());
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (sub->GetOrderedDeliveryStatus().delivered_messages < start + 32 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        progress = sub->GetOrderedDeliveryStatus().delivered_messages >= start + 32;
    }
    if (!progress) cancelled.store(true);
    EXPECT_TRUE(progress);
    EXPECT_TRUE(audit.get());
    EXPECT_TRUE(sub->ValidateOrderedDeliveryCompletion(count));
    EXPECT_LE(sub->GetOrderedDeliveryStatus().peak_retained_bytes, budget);
    EXPECT_LE(SubscriberTestPeer::AllocatedMessages(*sub), 256u);
    // Total received framed data exceeds the budget by >8x; it cannot all remain retained.
    EXPECT_GT(count * Embarcadero::wire::ComputeStrideV2(seed.size()), budget * 4);
}

TEST(Order5DeliveryAudit, FinalCounterCheckRejectsErrorsAfterEarlyAuditCompletion) {
    const std::string payload(32, 'a');
    for (bool duplicate : {false, true}) {
        auto sub = BoundedSubscriber({1 << 20, 64});
        SubscriberTestPeer::StreamParseState state;
        const auto bytes = AuditFrames(1);
        SubscriberTestPeer::ParseChunk(*sub, state, bytes.data(), bytes.size());
        ASSERT_TRUE(sub->AuditOrderedDelivery(1, payload.data(), payload.size(), 100));
        ASSERT_TRUE(sub->ValidateOrderedDeliveryCompletion(1));
        if (duplicate) {
            SubscriberTestPeer::ParseChunk(*sub, state, bytes.data(), bytes.size());
        } else {
            const std::vector<uint8_t> invalid(sizeof(Embarcadero::wire::BatchMetadata), 0xff);
            SubscriberTestPeer::ParseChunk(*sub, state, invalid.data(), invalid.size());
        }
        EXPECT_FALSE(sub->ValidateOrderedDeliveryCompletion(1));
    }
}

TEST(Order5DeliveryAudit, CancellationAndShutdownInterruptLongAuditDeadline) {
    using namespace std::chrono_literals;
    const char* previous = std::getenv("EMBARCADERO_CONSUME_ORDERED_WAIT_US");
    const std::string saved = previous ? previous : "";
    const bool had_previous = previous != nullptr;
    setenv("EMBARCADERO_CONSUME_ORDERED_WAIT_US", "1000000", 1);
    for (bool stop_subscriber : {false, true}) {
        auto sub = BoundedSubscriber({1 << 20, 64});
        const std::string seed(32, 'a');
        std::atomic<bool> cancelled{false};
        std::promise<void> started;
        auto ready = started.get_future();
        auto audit = std::async(std::launch::async, [&] {
            started.set_value();
            return sub->AuditOrderedDelivery(1, seed.data(), seed.size(), 10000, true, &cancelled);
        });
        ready.wait();
        std::this_thread::sleep_for(10ms);
        if (stop_subscriber) sub->Shutdown();
        else cancelled.store(true, std::memory_order_release);
        EXPECT_EQ(audit.wait_for(200ms), std::future_status::ready);
        EXPECT_FALSE(audit.get());
    }
    if (had_previous) setenv("EMBARCADERO_CONSUME_ORDERED_WAIT_US", saved.c_str(), 1);
    else unsetenv("EMBARCADERO_CONSUME_ORDERED_WAIT_US");
}

TEST(Order5DeliveryAudit, TerminalParseErrorDoesNotWaitForMissingPopulation) {
    using namespace std::chrono_literals;
    auto sub = BoundedSubscriber({1 << 20, 64});
    SubscriberTestPeer::StreamParseState state;
    const std::vector<uint8_t> invalid(sizeof(Embarcadero::wire::BatchMetadata), 0xff);
    SubscriberTestPeer::ParseChunk(*sub, state, invalid.data(), invalid.size());
    const std::string seed(32, 'a');
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(sub->AuditOrderedDelivery(100, seed.data(), seed.size(), 10000, true));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
}

TEST(LatencyDeliveryCompletion, ActualRetentionFailureCannotBecomeAckPrimarySuccess) {
    using namespace std::chrono_literals;
    auto sub = BoundedSubscriber({1 << 20, 1});
    DistributedKVStoreTestPeer::DeliveryFailure kv_failure;
    EXPECT_FALSE(kv_failure.Observe(sub->GetOrderedDeliveryStatus()));
    EXPECT_NO_THROW(kv_failure.ThrowIfFailed());
    SubscriberTestPeer::StreamParseState state;
    const auto bytes = IndexedAuditFrames(0, 2);
    SubscriberTestPeer::ParseChunk(*sub, state, bytes.data(), bytes.size());
    ASSERT_TRUE(sub->GetOrderedDeliveryStatus().retention_exhausted);
    std::vector<Subscriber::OrderedMessageView> batch;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(sub->ConsumeOrderedBatch(&batch, 2, 10000), 0u);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);

    auto consumer = std::async(std::launch::async, [&] {
        return kv_failure.Observe(sub->GetOrderedDeliveryStatus());
    });
    ASSERT_TRUE(consumer.get());
    EXPECT_THROW(kv_failure.ThrowIfFailed(), std::runtime_error);
    EXPECT_TRUE(kv_failure.Observe({})); // A later benign snapshot cannot revive the stream.
    EXPECT_THROW(kv_failure.ThrowIfFailed(), std::runtime_error);

    Embarcadero::client::DeliveryCompletion completion;
    EXPECT_TRUE(completion.ObserveOrderedStatus(sub->GetOrderedDeliveryStatus()));
    EXPECT_FALSE(completion.timed_out);  // Terminal failure happened before any deadline.
    EXPECT_FALSE(completion.MayUseAckPrimary(true, false, false));
    completion.timed_out = true;
    EXPECT_FALSE(completion.MayUseAckPrimary(true, false, true));
    EXPECT_TRUE(completion.ObserveOrderedStatus({}));  // Failure cannot be reset by a later snapshot.
}

TEST(LatencyDeliveryCompletion, OrdinaryMissingPopulationRetainsTimeoutPolicy) {
    auto sub = BoundedSubscriber({1 << 20, 64});
    std::vector<Subscriber::OrderedMessageView> batch;
    EXPECT_EQ(sub->ConsumeOrderedBatch(&batch, 1, 1), 0u);
    Embarcadero::client::DeliveryCompletion completion;
    EXPECT_FALSE(completion.ObserveOrderedStatus(sub->GetOrderedDeliveryStatus()));
    EXPECT_FALSE(completion.MayUseAckPrimary(true, false, false));
    completion.timed_out = true;
    EXPECT_TRUE(completion.MayUseAckPrimary(true, false, false));
    EXPECT_FALSE(completion.MayUseAckPrimary(false, false, false));
    EXPECT_FALSE(completion.MayUseAckPrimary(true, true, false));
    sub->Shutdown();
    EXPECT_TRUE(completion.ObserveOrderedStatus(sub->GetOrderedDeliveryStatus()));
    EXPECT_FALSE(completion.MayUseAckPrimary(true, false, false));
}

TEST(LatencyDeliveryCompletion, FinalSnapshotRejectsErrorsAfterPopulationWasDrained) {
    for (bool duplicate : {false, true}) {
        auto sub = BoundedSubscriber({1 << 20, 64});
        SubscriberTestPeer::StreamParseState state;
        const auto bytes = IndexedAuditFrames(0, 1);
        SubscriberTestPeer::ParseChunk(*sub, state, bytes.data(), bytes.size());
        std::vector<Subscriber::OrderedMessageView> batch;
        ASSERT_EQ(sub->ConsumeOrderedBatch(&batch, 1, 100), 1u);
        Embarcadero::client::DeliveryCompletion completion;
        ASSERT_FALSE(completion.ObserveOrderedStatus(sub->GetOrderedDeliveryStatus()));
        if (duplicate) {
            SubscriberTestPeer::ParseChunk(*sub, state, bytes.data(), bytes.size());
        } else {
            const std::vector<uint8_t> invalid(sizeof(Embarcadero::wire::BatchMetadata), 0xff);
            SubscriberTestPeer::ParseChunk(*sub, state, invalid.data(), invalid.size());
        }
        EXPECT_TRUE(completion.ObserveOrderedStatus(sub->GetOrderedDeliveryStatus()));
        EXPECT_FALSE(completion.MayUseAckPrimary(true, false, true));
    }
}

TEST(Order5WireAlignment, PublicViewReadsBothFormatsAtEveryByteAlignment) {
    for (uint16_t version : {Embarcadero::wire::HEADER_VERSION_V1, Embarcadero::wire::HEADER_VERSION_V2}) {
        for (size_t offset = 0; offset < 64; ++offset) {
            std::vector<uint8_t> storage(offset + 128, 0);
            auto* raw = storage.data() + offset;
            if (version == Embarcadero::wire::HEADER_VERSION_V2) {
                Embarcadero::BlogMessageHeader header{};
                header.size = 32; header.total_order = 47; header.client_id = 17; header.batch_seq = 9;
                std::memcpy(raw, &header, sizeof(header));
            } else {
                Embarcadero::MessageHeader header{};
                header.size = 32; header.paddedSize = 128; header.total_order = 47;
                header.client_id = 17; header.client_order = 9;
                std::memcpy(raw, &header, sizeof(header));
            }
            Subscriber::OrderedMessageView view{raw, version};
            EXPECT_EQ(view.PayloadSize(), 32u);
            EXPECT_EQ(view.PaddedSize(), 128u);
            EXPECT_EQ(view.HeaderSize(), 64u);
            EXPECT_EQ(view.TotalOrder(), 47u);
            EXPECT_EQ(view.ClientId(), 17u);
            EXPECT_EQ(view.ClientOrder(), 9u);
            EXPECT_EQ(view.Payload(), raw + 64);
        }
    }
}

TEST(Order5WireAlignment, V1FragmentationAndIndexedAuditUseSerializedHeaderFields) {
    auto& sub = SharedSubscriber();
    constexpr size_t payload_size = 32;
    constexpr size_t stride = 128;
    Embarcadero::wire::BatchMetadata metadata{};
    metadata.header_version = Embarcadero::wire::HEADER_VERSION_V1;
    metadata.num_messages = 2;
    std::vector<uint8_t> bytes(sizeof(metadata) + 2 * stride, 0);
    std::memcpy(bytes.data(), &metadata, sizeof(metadata));
    for (size_t i = 0; i < 2; ++i) {
        Embarcadero::MessageHeader header{};
        header.paddedSize = stride; header.size = payload_size;
        auto* frame = bytes.data() + sizeof(metadata) + i * stride;
        std::memcpy(frame, &header, sizeof(header));
        std::memset(frame + sizeof(header), 'a', payload_size);
        const uint64_t sequence = i;
        std::memcpy(frame + sizeof(header), &sequence, sizeof(sequence));
    }
    const std::string seed(payload_size, 'a');
    for (size_t split = 1; split < bytes.size(); ++split) {
        SubscriberTestPeer::ResetOrderState(sub);
        SubscriberTestPeer::StreamParseState state;
        SubscriberTestPeer::ParseChunk(sub, state, bytes.data(), split);
        SubscriberTestPeer::ParseChunk(sub, state, bytes.data() + split, bytes.size() - split);
        ASSERT_TRUE(sub.AuditOrderedDelivery(2, seed.data(), seed.size(), 100, true));
        ASSERT_TRUE(sub.ValidateOrderedDeliveryCompletion(2));
    }
}
