#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include "common/fault_injection.h"

namespace Embarcadero::network {
enum class PrefixResult { legacy, control, invalid };

// Preserve legacy bytes, but never classify a fragmented control prefix as a
// legacy batch. A bounded deadline also rejects truncated negotiation.
inline PrefixResult PeekControlPrefix(int fd, uint32_t magic,
        std::chrono::milliseconds timeout = std::chrono::seconds(2),
        const std::atomic<bool>* stop = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        uint32_t prefix{};
        const auto n = recv(fd, &prefix, sizeof(prefix), MSG_PEEK | MSG_DONTWAIT);
        if (n == sizeof(prefix)) return prefix == magic ? PrefixResult::control : PrefixResult::legacy;
        if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return PrefixResult::invalid;
#if defined(EMBARCADERO_ENABLE_FAULT_INJECTION) && EMBARCADERO_ENABLE_FAULT_INJECTION
        if (n > 0 && !fault::Pause("ingress.session_prefix.partial",
                {UINT64_MAX, UINT64_MAX, UINT64_MAX, static_cast<uint64_t>(n), sizeof(prefix)}, stop))
            return PrefixResult::invalid;
#else
        (void)stop;
#endif
        // POLLIN stays asserted for a partial peek; sleep rather than spin.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return PrefixResult::invalid;
}

inline bool ReceiveExact(int fd, void* data, size_t size,
        std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto* bytes = static_cast<uint8_t*>(data);
    size_t done = 0;
    while (done < size && std::chrono::steady_clock::now() < deadline) {
        const auto n = recv(fd, bytes + done, size - done, MSG_DONTWAIT);
        if (n > 0) { done += static_cast<size_t>(n); continue; }
        if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
        pollfd p{fd, POLLIN, 0};
        poll(&p, 1, 10);
    }
    return done == size;
}
}  // namespace Embarcadero::network
