#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// This is a bounded, two-process local-CXL emulation of the media-durability
// contract used by the Scalog/LazyLog CXL replica pollers.  The shared page is
// deliberately MAP_SHARED: replication_done has the same cross-process
// visibility property as the CXL-resident frontier, while the data file models
// the replica's media sink.
struct SharedFrontier {
  std::atomic<uint64_t> replication_done;
  std::atomic<uint32_t> stage;
};

constexpr uint64_t kNoDurableBatch = 0;
constexpr uint64_t kFirstBatch = 1;
constexpr uint64_t kSecondBatch = 2;
constexpr uint32_t kSecondBatchPwriteComplete = 1;
constexpr uint32_t kSecondBatchDurable = 2;

bool WriteFullyAt(int fd, const char* data, size_t size, off_t offset) {
  size_t written = 0;
  while (written < size) {
    const ssize_t rc = pwrite(fd, data + written, size - written,
                              offset + static_cast<off_t>(written));
    if (rc <= 0) return false;
    written += static_cast<size_t>(rc);
  }
  return true;
}

bool ReadFullyAt(int fd, char* data, size_t size, off_t offset) {
  size_t read_bytes = 0;
  while (read_bytes < size) {
    const ssize_t rc = pread(fd, data + read_bytes, size - read_bytes,
                             offset + static_cast<off_t>(read_bytes));
    if (rc <= 0) return false;
    read_bytes += static_cast<size_t>(rc);
  }
  return true;
}

// Checkpoint publication is intentionally after the data fdatasync. Recovery
// trusts only this marker and truncates any uncheckpointed suffix, which is the
// conservative reconstruction rule needed after a process/power failure.
bool StoreDurableCheckpoint(int checkpoint_fd, uint64_t batch_count) {
  if (!WriteFullyAt(checkpoint_fd, reinterpret_cast<const char*>(&batch_count),
                    sizeof(batch_count), 0)) {
    return false;
  }
  return fdatasync(checkpoint_fd) == 0;
}

bool LoadDurableCheckpoint(int checkpoint_fd, uint64_t* batch_count) {
  if (batch_count == nullptr) return false;
  struct stat st {};
  if (fstat(checkpoint_fd, &st) != 0) return false;
  if (st.st_size == 0) {
    *batch_count = kNoDurableBatch;
    return true;
  }
  if (st.st_size != static_cast<off_t>(sizeof(*batch_count))) return false;
  return ReadFullyAt(checkpoint_fd, reinterpret_cast<char*>(batch_count),
                     sizeof(*batch_count), 0);
}

void WriteByteOrExit(int fd, char value) {
  if (write(fd, &value, 1) != 1) _exit(100);
}

char ReadByteOrExit(int fd) {
  char value = 0;
  if (read(fd, &value, 1) != 1) _exit(101);
  return value;
}

pid_t SpawnReplicaThatStopsAfterSecondPwrite(int data_fd, int checkpoint_fd,
                                              SharedFrontier* frontier,
                                              int ready_fd, int proceed_fd) {
  const pid_t pid = fork();
  if (pid != 0) return pid;

  constexpr char kFirstPayload[] = "acknowledged-first";
  constexpr char kSecondPayload[] = "must-redrive-second";
  const size_t first_size = sizeof(kFirstPayload) - 1;
  const size_t second_size = sizeof(kSecondPayload) - 1;

  if (!WriteFullyAt(data_fd, kFirstPayload, first_size, 0) ||
      fdatasync(data_fd) != 0 ||
      !StoreDurableCheckpoint(checkpoint_fd, kFirstBatch)) {
    _exit(102);
  }
  frontier->replication_done.store(kFirstBatch, std::memory_order_release);

  if (!WriteFullyAt(data_fd, kSecondPayload, second_size,
                    static_cast<off_t>(first_size))) {
    _exit(103);
  }
  // This is the fault-injection point: pwrite has returned but no fdatasync,
  // durable checkpoint, or ACK-visible frontier update has occurred.
  frontier->stage.store(kSecondBatchPwriteComplete, std::memory_order_release);
  WriteByteOrExit(ready_fd, 'W');

  if (ReadByteOrExit(proceed_fd) != 'S') _exit(104);
  if (fdatasync(data_fd) != 0 ||
      !StoreDurableCheckpoint(checkpoint_fd, kSecondBatch)) {
    _exit(105);
  }
  frontier->replication_done.store(kSecondBatch, std::memory_order_release);
  frontier->stage.store(kSecondBatchDurable, std::memory_order_release);
  _exit(0);
}

pid_t SpawnRedriveReplica(int data_fd, int checkpoint_fd,
                          SharedFrontier* frontier, int ready_fd) {
  const pid_t pid = fork();
  if (pid != 0) return pid;

  constexpr char kSecondPayload[] = "must-redrive-second";
  constexpr size_t kFirstSize = sizeof("acknowledged-first") - 1;
  if (!WriteFullyAt(data_fd, kSecondPayload, sizeof(kSecondPayload) - 1,
                    static_cast<off_t>(kFirstSize)) ||
      fdatasync(data_fd) != 0 ||
      !StoreDurableCheckpoint(checkpoint_fd, kSecondBatch)) {
    _exit(106);
  }
  frontier->replication_done.store(kSecondBatch, std::memory_order_release);
  frontier->stage.store(kSecondBatchDurable, std::memory_order_release);
  WriteByteOrExit(ready_fd, 'D');
  _exit(0);
}

TEST(MediaDurabilityProcessRestartTest,
     KillAfterPwriteBeforeFdatasyncDoesNotAdvanceAndRequiresRedrive) {
  char data_path[] = "/tmp/embarcadero-media-data-XXXXXX";
  char checkpoint_path[] = "/tmp/embarcadero-media-checkpoint-XXXXXX";
  const int data_fd = mkstemp(data_path);
  const int checkpoint_fd = mkstemp(checkpoint_path);
  ASSERT_NE(data_fd, -1) << strerror(errno);
  ASSERT_NE(checkpoint_fd, -1) << strerror(errno);
  ASSERT_EQ(unlink(data_path), 0);
  ASSERT_EQ(unlink(checkpoint_path), 0);

  auto* frontier = static_cast<SharedFrontier*>(mmap(
      nullptr, sizeof(SharedFrontier), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(frontier, MAP_FAILED) << strerror(errno);
  new (frontier) SharedFrontier{};
  frontier->replication_done.store(kNoDurableBatch, std::memory_order_relaxed);
  frontier->stage.store(0, std::memory_order_relaxed);

  int ready[2];
  int proceed[2];
  ASSERT_EQ(pipe(ready), 0);
  ASSERT_EQ(pipe(proceed), 0);
  const pid_t replica = SpawnReplicaThatStopsAfterSecondPwrite(
      data_fd, checkpoint_fd, frontier, ready[1], proceed[0]);
  ASSERT_GT(replica, 0) << strerror(errno);
  close(ready[1]);
  close(proceed[0]);

  char marker = 0;
  ASSERT_EQ(read(ready[0], &marker, 1), 1);
  ASSERT_EQ(marker, 'W');
  ASSERT_EQ(frontier->stage.load(std::memory_order_acquire),
            kSecondBatchPwriteComplete);
  // Batch one is acknowledged/durable; batch two was written but must never
  // become ACK-visible before the process reaches fdatasync.
  ASSERT_EQ(frontier->replication_done.load(std::memory_order_acquire),
            kFirstBatch);

  ASSERT_EQ(kill(replica, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(waitpid(replica, &status, 0), replica);
  ASSERT_TRUE(WIFSIGNALED(status));
  ASSERT_EQ(WTERMSIG(status), SIGKILL);
  EXPECT_EQ(frontier->replication_done.load(std::memory_order_acquire),
            kFirstBatch);

  uint64_t checkpoint = 0;
  ASSERT_TRUE(LoadDurableCheckpoint(checkpoint_fd, &checkpoint));
  ASSERT_EQ(checkpoint, kFirstBatch);
  constexpr off_t kFirstSize = sizeof("acknowledged-first") - 1;
  ASSERT_EQ(ftruncate(data_fd, kFirstSize), 0);
  struct stat recovered_stat {};
  ASSERT_EQ(fstat(data_fd, &recovered_stat), 0);
  ASSERT_EQ(recovered_stat.st_size, kFirstSize);
  char first[sizeof("acknowledged-first")] = {};
  ASSERT_TRUE(ReadFullyAt(data_fd, first, kFirstSize, 0));
  EXPECT_STREQ(first, "acknowledged-first");

  int redrive_ready[2];
  ASSERT_EQ(pipe(redrive_ready), 0);
  const pid_t redrive = SpawnRedriveReplica(data_fd, checkpoint_fd, frontier,
                                            redrive_ready[1]);
  ASSERT_GT(redrive, 0) << strerror(errno);
  close(redrive_ready[1]);
  ASSERT_EQ(read(redrive_ready[0], &marker, 1), 1);
  ASSERT_EQ(marker, 'D');
  ASSERT_EQ(waitpid(redrive, &status, 0), redrive);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(frontier->replication_done.load(std::memory_order_acquire),
            kSecondBatch);
  ASSERT_TRUE(LoadDurableCheckpoint(checkpoint_fd, &checkpoint));
  EXPECT_EQ(checkpoint, kSecondBatch);

  close(ready[0]);
  close(proceed[1]);
  close(redrive_ready[0]);
  close(data_fd);
  close(checkpoint_fd);
  frontier->~SharedFrontier();
  ASSERT_EQ(munmap(frontier, sizeof(SharedFrontier)), 0);
}

}  // namespace
