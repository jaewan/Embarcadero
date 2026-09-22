#pragma once

#include "topic_session_key.h"
#include "common/env_flags.h"
#include "common/order_level.h"
#include "common/performance_utils.h"
#include "cxl_manager/cxl_datastructure.h"
#include <chrono>
#include <cstdlib>
#include <limits>
#include <glog/logging.h>

// Private implementation helpers shared by Topic compilation units. External
// inline linkage preserves one instance of each cached environment value across
// translation units while retaining compiler visibility of hot helper bodies.
namespace Embarcadero::topic_internal {

inline constexpr size_t kReplicationNotStarted = std::numeric_limits<size_t>::max();

inline uint64_t SteadyNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline bool ShouldEnableOrder5Trace() {
	static const bool enabled = ReadEnvBoolStrict("EMBARCADERO_ORDER5_TRACE", false);
	return enabled;
}

inline bool ShouldFatalOnOrder5ExportOverrun() {
	// [[O5-1]] An ORDER=5 export lap drops committed total_order values from a lagging subscriber's
	// stream — a total-order violation. Default to FATAL in debug/CI so it can never regress
	// silently; in release default OFF (the lap is handled by terminalizing the lagging subscriber,
	// not by aborting the broker). The env var still overrides either default.
#ifdef NDEBUG
	static const bool kDefault = false;
#else
	static const bool kDefault = true;
#endif
	static const bool enabled = ReadEnvBoolStrict("EMBARCADERO_ORDER5_EXPORT_OVERRUN_FATAL", kDefault);
	return enabled;
}

// [[CXL-1]] Fresh read of a 2-cache-line GOIEntry across the non-coherent link. The writer is
// sentinel-last across both lines (line1 fields durable before the global_seq sentinel on line0),
// so the reader must invalidate BOTH lines before reading ANY field — invalidating only line0 (the
// sentinel line) can pair a fresh global_seq with a stale line1 (client_seq/session_epoch) from
// local cache, poisoning the recovered dedup/FIFO frontier. invalidate_cacheline_for_read is
// clflush (serializing), so a single load_fence after both invalidations is sufficient. All GOI
// readers must go through this to prevent reader drift.
inline void ReadGOIEntryFresh(const Embarcadero::GOIEntry* entry) {
	CXL::invalidate_cacheline_for_read(entry);                                        // LINE0 (global_seq)
	CXL::invalidate_cacheline_for_read(reinterpret_cast<const uint8_t*>(entry) + 64); // LINE1 (client_seq, session_epoch, ...)
	CXL::load_fence();
}

inline bool ShouldEnableOrder5PhaseDiag() {
	static const bool enabled = ReadEnvBoolLenient("EMBARCADERO_ORDER5_PHASE_DIAG", false);
	return enabled;
}

inline bool ShouldEnableFrontierTrace() {
	static const bool enabled = ReadEnvBoolLenient("EMBAR_FRONTIER_TRACE", false);
	return enabled;
}

// [[ORDER5_COMMIT_PROFILE]] Opt-in wall-clock breakdown of CommitEpoch's internal phases
// (GOI write, export-descriptor + size-recompute, per-client/CV metadata), to find where the
// single-threaded EpochSequencerThread's commit path spends time relative to raw publish
// throughput. Zero overhead when disabled beyond one relaxed atomic load per epoch.
inline bool ShouldEnableOrder5CommitProfile() {
	static const bool enabled = ReadEnvBoolLenient("EMBAR_ORDER5_COMMIT_PROFILE", false);
	return enabled;
}

// Experimental cost-isolation switch. This retains the complete ORDER=5 scanner,
// epoch, GOI, export, CV, ACK, fencing, and duplicate-suppression path, but bypasses
// predecessor comparison and hold-map enforcement. It deliberately does NOT provide
// per-session program order and must never be enabled for a correctness or failure run.
// Its only supported use is a matched throughput ablation against normal ORDER=5.
inline bool ShouldBypassOrder5SessionFIFOForAblation() {
	static const bool enabled = ReadEnvBoolLenient(
		"EMBARCADERO_ORDER5_BYPASS_SESSION_FIFO_ABLATION", false);
	return enabled;
}

inline bool ShouldEnableOrder5SessionTestTrace() {
	static const bool enabled =
		ReadEnvBoolLenient("EMBARCADERO_TEST_ORDER5_SESSION_TRACE", false);
	return enabled;
}

inline uint64_t GetOrder5ClaimedWaitUs() {
	static const uint64_t value = []() {
		if (const char* env = std::getenv("EMBARCADERO_TEST_ORDER5_CLAIMED_WAIT_MS")) {
			char* end = nullptr;
			unsigned long parsed = std::strtoul(env, &end, 10);
			if (end != env && *end == '\0' && parsed > 0) {
				return static_cast<uint64_t>(parsed) * 1000ULL;
			}
			LOG(WARNING) << "Ignoring invalid EMBARCADERO_TEST_ORDER5_CLAIMED_WAIT_MS='"
			             << env << "'; using 10s default";
		}
		return 10ULL * 1000ULL * 1000ULL;
	}();
	return value;
}

enum Order5FlightKind : uint32_t {
	kOrder5FlightDriver = 1,
	kOrder5FlightCommit = 2,
	kOrder5FlightCV = 3,
	kOrder5FlightExpiry = 4,
	kOrder5FlightDisconnect = 5,
};

inline const char* Order5FlightKindToString(uint32_t kind) {
	switch (kind) {
		case kOrder5FlightDriver: return "driver";
		case kOrder5FlightCommit: return "commit";
		case kOrder5FlightCV: return "cv";
		case kOrder5FlightExpiry: return "expiry";
		case kOrder5FlightDisconnect: return "disconnect";
		default: return "unknown";
	}
}

// [[O5-6]] The ORDER=5 flight recorder captures a clock_gettime + ring write per event
// and dumps ~ring-size LOG(ERROR) lines on broker 0. That is live per-event overhead on the
// hot path, so it is gated behind EMBARCADERO_ORDER5_FLIGHT_TRACE (default OFF). Set the env
// var to a positive integer to enable it for debugging. The env lookup is memoized in a
// function-local static so steady-state cost is a single relaxed bool load per event.
inline bool Order5FlightTraceEnabled() {
	static const bool v = []() {
		const char* e = std::getenv("EMBARCADERO_ORDER5_FLIGHT_TRACE");
		return e != nullptr && std::atoi(e) > 0;
	}();
	return v;
}

inline bool ShouldCaptureOrder5Flight(int order, int broker_id) {
	return Order5FlightTraceEnabled() && order == 5 && broker_id == 0;
}

inline uint64_t MakeClientBrokerStreamKey(size_t client_id, uint32_t session_epoch, int broker_id) {
	if (session_epoch == 0) {
		return (static_cast<uint64_t>(client_id) << 16) |
		       static_cast<uint16_t>(broker_id & 0xFFFF);
	}
	const uint64_t session_key = MakeSessionKey(client_id, session_epoch);
	return Mix64(session_key ^ (static_cast<uint64_t>(static_cast<uint16_t>(broker_id & 0xFFFF)) << 1));
}

inline uint64_t MakeClientBrokerStreamKey(size_t client_id, int broker_id) {
	return (static_cast<uint64_t>(client_id) << 16) |
	       static_cast<uint16_t>(broker_id & 0xFFFF);
}


inline bool UsesTrueClientChainOrdering(int order) {
	return order == kOrderStrong;
}

// [[W1.2]] EMBAR_ASSERT_COMMIT_ORDER=1 makes the per-session commit-order check a hard CHECK
// (abort) for tests; default off (count + LOG only) so it is safe to leave on under load runs.
inline bool ShouldAssertCommitOrder() {
	static const bool v = []() {
		const char* e = std::getenv("EMBAR_ASSERT_COMMIT_ORDER");
		return e != nullptr && std::atoi(e) > 0;
	}();
	return v;
}

[[maybe_unused]] inline uint64_t GetOrder5HoldTimeoutNs(bool replicated_ack2_mode) {
	if (const char* env = std::getenv("EMBARCADERO_ORDER5_HOLD_TIMEOUT_MS")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0') {
			return static_cast<uint64_t>(parsed) * 1000ULL * 1000ULL;
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_ORDER5_HOLD_TIMEOUT_MS='" << env
		             << "'; using runtime default";
	}
	(void)replicated_ack2_mode;
	// ORDER=5 gaps are normal while multiple publishers stripe across brokers. Treating
	// "held for N ms" as loss in steady state causes false expiry waves and ACK stalls.
	// We therefore disable steady-state age expiry by default and rely on explicit
	// force-expire windows during verified stalls/tail drain. Operators can still opt
	// back into a fixed timeout for experiments via EMBARCADERO_ORDER5_HOLD_TIMEOUT_MS.
	return 0;
}

inline uint64_t GetSessionLeaseNs(bool replicated_ack2_mode) {
	if (const char* env = std::getenv("EMBARCADERO_SESSION_LEASE_MS")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			return static_cast<uint64_t>(parsed) * 1000ULL * 1000ULL;
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_SESSION_LEASE_MS='" << env
		             << "'; using placeholder default";
	}
	// RF0 was 1000ms then 5000ms; multi-broker ORDER=5 head gaps at high offered
	// load (linger@750 4GiB) still false-fenced after ~5s while later striped
	// batches sat in hold. Keep well above that skew + pool-backpressure window.
	(void)replicated_ack2_mode;
	return 30000ULL * 1000ULL * 1000ULL;
}

inline uint64_t GetOrder5IdleForceExpireTriggerNs(bool replicated_ack2_mode) {
	if (const char* env = std::getenv("EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			return static_cast<uint64_t>(parsed) * 1000ULL * 1000ULL;
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS='" << env
		             << "'; using runtime default";
	}
	// Stay at/above the session lease so idle force-expire is a backstop, not the
	// primary fence clock for transient multi-broker skew.
	(void)replicated_ack2_mode;
	const uint64_t default_ms = 30000ULL;
	return default_ms * 1000ULL * 1000ULL;
}

inline uint64_t GetOrder5IdleForceExpireWindowNs(bool replicated_ack2_mode) {
	if (const char* env = std::getenv("EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_WINDOW_MS")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			return static_cast<uint64_t>(parsed) * 1000ULL * 1000ULL;
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_WINDOW_MS='" << env
		             << "'; using runtime default";
	}
	const uint64_t default_ms = replicated_ack2_mode ? 1000ULL : 500ULL;
	return default_ms * 1000ULL * 1000ULL;
}

inline void ClearOrder5PublishState(BatchHeader* hdr) {
	if (!hdr) return;
	hdr->publish_commit = kBatchHeaderPublishUncommitted;
	hdr->batch_complete = 0;
	// RETIRED (not 0): a cleared slot must stay distinguishable from a never-written tail
	// slot. If it reads as empty, a scanner resynced onto it parks forever waiting for a
	// publication that will never come, stranding every later published slot in the ring.
	__atomic_store_n(&hdr->flags, kBatchHeaderFlagRetired, __ATOMIC_RELEASE);
}

// Once an ORDER=5 batch is copied into the hold buffer, its original ring slot must stop
// looking publishable immediately. The eventual from-hold commit cannot safely clear p.hdr,
// because the ring slot may already have been reused by then.
inline void InvalidateOrder5HeldSlot(BatchHeader* hdr) {
	if (!hdr) return;
	ClearOrder5PublishState(hdr);
	CXL::store_fence();
	CXL::flush_cacheline(hdr);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(hdr) + 64);
	CXL::store_fence();
}

}  // namespace Embarcadero::topic_internal
