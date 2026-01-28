#include "performance_profiler.h"

namespace Embarcadero {
namespace Profiler {

// Thread-local measurement buffers
thread_local MeasurementPoint measurements[MEASUREMENT_BUFFER_SIZE] = {};
thread_local size_t measurement_idx = 0;
thread_local size_t measurement_wrap_count = 0;

// Global profiling enable flag
std::atomic<bool> g_profiling_enabled{false};

} // namespace Profiler
} // namespace Embarcadero
