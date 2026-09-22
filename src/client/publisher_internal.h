#pragma once
// Private definitions shared by publisher compilation units. Inline functions
// with cached locals have one ODR instance, not one cache/profile per source file.
#include "publisher.h"
#include "common/env_flags.h"
#include "common/config.h"
#include "network_manager/protocol.h"

namespace Embarcadero::client::detail {

using Embarcadero::network::kSessionControlMagic;
using Embarcadero::network::SessionControlHeader;
inline constexpr uint32_t kMaxSessionControlPayload = 64 * 1024;

inline uint32_t ReadSessionEpochOverride() {
	const char* env = std::getenv("EMBARCADERO_SESSION_EPOCH");
	if (!env || env[0] == '\0') return 0;
	char* end = nullptr;
	unsigned long parsed = std::strtoul(env, &end, 10);
	if (end == env || (end && *end != '\0')) return 0;
	return static_cast<uint32_t>(parsed);
}

inline bool ShouldEnableNetworkPathProfile() {
	static const bool enabled =
		Embarcadero::ReadEnvBoolLenient("EMBAR_PROFILE_NETWORK_PATH", false);
	return enabled;
}

inline uint64_t NsSince(const std::chrono::steady_clock::time_point& start) {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - start).count());
}

inline int64_t SteadyNowNs() {
	return static_cast<int64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline double RuntimeSessionRtoFloorMs() {
	static const double value = []() {
		if (const char* env = std::getenv("EMBARCADERO_SESSION_RTO_MIN_MS")) {
			char* end = nullptr;
			const double parsed = std::strtod(env, &end);
			if (end != env && *end == '\0' && parsed >= DeltaEstimator::kDeltaFloorMs &&
			    parsed <= 60000.0) {
				return parsed;
			}
			LOG(WARNING) << "Ignoring invalid EMBARCADERO_SESSION_RTO_MIN_MS='"
			             << env << "'";
		}
		const auto& runtime = Embarcadero::GetConfig().config().client.runtime;
		const std::string mode = Embarcadero::GetConfig().getRuntimeMode();
		if (mode == "failure") return static_cast<double>(runtime.session_rto_min_ms_failure.get());
		if (mode == "latency") return static_cast<double>(runtime.session_rto_min_ms_latency.get());
		return static_cast<double>(runtime.session_rto_min_ms_throughput.get());
	}();
	return value;
}

struct ClientBrokerPathProfile {
	std::atomic<uint64_t> payload_bytes{0};
	std::atomic<uint64_t> payload_send_calls{0};
	std::atomic<uint64_t> payload_eagain_events{0};
	std::atomic<uint64_t> payload_wait_ns{0};
	std::atomic<uint64_t> acked_messages{0};
};

struct ClientNetworkPathProfile {
	std::atomic<bool> logged{false};
	std::atomic<uint64_t> header_loop_ns{0};
	std::atomic<uint64_t> header_send_calls{0};
	std::atomic<uint64_t> header_send_bytes{0};
	std::atomic<uint64_t> header_send_syscall_ns{0};
	std::atomic<uint64_t> header_eagain_events{0};
	std::atomic<uint64_t> header_wait_calls{0};
	std::atomic<uint64_t> header_wait_ns{0};
	std::atomic<uint64_t> header_wait_timeouts{0};

	std::atomic<uint64_t> payload_loop_ns{0};
	std::atomic<uint64_t> payload_send_calls{0};
	std::atomic<uint64_t> payload_send_bytes{0};
	std::atomic<uint64_t> payload_send_syscall_ns{0};
	std::atomic<uint64_t> payload_eagain_events{0};
	std::atomic<uint64_t> payload_wait_calls{0};
	std::atomic<uint64_t> payload_wait_ns{0};
	std::atomic<uint64_t> payload_wait_timeouts{0};
	std::atomic<uint64_t> batches_completed{0};

	std::atomic<uint64_t> ack_recv_calls{0};
	std::atomic<uint64_t> ack_recv_syscall_ns{0};
	std::atomic<uint64_t> ack_values_processed{0};
	std::atomic<uint64_t> ack_epoll_calls{0};
	std::atomic<uint64_t> ack_epoll_wait_ns{0};
	std::atomic<uint64_t> ack_epoll_timeouts{0};

	std::array<ClientBrokerPathProfile, NUM_MAX_BROKERS> per_broker{};
};

inline ClientNetworkPathProfile& GetClientNetworkPathProfile() {
	static ClientNetworkPathProfile profile;
	return profile;
}

}  // namespace Embarcadero::client::detail
