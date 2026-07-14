#pragma once

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <unistd.h>

namespace Embarcadero {

// Test-only deterministic media-fault seam. Neither variable is set by a
// launcher by default: production calls take the native fdatasync path.
inline int DurableFdatasync(int fd) {
  if (const char* stall_ms = std::getenv("EMBARCADERO_FDATASYNC_STALL_MS")) {
    char* end = nullptr;
    const long delay = std::strtol(stall_ms, &end, 10);
    if (end != stall_ms && *end == '\0' && delay > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
  }
  if (const char* fail = std::getenv("EMBARCADERO_FDATASYNC_FAIL");
      fail != nullptr && fail[0] == '1' && fail[1] == '\0') {
    errno = EIO;
    return -1;
  }
  return fdatasync(fd);
}

}  // namespace Embarcadero
