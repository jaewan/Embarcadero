#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <glog/logging.h>

namespace Embarcadero {
namespace StageTrace {

/**
 * Low-overhead publish→ACK stage instrumentation.
 *
 * Enable with EMBARCADERO_STAGE_TRACE=1. Sampling interval defaults to every
 * 1024th batch (EMBARCADERO_STAGE_TRACE_SAMPLE_N). Do not infer one-way
 * cross-host latency unless PTP offset/uncertainty are recorded separately.
 *
 * Negative findings preserved from prior sessions:
 * - inactive 100 MB request path (not executed by throughput tests)
 * - getenv storm (fixed; did not move the ceiling)
 */

inline bool Enabled() {
	static const bool on = []() {
		const char* e = std::getenv("EMBARCADERO_STAGE_TRACE");
		return e != nullptr && e[0] == '1' && e[1] == '\0';
	}();
	return on;
}

inline uint64_t SampleN() {
	static const uint64_t n = []() -> uint64_t {
		const char* e = std::getenv("EMBARCADERO_STAGE_TRACE_SAMPLE_N");
		if (e == nullptr || e[0] == '\0') return 1024ULL;
		return static_cast<uint64_t>(std::strtoull(e, nullptr, 10));
	}();
	return n == 0 ? 1 : n;
}

inline bool ShouldSample(uint64_t seq) {
	return Enabled() && (seq % SampleN()) == 0;
}

enum class Stage : uint8_t {
	ClientSeal = 1,
	ClientEnqueue = 2,
	ClientSendDone = 3,
	BrokerRecvDone = 4,
	PbrVisible = 5,
	Level5Accept = 6,
	Level5Hold = 7,
	Level5Release = 8,
	GoiAssign = 9,
	GoiCommit = 10,
	OrderedFrontier = 11,
	DurableFrontier = 12,
	AckObserve = 13,
	AckSerialize = 14,
	ClientAckParse = 15,
	ClientLedgerRetire = 16,
};

struct Counters {
	std::atomic<uint64_t> samples{0};
	std::atomic<uint64_t> last_stage_ns{0};
	std::atomic<uint64_t> backlog_hint{0};
};

inline Counters& Global() {
	static Counters c;
	return c;
}

inline void Record(Stage stage, uint64_t client_id, uint64_t batch_seq, uint64_t now_ns,
                   uint64_t backlog_hint = 0) {
	if (!ShouldSample(batch_seq)) return;
	Global().samples.fetch_add(1, std::memory_order_relaxed);
	Global().last_stage_ns.store(now_ns, std::memory_order_relaxed);
	if (backlog_hint != 0) {
		Global().backlog_hint.store(backlog_hint, std::memory_order_relaxed);
	}
	LOG_EVERY_N(INFO, 64) << "[STAGE_TRACE]"
	                      << " stage=" << static_cast<int>(stage)
	                      << " client=" << client_id
	                      << " batch_seq=" << batch_seq
	                      << " ts_ns=" << now_ns
	                      << " backlog=" << backlog_hint;
}

}  // namespace StageTrace
}  // namespace Embarcadero
