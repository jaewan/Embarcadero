#pragma once

#include <cstdint>
#include <string>

namespace Embarcadero {

/**
 * Authoritative ACK / replication-factor contract.
 *
 * RF includes the CXL primary (self):
 *   RF=1 -> local primary only
 *   RF=2 -> primary + one durable disk replica
 *
 * ACK levels:
 *   ACK=0 -> fire-and-forget; no broker completion guarantee
 *   ACK=1 -> payload visible in CXL and ORDER=5 GOI commit visible; independent of RF
 *   ACK=2 -> valid only for RF >= 2; waits until all required RF-1 disk replicas
 *            are media-durable
 */
inline constexpr int kMinAckLevel = 0;
inline constexpr int kMaxAckLevel = 2;
inline constexpr int kMinReplicationFactorForAck2 = 2;

struct AckRfValidation {
	bool ok{false};
	std::string error;
};

inline AckRfValidation ValidateAckReplicationPolicy(int ack_level, int replication_factor) {
	AckRfValidation result;
	if (ack_level < kMinAckLevel || ack_level > kMaxAckLevel) {
		result.error = "ack_level must be 0, 1, or 2 (got " + std::to_string(ack_level) + ")";
		return result;
	}
	if (replication_factor < 0) {
		result.error = "replication_factor must be >= 0 (got " + std::to_string(replication_factor) + ")";
		return result;
	}
	if (ack_level == 2 && replication_factor < kMinReplicationFactorForAck2) {
		result.error =
			"ACK level 2 requires replication_factor>=" +
			std::to_string(kMinReplicationFactorForAck2) +
			" (RF includes the CXL primary; got replication_factor=" +
			std::to_string(replication_factor) + ")";
		return result;
	}
	result.ok = true;
	return result;
}

inline bool IsValidAckReplicationPolicy(int ack_level, int replication_factor) {
	return ValidateAckReplicationPolicy(ack_level, replication_factor).ok;
}

/**
 * Typed ORDER=5 frontiers.
 * - InclusiveBatchHwm: last committed client batch_seq (or empty when has_prefix=false)
 * - ExclusiveNextBatchSeq: next expected client batch_seq
 * - ExclusiveMessageCount: exclusive-next message-count ACK wire value
 */
struct InclusiveBatchHwm {
	uint64_t value{0};
	bool has_prefix{false};

	static InclusiveBatchHwm Empty() { return InclusiveBatchHwm{}; }

	static InclusiveBatchHwm FromInclusive(uint64_t inclusive) {
		return InclusiveBatchHwm{inclusive, true};
	}

	static InclusiveBatchHwm FromExclusiveNext(uint64_t exclusive_next) {
		if (exclusive_next == 0) {
			return Empty();
		}
		return FromInclusive(exclusive_next - 1);
	}

	uint64_t ExclusiveNext() const {
		return has_prefix ? (value + 1) : 0;
	}
};

inline uint64_t ExclusiveNextFromInclusiveBatchHwm(uint64_t inclusive_hwm, bool has_prefix) {
	return has_prefix ? (inclusive_hwm + 1) : 0;
}

inline InclusiveBatchHwm InclusiveBatchHwmFromExclusiveNext(uint64_t exclusive_next) {
	return InclusiveBatchHwm::FromExclusiveNext(exclusive_next);
}

}  // namespace Embarcadero
