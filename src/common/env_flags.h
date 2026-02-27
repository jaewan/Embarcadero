#pragma once

#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace Embarcadero {

inline bool IsEnvFalseValue(const char* value) {
	if (!value || !value[0]) return false;
	return std::strcmp(value, "0") == 0 ||
	       std::strcmp(value, "false") == 0 ||
	       std::strcmp(value, "FALSE") == 0 ||
	       std::strcmp(value, "no") == 0 ||
	       std::strcmp(value, "NO") == 0;
}

inline bool IsEnvTrueValue(const char* value) {
	if (!value || !value[0]) return false;
	return std::strcmp(value, "1") == 0 ||
	       std::strcmp(value, "true") == 0 ||
	       std::strcmp(value, "TRUE") == 0 ||
	       std::strcmp(value, "yes") == 0 ||
	       std::strcmp(value, "YES") == 0;
}

inline bool ReadEnvBoolStrict(const char* env_name, bool default_value) {
	const char* env = std::getenv(env_name);
	if (!env) return default_value;
	if (IsEnvFalseValue(env)) return false;
	return IsEnvTrueValue(env);
}

inline bool ReadEnvBoolLenient(const char* env_name, bool default_value) {
	const char* env = std::getenv(env_name);
	if (!env) return default_value;
	return !IsEnvFalseValue(env);
}

inline uint64_t ReadEnvPositiveMsToNs(const char* env_name, uint64_t default_ns) {
	const char* env = std::getenv(env_name);
	if (!env || !env[0]) return default_ns;
	long ms = std::strtol(env, nullptr, 10);
	if (ms <= 0) return default_ns;
	return static_cast<uint64_t>(ms) * 1000ULL * 1000ULL;
}

}  // namespace Embarcadero
