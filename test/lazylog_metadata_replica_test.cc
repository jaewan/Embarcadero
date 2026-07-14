#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include "../src/cxl_manager/lazylog_metadata_replica.h"

namespace {

std::string TempPath(const char* suffix) {
  const auto path = std::filesystem::temp_directory_path() /
      (std::string("embarcadero_lazylog_metadata_") + suffix + ".log");
  std::filesystem::remove(path);
  return path.string();
}

lazylogmetadata::MetadataAppendRequest Request(uint64_t source_batch_seq = 7) {
  lazylogmetadata::MetadataAppendRequest request;
  request.set_topic("lazylog-contract-test");
  request.set_source_broker_id(1);
  request.set_source_batch_seq(source_batch_seq);
  request.set_client_id(11);
  request.set_client_batch_seq(13);
  request.set_payload_offset(4096);
  request.set_payload_size(1024);
  request.set_num_messages(1);
  return request;
}

TEST(LazyLogMetadataReplicaStoreTest, DurableAppendIsIdempotentAndRejectsConflict) {
  const std::string path = TempPath("idempotent");
  LazyLog::MetadataReplicaStore store(path);
  std::string error;
  ASSERT_TRUE(store.Open(&error)) << error;

  bool already_present = false;
  ASSERT_TRUE(store.Append(Request(), &already_present, &error)) << error;
  EXPECT_FALSE(already_present);
  EXPECT_EQ(store.size(), 1u);

  ASSERT_TRUE(store.Append(Request(), &already_present, &error)) << error;
  EXPECT_TRUE(already_present);
  EXPECT_EQ(store.size(), 1u);

  auto conflict = Request();
  conflict.set_payload_size(2048);
  EXPECT_FALSE(store.Append(conflict, &already_present, &error));
  EXPECT_NE(error.find("conflicts"), std::string::npos);
  EXPECT_EQ(store.size(), 1u);
  std::filesystem::remove(path);
}

TEST(LazyLogMetadataReplicaStoreTest, RestartReplaysDurableRecords) {
  const std::string path = TempPath("restart");
  std::string error;
  {
    LazyLog::MetadataReplicaStore store(path);
    ASSERT_TRUE(store.Open(&error)) << error;
    bool already_present = false;
    ASSERT_TRUE(store.Append(Request(), &already_present, &error)) << error;
  }
  {
    LazyLog::MetadataReplicaStore store(path);
    ASSERT_TRUE(store.Open(&error)) << error;
    bool already_present = false;
    ASSERT_TRUE(store.Append(Request(), &already_present, &error)) << error;
    EXPECT_TRUE(already_present);
    EXPECT_EQ(store.size(), 1u);
  }
  std::filesystem::remove(path);
}

TEST(LazyLogMetadataReplicaStoreTest, TruncatedFinalRecordIsIgnoredOnRecovery) {
  const std::string path = TempPath("truncated");
  std::string error;
  {
    LazyLog::MetadataReplicaStore store(path);
    ASSERT_TRUE(store.Open(&error)) << error;
    bool already_present = false;
    ASSERT_TRUE(store.Append(Request(), &already_present, &error)) << error;
  }
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    const char partial[] = {0x44, 0x4d, 0x4c};
    out.write(partial, sizeof(partial));
  }
  {
    LazyLog::MetadataReplicaStore store(path);
    ASSERT_TRUE(store.Open(&error)) << error;
    EXPECT_EQ(store.size(), 1u);
    bool already_present = false;
    ASSERT_TRUE(store.Append(Request(8), &already_present, &error)) << error;
    EXPECT_FALSE(already_present);
    EXPECT_EQ(store.size(), 2u);
  }
  std::filesystem::remove(path);
}

TEST(LazyLogMetadataReplicaStoreTest, FailedSyncDoesNotAdvanceDurableMetadataFrontier) {
  const std::string path = TempPath("sync-failure");
  std::string error;
  {
    LazyLog::MetadataReplicaStore store(path);
    ASSERT_TRUE(store.Open(&error)) << error;
    setenv("EMBARCADERO_FDATASYNC_FAIL", "1", 1);
    bool already_present = false;
    EXPECT_FALSE(store.Append(Request(), &already_present, &error));
    EXPECT_EQ(store.size(), 0u);
  }
  unsetenv("EMBARCADERO_FDATASYNC_FAIL");
  {
    // A redrive must recover the descriptor once, rather than admitting a
    // second logical record after a failed acknowledgement.
    LazyLog::MetadataReplicaStore store(path);
    ASSERT_TRUE(store.Open(&error)) << error;
    bool already_present = false;
    ASSERT_TRUE(store.Append(Request(), &already_present, &error)) << error;
    EXPECT_TRUE(already_present);
    EXPECT_EQ(store.size(), 1u);
  }
  std::filesystem::remove(path);
}

TEST(LazyLogMetadataReplicaStoreTest, SyncStallDelaysDurabilityAcknowledgement) {
  const std::string path = TempPath("sync-stall");
  std::string error;
  LazyLog::MetadataReplicaStore store(path);
  ASSERT_TRUE(store.Open(&error)) << error;
  setenv("EMBARCADERO_FDATASYNC_STALL_MS", "40", 1);
  bool already_present = false;
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(store.Append(Request(), &already_present, &error)) << error;
  const auto elapsed = std::chrono::steady_clock::now() - start;
  unsetenv("EMBARCADERO_FDATASYNC_STALL_MS");
  EXPECT_FALSE(already_present);
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 30);
  std::filesystem::remove(path);
}

TEST(LazyLogMetadataReplicaClientTest, FanoutRequiresEveryReplicaAndPersistsEverySuccess) {
  const std::string first_path = TempPath("fanout-first");
  const std::string second_path = TempPath("fanout-second");
  std::string error;
  {
    LazyLog::LazyLogMetadataReplicaServer first("127.0.0.1:0", first_path);
    LazyLog::LazyLogMetadataReplicaServer second("127.0.0.1:0", second_path);
    ASSERT_TRUE(first.Start(&error)) << error;
    ASSERT_TRUE(second.Start(&error)) << error;

    LazyLog::LazyLogMetadataReplicaClient client({first.endpoint(), second.endpoint()});
    ASSERT_TRUE(client.AppendToAll(Request(), &error)) << error;
    first.Shutdown();
    second.Shutdown();
  }
  {
    LazyLog::MetadataReplicaStore first(first_path);
    ASSERT_TRUE(first.Open(&error)) << error;
    EXPECT_EQ(first.size(), 1u);
    LazyLog::MetadataReplicaStore second(second_path);
    ASSERT_TRUE(second.Open(&error)) << error;
    EXPECT_EQ(second.size(), 1u);
  }
  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
}

TEST(LazyLogMetadataReplicaClientTest, FailedReplicaPreventsAppendCompletion) {
  const std::string path = TempPath("fanout-failure");
  std::string error;
  LazyLog::LazyLogMetadataReplicaServer server("127.0.0.1:0", path);
  ASSERT_TRUE(server.Start(&error)) << error;
  LazyLog::LazyLogMetadataReplicaClient client(
      {server.endpoint(), "127.0.0.1:1"}, std::chrono::milliseconds(50), 1);
  EXPECT_FALSE(client.AppendToAll(Request(), &error));
  EXPECT_FALSE(error.empty());
  server.Shutdown();
  std::filesystem::remove(path);
}

TEST(LazyLogMetadataReplicaClientTest, RetriesTransportFailureUntilReplicaStarts) {
  const std::string path = TempPath("retry");
  const std::string reservation_path = TempPath("retry-reservation");
  std::string error;

  // Reserve a kernel-selected port, then release it so the first client attempt
  // sees a transport failure and a later retry can reach the real replica.
  LazyLog::LazyLogMetadataReplicaServer reservation("127.0.0.1:0", reservation_path);
  ASSERT_TRUE(reservation.Start(&error)) << error;
  const std::string endpoint = reservation.endpoint();
  reservation.Shutdown();

  LazyLog::LazyLogMetadataReplicaClient client(
      {endpoint}, std::chrono::milliseconds(100), 4, std::chrono::milliseconds(50));
  auto append = std::async(std::launch::async, [&] { return client.AppendToAll(Request(), &error); });
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  LazyLog::LazyLogMetadataReplicaServer server(endpoint, path);
  ASSERT_TRUE(server.Start(&error)) << error;
  EXPECT_TRUE(append.get()) << error;
  server.Shutdown();
  std::filesystem::remove(path);
  std::filesystem::remove(reservation_path);
}

}  // namespace
