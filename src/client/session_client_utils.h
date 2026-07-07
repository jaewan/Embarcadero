#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

inline uint64_t Splitmix64Finalize(uint64_t x) {
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

inline int RendezvousBroker(uint32_t client_id,
                            uint64_t batch_seq,
                            const std::vector<int>& survivors,
                            int exclude_broker) {
	int best_broker = -1;
	uint64_t best_score = 0;
	bool have_best = false;
	for (int broker : survivors) {
		if (broker == exclude_broker) continue;
		uint64_t mixed = static_cast<uint64_t>(client_id);
		mixed ^= batch_seq + 0x9e3779b97f4a7c15ULL + (mixed << 6) + (mixed >> 2);
		mixed ^= static_cast<uint64_t>(static_cast<uint32_t>(broker)) +
		         0xbf58476d1ce4e5b9ULL + (mixed << 7) + (mixed >> 3);
		const uint64_t score = Splitmix64Finalize(mixed);
		if (!have_best || score > best_score ||
		    (score == best_score && broker < best_broker)) {
			best_score = score;
			best_broker = broker;
			have_best = true;
		}
	}
	return best_broker;
}

inline size_t RebasedAckBaseAfterFenceCredit(size_t prior_ack_received,
                                             size_t locally_committed_messages) {
	return prior_ack_received + locally_committed_messages;
}

inline bool SessionGlobalUnackedRetired(size_t batch_global_ack_end, size_t acked_messages) {
	return batch_global_ack_end <= acked_messages;
}

inline size_t SessionGlobalAckFromGeneration(size_t ack_base, size_t generation_ack) {
	return ack_base + generation_ack;
}

inline bool SessionPrefixAckEnd(uint64_t batch_seq,
                                uint64_t expected_batch_seq,
                                size_t current_prefix_hwm,
                                size_t batch_num_msg,
                                size_t* batch_ack_end) {
	if (batch_seq != expected_batch_seq || batch_ack_end == nullptr) return false;
	*batch_ack_end = current_prefix_hwm + batch_num_msg;
	return true;
}

inline size_t NextBatchSeqAfterSuffixResubmit(size_t suffix_batches) {
	return suffix_batches;
}

inline uint32_t NextSessionEpochAfterFence(uint32_t old_epoch) {
	return old_epoch == 0 ? 1U : old_epoch + 1U;
}

struct DeltaEstimator {
	static constexpr double kDeltaFloorMs = 1.7;
	static constexpr double kDeltaCapMs = 12.0;
	static constexpr double kAlpha = 1.0 / 8.0;
	static constexpr double kBeta = 1.0 / 4.0;

	bool initialized{false};
	double srtt_ms{kDeltaFloorMs};
	double rttvar_ms{kDeltaFloorMs / 2.0};

	double delta_ms() const {
		const double raw = initialized ? (srtt_ms + 4.0 * rttvar_ms) : kDeltaFloorMs;
		return std::min(kDeltaCapMs, std::max(kDeltaFloorMs, raw));
	}

	void sample(uint64_t rtt_ns, uint32_t attempt) {
		if (attempt != 0) return;
		const double r_ms = static_cast<double>(rtt_ns) / 1e6;
		if (!initialized) {
			srtt_ms = r_ms;
			rttvar_ms = r_ms / 2.0;
			initialized = true;
			return;
		}
		rttvar_ms = (1.0 - kBeta) * rttvar_ms + kBeta * std::abs(srtt_ms - r_ms);
		srtt_ms = (1.0 - kAlpha) * srtt_ms + kAlpha * r_ms;
	}
};
