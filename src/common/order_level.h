#pragma once

namespace Embarcadero {

inline constexpr int kOrderUnordered = 0;
inline constexpr int kOrderPerBroker = 1;
inline constexpr int kOrderTotal = 2;
inline constexpr int kOrderCorfuCompat = 3;
inline constexpr int kOrderClientBrokerStream = 4;
inline constexpr int kOrderLegacyStrong = kOrderClientBrokerStream;
inline constexpr int kOrderStrong = 5;

inline bool IsCanonicalOrderLevel(int order) {
	return order == kOrderUnordered || order == kOrderTotal ||
	       order == kOrderClientBrokerStream || order == kOrderStrong;
}

inline bool UsesEpochSequencerPath(int order) {
	return order == kOrderClientBrokerStream || order == kOrderStrong;
}

}  // namespace Embarcadero
