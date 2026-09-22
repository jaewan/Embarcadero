#include "production_fault_support.h"
#include "network_manager/framing.h"

namespace Embarcadero::testdriver {
void SelfTest() {
    int pair[2];
    Require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair failed");
    Fd left(pair[0]), right(pair[1]);
    Batch batch(kPrefixClient, 7, 42);
    // This fixture is smaller than a socketpair send buffer, so it needs no
    // background writer that could outlive an exception.
    batch.Publish(left.value);
    BatchHeader received{};
    Receive(right.value, &received, sizeof(received));
    std::vector<uint8_t> body(received.total_size);
    Receive(right.value, body.data(), body.size());
    Require(received.batch_seq == 7 && received.client_id == kPrefixClient && body == batch.body,
            "native batch transport changed identity");
    Require(network::ValidBatchEnvelope(received, body.size(), kPrefixClient) &&
                network::ValidateBatchBody(body.data(), body.size(), kMessagesPerBatch, true),
            "valid fixture rejected by production validation");
    Require(!network::ValidBatchEnvelope(received, body.size(), kBadClient), "wrong identity accepted");
    for (int kind = 0; kind < 4; ++kind) {
        auto invalid_envelope = received;
        if (kind == 0) invalid_envelope.num_msg = 0;
        if (kind == 1) invalid_envelope.total_size = 0;
        if (kind == 2) invalid_envelope.total_size = std::numeric_limits<size_t>::max();
        if (kind == 3) invalid_envelope.num_msg = std::numeric_limits<size_t>::max();
        Require(!network::ValidBatchEnvelope(invalid_envelope, body.size(), kPrefixClient),
                "zero/oversized/overflow envelope boundary accepted");
    }
    uint32_t invalid = std::numeric_limits<uint32_t>::max();
    std::memcpy(body.data(), &invalid, sizeof(invalid));
    Require(!network::ValidateBatchBody(body.data(), body.size(), kMessagesPerBatch, true), "bad fixture accepted");
    // Exercise the actual bounded framing helpers at every incomplete native
    // control-header boundary. These socketpair checks allocate no region.
    network::SessionControlHeader control{network::kSessionControlMagic, 4};
    for (size_t length = 0; length < sizeof(control); ++length) {
        int sockets[2];
        Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
        Fd sender(sockets[0]), receiver(sockets[1]);
        Send(sender.value, &control, length);
        Require(shutdown(sender.value, SHUT_WR) == 0, "socketpair half-close failed");
        network::SessionControlHeader parsed{};
        Require(!network::ReceiveExact(receiver.value, &parsed, sizeof(parsed), std::chrono::milliseconds(20)),
                "truncated native control header accepted");
    }
    const auto protobuf = OpenPayload(kPrefixClient);
    for (size_t length = 0; length < protobuf.size(); ++length) {
        int sockets[2];
        Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
        Fd sender(sockets[0]), receiver(sockets[1]);
        Send(sender.value, protobuf.data(), length);
        Require(shutdown(sender.value, SHUT_WR) == 0, "socketpair half-close failed");
        std::string parsed(protobuf.size(), '\0');
        Require(!network::ReceiveExact(receiver.value, parsed.data(), parsed.size(), std::chrono::milliseconds(20)),
                "truncated protobuf payload accepted as a complete frame");
    }
    std::cout << "native protocol driver self-test passed" << std::endl;
}
}  // namespace Embarcadero::testdriver
