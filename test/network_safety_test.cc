#include <gtest/gtest.h>
#include <future>
#include <thread>
#include <unistd.h>
#include "common/cancellable_queue.h"
#include "network_manager/framing.h"
#include "network_manager/batch_validation.h"

using namespace Embarcadero;
using namespace std::chrono_literals;

TEST(ConnectionQueue, CloseUnblocksFullProducerAndReturnsOwnedRequests) {
    CancellableQueue<int> queue(1);
    ASSERT_TRUE(queue.push(7));
    auto blocked = std::async(std::launch::async, [&] { return queue.push(8); });
    EXPECT_EQ(blocked.wait_for(20ms), std::future_status::timeout);
    const auto remaining = queue.close();
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(remaining.front(), 7);
    ASSERT_EQ(blocked.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(blocked.get());
    EXPECT_TRUE(queue.close().empty());
}

TEST(ConnectionQueue, CloseUnblocksEmptyConsumer) {
    CancellableQueue<int> queue(1);
    auto blocked = std::async(std::launch::async, [&] { int item; return queue.pop(item); });
    queue.close();
    ASSERT_EQ(blocked.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(blocked.get());
}

TEST(NetworkFraming, FragmentedControlPrefixIsNotLegacy) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    const uint32_t magic = 0x12345678;
    auto reader = std::async(std::launch::async, [&] { return network::PeekControlPrefix(sockets[1], magic); });
    const auto* bytes = reinterpret_cast<const char*>(&magic);
    for (size_t i = 0; i < sizeof(magic); ++i) {
        ASSERT_EQ(send(sockets[0], bytes + i, 1, MSG_NOSIGNAL), 1);
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(reader.get(), network::PrefixResult::control);
    uint32_t read{};
    EXPECT_TRUE(network::ReceiveExact(sockets[1], &read, sizeof(read)));
    EXPECT_EQ(read, magic); // peeking did not consume bytes
    close(sockets[0]); close(sockets[1]);
}

TEST(NetworkFraming, TruncatedPrefixFailsClosed) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    ASSERT_EQ(send(sockets[0], "x", 1, MSG_NOSIGNAL), 1);
    shutdown(sockets[0], SHUT_WR);
    EXPECT_EQ(network::PeekControlPrefix(sockets[1], 42, 20ms), network::PrefixResult::invalid);
    close(sockets[0]); close(sockets[1]);
}

TEST(BatchValidation, RejectsTruncationOverflowAndTrailingBytesBeforePublication) {
    alignas(64) uint8_t data[256]{};
    auto* first = reinterpret_cast<BlogMessageHeader*>(data);
    first->size = 64;
    auto* second = reinterpret_cast<BlogMessageHeader*>(data + 128);
    second->size = 64;
    EXPECT_TRUE(network::ValidateBatchBody(data, sizeof(data), 2, true));
    EXPECT_FALSE(network::ValidateBatchBody(data, sizeof(data) - 1, 2, true));
    EXPECT_FALSE(network::ValidateBatchBody(data, sizeof(data), 1, true));
    first->size = UINT32_MAX;
    EXPECT_FALSE(network::ValidateBatchBody(data, sizeof(data), 2, true));
    MessageHeader header{};
    header.paddedSize = 32;
    std::memcpy(data, &header, sizeof(header));
    EXPECT_FALSE(network::ValidateBatchBody(data, sizeof(data), 2, false));
}

TEST(BatchValidation, EnvelopeIsBoundedBeforeAllocationOrDrain) {
    BatchHeader batch{};
    batch.total_size = 128;
    batch.num_msg = 1;
    batch.client_id = 9;
    EXPECT_TRUE(network::ValidBatchEnvelope(batch, 128, 9));
    EXPECT_FALSE(network::ValidBatchEnvelope(batch, 128, 10));
    batch.total_size = SIZE_MAX;
    EXPECT_FALSE(network::ValidBatchEnvelope(batch, 128, 9));
    batch.total_size = 128;
    batch.num_msg = 3;
    EXPECT_FALSE(network::ValidBatchEnvelope(batch, 128, 9));
}
