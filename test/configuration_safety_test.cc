#include <gtest/gtest.h>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <vector>
#include "common/configuration.h"
#include "common/topic_identity.h"
#include "cxl_manager/region_layout.h"

using namespace Embarcadero;

TEST(TopicIdentity, ShortAndPaddedNamesHaveTheSameStableSlot) {
    const std::string name = "orders";
    char padded[256] = "orders";
    EXPECT_EQ(TopicSlot(name.c_str(), 32), TopicSlot(padded, 32));
    EXPECT_NE(TopicSlot("orders", 32), TopicSlot("payments", 32));
    EXPECT_THROW(TopicSlot("", 32), std::invalid_argument);
}

TEST(ConfigurationSafety, ConcurrentFirstReadPublishesOneCompleteValue) {
    ConfigValue<std::string> value(std::string(8192, 'x'));
    std::atomic<bool> start{false};
    std::atomic<bool> mismatch{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 16; ++i) readers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int j = 0; j < 100; ++j)
            if (value.get() != std::string(8192, 'x')) mismatch.store(true);
    });
    start.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();
    EXPECT_FALSE(mismatch.load());
}

TEST(ConfigurationSafety, FrozenValueRejectsMutation) {
    ConfigValue<int> value(7);
    value.freeze();
    EXPECT_THROW(value.set(9), std::logic_error);
    EXPECT_EQ(value.get(), 7);
}

TEST(ConfigurationSafety, InvalidEnvironmentDoesNotSilentlyFallBack) {
    ASSERT_EQ(setenv("EMBARCADERO_TEST_CONFIG_VALUE", "123junk", 1), 0);
    ConfigValue<int> integer(7, "EMBARCADERO_TEST_CONFIG_VALUE");
    EXPECT_THROW(integer.get(), std::invalid_argument);
    ASSERT_EQ(setenv("EMBARCADERO_TEST_CONFIG_VALUE", "-1", 1), 0);
    ConfigValue<size_t> bytes(7, "EMBARCADERO_TEST_CONFIG_VALUE");
    EXPECT_THROW(bytes.get(), std::invalid_argument);
    unsetenv("EMBARCADERO_TEST_CONFIG_VALUE");
}

TEST(ConfigurationSafety, StringParsingAndInvalidGeometryRollback) {
    auto& config = Configuration::getInstance();
    ASSERT_TRUE(config.loadFromString("embarcadero: {network: {io_threads: 3}}"));
    EXPECT_EQ(config.getNetworkIOThreads(), 3);
    EXPECT_FALSE(config.loadFromString("embarcadero: {storage: {segment_size: 0}}"));
    EXPECT_GT(config.config().storage.segment_size.get(), 0U);
    EXPECT_FALSE(config.loadFromString("embarcadero: {broker: {max_brokers: 33}}"));
    EXPECT_FALSE(config.loadFromString("embarcadero: {storage: {batch_headers_size: 129}}"));
    EXPECT_FALSE(config.loadFromString("[]"));
    EXPECT_EQ(config.getNetworkIOThreads(), 3);
}

TEST(ConfigurationSafety, CliWinsOverEnvironmentAndDoesNotReloadFile) {
    ConfigValue<int> value(1, "EMBARCADERO_TEST_CONFIG_VALUE");
    ASSERT_EQ(setenv("EMBARCADERO_TEST_CONFIG_VALUE", "2", 1), 0);
    EXPECT_EQ(value.get(), 2);
    value.setOverride(3);
    EXPECT_EQ(value.get(), 3);
    unsetenv("EMBARCADERO_TEST_CONFIG_VALUE");
    auto& config = Configuration::getInstance();
    char arg0[] = "test", arg1[] = "--network_threads=4";
    char arg2[] = "--config", arg3[] = "/nonexistent", arg4[] = "-c", arg5[] = "10";
    char* argv[]{arg0, arg1, arg2, arg3, arg4, arg5};
    config.overrideFromCommandLine(6, argv);
    EXPECT_EQ(config.getNetworkIOThreads(), 4);
    EXPECT_NE(config.getCXLSize(), 10U);
}

TEST(ConfigurationSafety, FinalizePublishesImmutableSnapshot) {
    EXPECT_EXIT({
        auto& config = Configuration::getInstance();
        if (!config.finalize() || !config.frozen()) _exit(2);
        if (config.loadFromString("embarcadero: {network: {io_threads: 5}}")) _exit(3);
        _exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

TEST(RegionLayout, CapacityAndOverflowCheckedBeforeAllocation) {
    using cxl_manager::CalculateRegionLayout;
    constexpr size_t GiB = size_t{1} << 30;
    const auto a = CalculateRegionLayout(64 * GiB, GiB / 4, 5, 32, 10 * 1024 * 1024, 32 * GiB);
    const auto b = CalculateRegionLayout(64 * GiB, GiB / 2, 5, 32, 10 * 1024 * 1024, 32 * GiB);
    EXPECT_LT(a.payload_bytes, 32 * GiB);
    EXPECT_EQ(a.segment_count, a.payload_bytes / a.segment_size);
    EXPECT_GT(a.segment_count, b.segment_count);
    EXPECT_THROW(CalculateRegionLayout(32 * GiB, GiB / 4, 5, 32, 10 * 1024 * 1024, 32 * GiB), std::invalid_argument);
    EXPECT_THROW(CalculateRegionLayout(64 * GiB, 128, 5, 32, 256, 32 * GiB), std::invalid_argument);
    EXPECT_THROW(CalculateRegionLayout(64 * GiB, GiB / 4, 5, SIZE_MAX, 256, 32 * GiB), std::invalid_argument);
    EXPECT_THROW(CalculateRegionLayout(64 * GiB, 0, 5, 32, 256, 32 * GiB), std::invalid_argument);
    cxl_manager::RegionDescriptor descriptor{};
    descriptor.version = cxl_manager::kCxlLayoutVersion;
    descriptor.region_bytes = a.region_bytes;
    descriptor.metadata_bytes = a.metadata_bytes;
    descriptor.segment_size = a.segment_size;
    descriptor.pbr_bytes = 10 * 1024 * 1024;
    descriptor.brokers = 5;
    descriptor.topics = 32;
    descriptor.backend = 0;
    EXPECT_TRUE(cxl_manager::RegionDescriptorMatches(descriptor, a, 5, 32, descriptor.pbr_bytes, 0));
    EXPECT_FALSE(cxl_manager::RegionDescriptorMatches(descriptor, b, 5, 32, descriptor.pbr_bytes, 0));
    EXPECT_FALSE(cxl_manager::RegionDescriptorMatches(descriptor, a, 5, 32, descriptor.pbr_bytes, 1));
}

TEST(RegionLayout, ConcurrentBitmapClaimsUseEveryValidSlotExactlyOnce) {
    alignas(64) uint64_t bitmap[3]{};
    constexpr size_t count = 131;
    std::vector<size_t> claims;
    std::mutex results;
    std::vector<std::thread> workers;
    for (int i = 0; i < 16; ++i) workers.emplace_back([&] {
        size_t hint = 0;
        while (auto slot = cxl_manager::ClaimSegment(bitmap, count, hint)) {
            std::lock_guard<std::mutex> lock(results);
            claims.push_back(*slot);
        }
    });
    for (auto& t : workers) t.join();
    std::sort(claims.begin(), claims.end());
    ASSERT_EQ(claims.size(), count);
    for (size_t i = 0; i < count; ++i) EXPECT_EQ(claims[i], i);
    uint64_t other[1]{};
    size_t hint = 100;
    EXPECT_EQ(cxl_manager::ClaimSegment(other, 1, hint), 0U);
    EXPECT_FALSE(cxl_manager::ClaimSegment(other, 1, hint));
}
