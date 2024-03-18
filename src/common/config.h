#include <cstdint>

/// Health check configs
/// The delay to send the first health check
const int64_t health_check_initial_delay_ms = 5000;
/// The interval between two health checks
const int64_t health_check_period_ms = 3000;
/// The timeout for a health check.
const int64_t health_check_timeout_ms = 10000;
/// The threshold for the amount of retries we do before we consider a node dead.
const int64_t health_check_failure_threshold = 5;