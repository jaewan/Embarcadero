#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "../src/disk_manager/scalog_replication_client.h"
#include "../src/disk_manager/scalog_replication_manager.h"

namespace {

constexpr int kPort = 29173;

std::string TempLog() {
  return "/tmp/embarcadero_scalog_rpc_durability_" + std::to_string(getpid()) + ".dat";
}

class ScalogReplicationRpcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    unsetenv("EMBARCADERO_FDATASYNC_FAIL");
    unsetenv("EMBARCADERO_FDATASYNC_STALL_MS");
    std::filesystem::remove(log_path_);
  }

  void TearDown() override {
    unsetenv("EMBARCADERO_FDATASYNC_FAIL");
    unsetenv("EMBARCADERO_FDATASYNC_STALL_MS");
    std::filesystem::remove(log_path_);
  }

  const std::string log_path_{TempLog()};
};

TEST_F(ScalogReplicationRpcTest, ReplyFollowsDurableWrite) {
  Scalog::ScalogReplicationManager server(
      /*broker_id=*/0, /*log_to_memory=*/true, "127.0.0.1", std::to_string(kPort),
      log_path_);
  Scalog::ScalogReplicationClient client("TestTopic", 2, "127.0.0.1", 0, kPort);
  ASSERT_TRUE(client.Connect(2));

  std::vector<char> payload(4096, 'x');
  setenv("EMBARCADERO_FDATASYNC_STALL_MS", "80", 1);
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(client.ReplicateData(0, payload.size(), 4, payload.data(), 0));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
  unsetenv("EMBARCADERO_FDATASYNC_STALL_MS");
  EXPECT_GE(elapsed, 60);
}

TEST_F(ScalogReplicationRpcTest, SyncFailureFailsClosed) {
  Scalog::ScalogReplicationManager server(
      /*broker_id=*/0, /*log_to_memory=*/true, "127.0.0.1", std::to_string(kPort),
      log_path_);
  Scalog::ScalogReplicationClient client("TestTopic", 2, "127.0.0.1", 0, kPort);
  ASSERT_TRUE(client.Connect(2));

  std::vector<char> payload(4096, 'y');
  setenv("EMBARCADERO_FDATASYNC_FAIL", "1", 1);
  EXPECT_FALSE(client.ReplicateData(0, payload.size(), 4, payload.data(), 0));
  unsetenv("EMBARCADERO_FDATASYNC_FAIL");

  // A subsequent redrive is admitted only after native fdatasync succeeds.
  EXPECT_TRUE(client.ReplicateData(0, payload.size(), 4, payload.data(), 0));
}

}  // namespace
