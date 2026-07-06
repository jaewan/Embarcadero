#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace {

uint32_t ComputeLazyLogExportedMessageCount(size_t previous_offset,
                                            size_t new_offset,
                                            bool had_previous_addr) {
    return had_previous_addr
        ? static_cast<uint32_t>(new_offset - previous_offset)
        : static_cast<uint32_t>(new_offset + 1);
}

const char* ResolveLazyLogSequencerIpForTest(const char* env_ip, const char* config_ip) {
    if (env_ip != nullptr && env_ip[0] != '\0') {
        return env_ip;
    }
    if (config_ip != nullptr && config_ip[0] != '\0') {
        return config_ip;
    }
    return "127.0.0.1";
}

TEST(LazyLogLatencyInvariantTest, FirstOrderedRangeExportsAllMessagesSeen) {
    EXPECT_EQ(ComputeLazyLogExportedMessageCount(0, 9, false), 10u);
}

TEST(LazyLogLatencyInvariantTest, LaterOrderedRangeExportsDeltaOnly) {
    EXPECT_EQ(ComputeLazyLogExportedMessageCount(99, 149, true), 50u);
}

TEST(LazyLogLatencyInvariantTest, EmptySequencerIpFallsBackToLoopback) {
    EXPECT_STREQ(ResolveLazyLogSequencerIpForTest("", ""), "127.0.0.1");
    EXPECT_STREQ(ResolveLazyLogSequencerIpForTest(nullptr, ""), "127.0.0.1");
}

TEST(LazyLogLatencyInvariantTest, NonEmptySequencerIpWinsOverFallback) {
    EXPECT_STREQ(ResolveLazyLogSequencerIpForTest("10.10.10.10", "127.0.0.1"), "10.10.10.10");
    EXPECT_STREQ(ResolveLazyLogSequencerIpForTest("", "10.10.10.11"), "10.10.10.11");
}

}  // namespace
