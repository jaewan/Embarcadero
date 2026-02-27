#pragma once

namespace Embarcadero {

inline constexpr int kOrderUnordered = 0;
inline constexpr int kOrderPerBroker = 1;
inline constexpr int kOrderTotal = 2;
inline constexpr int kOrderCorfuCompat = 3;
inline constexpr int kOrderLegacyStrong = 4;
inline constexpr int kOrderStrong = 5;

inline bool IsCanonicalOrderLevel(int order) {
	return order == kOrderUnordered || order == kOrderTotal || order == kOrderStrong;
}

inline bool IsLegacyOrder4(int order) {
	return order == kOrderLegacyStrong;
}

inline bool IsStrongOrderingLevelCompat(int order) {
	return order == kOrderStrong || IsLegacyOrder4(order);
}

inline int CanonicalizeOrderLevelCompat(int order) {
	return IsLegacyOrder4(order) ? kOrderStrong : order;
}

}  // namespace Embarcadero
