#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace {

// Model of the parallel SourcePipeline protocol used by chain_replication.cc.
// These tests encode the correctness contracts without linking embarlet/CXL.

enum class SinkMode { DiskDurable, MemoryCopy, MemoryAccounting };

struct PipelineTask {
	uint64_t goi{0};
	uint16_t source{0};
	uint16_t owner{0};
	uint16_t role{0};
	uint64_t pbr{0};
	uint64_t cumulative{0};
	size_t end_offset{0};
	bool sink_done{false};
	bool sync_done{false};
	bool token_done{false};
	bool cv_done{false};
};

struct SourceModel {
	int role{-1};
	std::deque<PipelineTask> sink_q;
	std::deque<PipelineTask> awaiting_sync;
	std::deque<PipelineTask> token_q;
	size_t written{0};
	size_t synced{0};
	size_t bytes_since_sync{0};
	std::vector<uint8_t> mem_ring;
	size_t mem_pos{0};
	uint64_t cv_logical{0};
	bool cv_has_hole_skip{false};
};

struct ChainModel {
	int rf{2};
	SinkMode sink{SinkMode::DiskDurable};
	size_t sync_bytes{64 * 1024 * 1024};
	std::vector<uint32_t> tokens;  // per GOI
	std::vector<SourceModel> sources;
	uint64_t next_dispatch{0};

	explicit ChainModel(int num_sources, int replication_factor, SinkMode mode)
		: rf(replication_factor), sink(mode), tokens(1024, 0), sources(num_sources) {
		for (int s = 0; s < num_sources; ++s) {
			// Local broker role-maps for source S: role 0 on S, role 1 on (S+1)%N for RF=2.
			sources[static_cast<size_t>(s)].role = 0;
		}
	}

	void SetRole(int source, int role) { sources[static_cast<size_t>(source)].role = role; }

	// Copy-ahead: dispatch even if predecessor token is absent.
	bool Dispatch(uint64_t goi, uint16_t source, uint16_t owner, uint16_t role,
	              uint64_t pbr, uint64_t cumulative, size_t payload) {
		if (goi != next_dispatch) return false;  // serialized GOI order
		if (tokens.size() <= goi) tokens.resize(goi + 1, 0);
		PipelineTask t;
		t.goi = goi;
		t.source = source;
		t.owner = owner;
		t.role = role;
		t.pbr = pbr;
		t.cumulative = cumulative;
		sources[source].sink_q.push_back(t);
		sources[source].written += payload;
		next_dispatch = goi + 1;
		return true;
	}

	void CompleteSink(int source) {
		auto& s = sources[static_cast<size_t>(source)];
		if (s.sink_q.empty()) return;
		PipelineTask t = s.sink_q.front();
		s.sink_q.pop_front();
		t.sink_done = true;
		t.end_offset = s.written;
		if (sink == SinkMode::DiskDurable) {
			s.awaiting_sync.push_back(t);
			s.bytes_since_sync += 1;  // unit bytes in model
		} else {
			t.sync_done = true;
			s.token_q.push_back(t);
			if (sink == SinkMode::MemoryCopy && !s.mem_ring.empty()) {
				s.mem_ring[s.mem_pos % s.mem_ring.size()] = 1;
				s.mem_pos++;
			}
		}
	}

	// Sync releases only tasks at or below synced offset; queue length is NOT a sync trigger.
	bool MaybeSync(int source, bool force_by_bytes) {
		auto& s = sources[static_cast<size_t>(source)];
		if (sink != SinkMode::DiskDurable) return false;
		if (s.awaiting_sync.empty()) return false;
		if (!force_by_bytes && s.bytes_since_sync < sync_bytes && s.awaiting_sync.size() >= 64) {
			// Explicit: queue depth alone must not force early sync.
			return false;
		}
		if (!force_by_bytes && s.bytes_since_sync < sync_bytes) return false;
		s.synced = s.awaiting_sync.back().end_offset;
		s.bytes_since_sync = 0;
		while (!s.awaiting_sync.empty() && s.awaiting_sync.front().end_offset <= s.synced) {
			PipelineTask t = s.awaiting_sync.front();
			s.awaiting_sync.pop_front();
			t.sync_done = true;
			s.token_q.push_back(t);
		}
		return true;
	}

	// Token wait happens only after sink(+sync). Tail updates CV; no hole skipping.
	bool AdvanceTokenHead(int source) {
		auto& s = sources[static_cast<size_t>(source)];
		if (s.token_q.empty()) return false;
		PipelineTask& t = s.token_q.front();
		if (!t.sink_done || (sink == SinkMode::DiskDurable && !t.sync_done)) return false;
		if (tokens[t.goi] < t.role) return false;  // wait for predecessor
		if (tokens[t.goi] == t.role) {
			tokens[t.goi] = static_cast<uint32_t>(t.role) + 1u;
			t.token_done = true;
		}
		if (t.role == static_cast<uint16_t>(rf - 1) && tokens[t.goi] >= static_cast<uint32_t>(rf)) {
			// CV must be monotonic without hole skipping on this owner.
			if (t.cumulative < s.cv_logical) {
				s.cv_has_hole_skip = true;
				return false;
			}
			s.cv_logical = t.cumulative;
			t.cv_done = true;
		} else if (t.role != static_cast<uint16_t>(rf - 1)) {
			t.cv_done = true;  // non-tail has no CV duty
		}
		if (t.token_done && t.cv_done) {
			s.token_q.pop_front();
			return true;
		}
		return false;
	}
};

TEST(ChainPipelineTest, CopyAheadWhilePredecessorTokenAbsent) {
	ChainModel m(/*sources=*/2, /*rf=*/2, SinkMode::MemoryCopy);
	m.SetRole(0, 1);  // tail for source 0
	ASSERT_TRUE(m.Dispatch(0, 0, 0, 1, 1, 10, 100));
	// Predecessor token still 0; sink may still proceed (copy-ahead).
	EXPECT_EQ(m.tokens[0], 0u);
	m.CompleteSink(0);
	EXPECT_EQ(m.sources[0].token_q.size(), 1u);
	EXPECT_FALSE(m.AdvanceTokenHead(0));  // blocked on predecessor
	m.tokens[0] = 1;                      // head publishes
	EXPECT_TRUE(m.AdvanceTokenHead(0));
	EXPECT_EQ(m.tokens[0], 2u);
}

TEST(ChainPipelineTest, PerSourceFifoUnderInterleaving) {
	ChainModel m(/*sources=*/2, /*rf=*/2, SinkMode::MemoryAccounting);
	m.SetRole(0, 0);
	m.SetRole(1, 0);
	ASSERT_TRUE(m.Dispatch(0, 0, 0, 0, 1, 10, 8));
	ASSERT_TRUE(m.Dispatch(1, 1, 1, 0, 1, 10, 8));
	ASSERT_TRUE(m.Dispatch(2, 0, 0, 0, 2, 20, 8));
	m.CompleteSink(1);
	m.CompleteSink(0);
	m.CompleteSink(0);
	EXPECT_EQ(m.sources[0].token_q.front().goi, 0u);
	EXPECT_EQ(m.sources[1].token_q.front().goi, 1u);
	EXPECT_TRUE(m.AdvanceTokenHead(0));
	EXPECT_EQ(m.sources[0].token_q.front().goi, 2u);
}

TEST(ChainPipelineTest, NoCvAdvanceBeforeSinkSyncCompletion) {
	ChainModel m(/*sources=*/1, /*rf=*/1, SinkMode::DiskDurable);
	m.SetRole(0, 0);
	m.sync_bytes = 100;
	ASSERT_TRUE(m.Dispatch(0, 0, 0, 0, 1, 10, 8));
	m.CompleteSink(0);
	EXPECT_FALSE(m.AdvanceTokenHead(0));  // still awaiting sync
	EXPECT_EQ(m.sources[0].cv_logical, 0u);
	EXPECT_TRUE(m.MaybeSync(0, /*force_by_bytes=*/true));
	EXPECT_TRUE(m.AdvanceTokenHead(0));
	EXPECT_EQ(m.sources[0].cv_logical, 10u);
}

TEST(ChainPipelineTest, QueueDepthDoesNotForceEarlySync) {
	ChainModel m(/*sources=*/1, /*rf=*/2, SinkMode::DiskDurable);
	m.SetRole(0, 0);
	m.sync_bytes = 1'000'000;
	for (uint64_t i = 0; i < 64; ++i) {
		ASSERT_TRUE(m.Dispatch(i, 0, 0, 0, i + 1, (i + 1) * 10, 1));
		m.CompleteSink(0);
	}
	EXPECT_EQ(m.sources[0].awaiting_sync.size(), 64u);
	EXPECT_FALSE(m.MaybeSync(0, /*force_by_bytes=*/false));
	EXPECT_EQ(m.sources[0].token_q.size(), 0u);
}

TEST(ChainPipelineTest, Rf2AndRf3TokenOrder) {
	for (int rf : {2, 3}) {
		ChainModel m(/*sources=*/1, rf, SinkMode::MemoryCopy);
		m.SetRole(0, static_cast<uint16_t>(rf - 1));
		ASSERT_TRUE(m.Dispatch(0, 0, 0, static_cast<uint16_t>(rf - 1), 1, 10, 4));
		m.CompleteSink(0);
		EXPECT_FALSE(m.AdvanceTokenHead(0));
		m.tokens[0] = static_cast<uint32_t>(rf - 1);
		EXPECT_TRUE(m.AdvanceTokenHead(0));
		EXPECT_EQ(m.tokens[0], static_cast<uint32_t>(rf));
	}
}

TEST(ChainPipelineTest, CvMonotonicityWithoutHoleSkipping) {
	ChainModel m(/*sources=*/1, /*rf=*/1, SinkMode::MemoryAccounting);
	m.SetRole(0, 0);
	ASSERT_TRUE(m.Dispatch(0, 0, 0, 0, 1, 10, 4));
	ASSERT_TRUE(m.Dispatch(1, 0, 0, 0, 2, 20, 4));
	m.CompleteSink(0);
	m.CompleteSink(0);
	EXPECT_TRUE(m.AdvanceTokenHead(0));
	EXPECT_EQ(m.sources[0].cv_logical, 10u);
	EXPECT_TRUE(m.AdvanceTokenHead(0));
	EXPECT_EQ(m.sources[0].cv_logical, 20u);
	EXPECT_FALSE(m.sources[0].cv_has_hole_skip);
}

TEST(ChainPipelineTest, MemoryRingWrap) {
	ChainModel m(/*sources=*/1, /*rf=*/1, SinkMode::MemoryCopy);
	m.SetRole(0, 0);
	m.sources[0].mem_ring.assign(4, 0);
	for (uint64_t i = 0; i < 10; ++i) {
		ASSERT_TRUE(m.Dispatch(i, 0, 0, 0, i + 1, (i + 1) * 10, 1));
		m.CompleteSink(0);
		EXPECT_TRUE(m.AdvanceTokenHead(0));
	}
	EXPECT_EQ(m.sources[0].mem_pos, 10u);
	EXPECT_EQ(m.sources[0].mem_ring.size(), 4u);
}

TEST(ChainPipelineTest, CleanShutdownDrain) {
	ChainModel m(/*sources=*/1, /*rf=*/2, SinkMode::DiskDurable);
	m.SetRole(0, 0);
	m.sync_bytes = 1;
	ASSERT_TRUE(m.Dispatch(0, 0, 0, 0, 1, 10, 1));
	m.CompleteSink(0);
	EXPECT_TRUE(m.MaybeSync(0, true));
	m.tokens[0] = 0;
	EXPECT_TRUE(m.AdvanceTokenHead(0));
	EXPECT_TRUE(m.sources[0].sink_q.empty());
	EXPECT_TRUE(m.sources[0].awaiting_sync.empty());
	EXPECT_TRUE(m.sources[0].token_q.empty());
}

TEST(ChainPipelineTest, AckClaimLabelContract) {
	auto label = [](SinkMode mode) -> const char* {
		return mode == SinkMode::DiskDurable ? "media_durable" : "replicated_ack_emulated";
	};
	EXPECT_STREQ(label(SinkMode::DiskDurable), "media_durable");
	EXPECT_STREQ(label(SinkMode::MemoryCopy), "replicated_ack_emulated");
	EXPECT_STREQ(label(SinkMode::MemoryAccounting), "replicated_ack_emulated");
}

}  // namespace
