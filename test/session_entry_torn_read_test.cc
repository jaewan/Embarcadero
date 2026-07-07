#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "../src/cxl_manager/cxl_datastructure.h"

namespace {

constexpr uint64_t kSessionKey = (0x1234ULL << 32) | 1ULL;
constexpr uint64_t kActiveFlag = 1ULL << 1;

struct Snapshot {
	uint64_t expected_seq{0};
	uint64_t committed_hwm{0};
	uint64_t highest_sequenced{0};
	uint64_t state_word{0};

	bool operator==(const Snapshot& other) const {
		return expected_seq == other.expected_seq &&
		       committed_hwm == other.committed_hwm &&
		       highest_sequenced == other.highest_sequenced &&
		       state_word == other.state_word;
	}
};

Snapshot ReadSnapshot(Embarcadero::SessionEntry& entry) {
	Embarcadero::CXL::invalidate_cacheline_for_read(&entry);
	Embarcadero::CXL::load_fence();
	Snapshot snapshot;
	snapshot.expected_seq = entry.expected_seq.load(std::memory_order_relaxed);
	snapshot.committed_hwm = entry.committed_hwm.load(std::memory_order_relaxed);
	snapshot.highest_sequenced = entry.highest_sequenced.load(std::memory_order_relaxed);
	snapshot.state_word = entry.state_word.load(std::memory_order_acquire);
	return snapshot;
}

void Publish(Embarcadero::SessionEntry& entry, uint32_t epoch) {
	const uint64_t expected_seq = static_cast<uint64_t>(epoch) * 10ULL;
	const uint64_t committed_hwm = expected_seq + 1;
	const uint64_t highest_sequenced = expected_seq + 2;

	entry.session_key.store(kSessionKey, std::memory_order_relaxed);
	entry.expected_seq.store(expected_seq, std::memory_order_relaxed);
	entry.committed_hwm.store(committed_hwm, std::memory_order_relaxed);
	entry.highest_sequenced.store(highest_sequenced, std::memory_order_relaxed);
	Embarcadero::CXL::store_fence();
	Embarcadero::CXL::flush_cacheline(&entry);
	Embarcadero::CXL::store_fence();

	entry.state_word.store((static_cast<uint64_t>(epoch) << 32) | kActiveFlag,
		std::memory_order_release);
	Embarcadero::CXL::store_fence();
	Embarcadero::CXL::flush_cacheline(&entry.state_word);
	Embarcadero::CXL::store_fence();
}

}  // namespace

TEST(SessionEntryTornReadTest, PublishedStateNeverOutrunsData) {
	alignas(64) Embarcadero::SessionEntry entry;
	Publish(entry, 1);

	std::atomic<bool> start{false};
	std::atomic<bool> stop{false};
	std::atomic<uint64_t> torn_reads{0};
	std::atomic<uint32_t> published_epoch{1};

	std::thread writer([&] {
		while (!start.load(std::memory_order_acquire)) {
			Embarcadero::CXL::cpu_pause();
		}
		for (uint32_t epoch = 2; epoch < 200000; ++epoch) {
			Publish(entry, epoch);
			published_epoch.store(epoch, std::memory_order_release);
		}
		stop.store(true, std::memory_order_release);
	});

	std::thread reader([&] {
		start.store(true, std::memory_order_release);
		while (!stop.load(std::memory_order_acquire)) {
			const Snapshot first = ReadSnapshot(entry);
			const Snapshot second = ReadSnapshot(entry);
			if (!(first == second)) continue;

			const uint64_t expected_seq = second.expected_seq;
			const uint64_t committed_hwm = second.committed_hwm;
			const uint64_t highest_sequenced = second.highest_sequenced;
			const uint64_t state_word = second.state_word;
			const uint32_t epoch = static_cast<uint32_t>(state_word >> 32);
			if (epoch == 0) continue;

			const uint64_t data_epoch = expected_seq / 10ULL;
			const uint32_t completed_epoch = published_epoch.load(std::memory_order_acquire);
			if (epoch > completed_epoch || data_epoch > completed_epoch) continue;

			if ((expected_seq % 10ULL) != 0 ||
			    data_epoch < epoch ||
			    committed_hwm != expected_seq + 1 ||
			    highest_sequenced != expected_seq + 2) {
				torn_reads.fetch_add(1, std::memory_order_relaxed);
				stop.store(true, std::memory_order_release);
				break;
			}
		}
	});

	writer.join();
	reader.join();

	EXPECT_EQ(torn_reads.load(std::memory_order_relaxed), 0);
}
