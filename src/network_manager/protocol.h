#pragma once

#include <cstddef>
#include <cstdint>

namespace Embarcadero {

enum ClientRequestType {Publish, Subscribe};

// Existing native wire ABI shared by the broker and protocol-test driver.
struct alignas(64) EmbarcaderoReq {
    uint32_t client_id;
    uint32_t ack;
    size_t num_msg;
    void* last_addr;
    uint32_t port;
    ClientRequestType client_req;
    char topic[32];
};
static_assert(sizeof(EmbarcaderoReq) == 64);
static_assert(offsetof(EmbarcaderoReq, topic) == 32);

namespace network {
inline constexpr uint32_t kSessionControlMagic = 0x53455346U;
struct SessionControlHeader {
    uint32_t magic;
    uint32_t length;
};
static_assert(sizeof(SessionControlHeader) == 8);
}  // namespace network
}  // namespace Embarcadero
