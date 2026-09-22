#pragma once

#include <cstddef>
#include <cstdint>

namespace Embarcadero {

// Internal shared encoding/hash rules; preserve existing table probe identity.
inline uint64_t MakeSessionKey(size_t client_id, uint32_t session_epoch) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(client_id)) << 32) |
	       static_cast<uint64_t>(session_epoch);
}

inline uint64_t Mix64(uint64_t x) {
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return x;
}

inline constexpr uint64_t kSessionEntryFlagFenced = 1ULL << 0;
inline constexpr uint64_t kSessionEntryFlagActive = 1ULL << 1;

}  // namespace Embarcadero
