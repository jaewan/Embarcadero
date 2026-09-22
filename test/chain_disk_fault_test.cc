// Link only this test with --wrap=pwrite,--wrap=fdatasync,--wrap=close.
// The actual ChainReplicationManager runs against a tiny owned regular file.
// These are syscall/error-ordering tests, not persistent-media qualification.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/performance_utils.h"
#include "disk_manager/chain_replication.h"

extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" int __real_fdatasync(int);
extern "C" int __real_close(int);

namespace {
using namespace Embarcadero;
using namespace std::chrono_literals;
enum class Fault { ShortWrite, WriteEio, SyncEio, SuccessfulSync, InterruptedWrite };

struct Boundary {
    std::string directory;
    Fault fault{Fault::SuccessfulSync};
    std::atomic<unsigned> writes{0}, syncs{0};
    std::atomic<bool> closed{false};
    std::atomic<ssize_t> short_result{-1};
    std::mutex mutex;
    std::condition_variable condition;
    bool reached{false}, released{false};

    bool Owns(int fd) const {
        char path[PATH_MAX];
        const std::string link = "/proc/self/fd/" + std::to_string(fd);
        const ssize_t length = ::readlink(link.c_str(), path, sizeof(path));
        return length > 0 && static_cast<size_t>(length) < sizeof(path) &&
            std::string(path, static_cast<size_t>(length)) == directory + "/replica_b0_src0.dat";
    }
    void Pause() {
        std::unique_lock<std::mutex> lock(mutex);
        reached = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
    }
    bool Await() {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, 3s, [&] { return reached; });
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};
std::atomic<Boundary*> active{nullptr};

class Environment {
public:
    Environment(const char* name, const std::string& value) : name_(name) {
        if (const char* before = std::getenv(name)) before_ = before;
        if (::setenv(name, value.c_str(), 1)) throw std::runtime_error("setenv failed");
    }
    ~Environment() {
        if (before_) ::setenv(name_.c_str(), before_->c_str(), 1);
        else ::unsetenv(name_.c_str());
    }
private:
    std::string name_;
    std::optional<std::string> before_;
};

template<class Predicate> bool Eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return predicate();
}

class ChainDiskFault : public ::testing::Test {
protected:
    struct alignas(128) Region {
        ControlBlock control{};
        alignas(128) std::array<std::array<unsigned char, 128>, 2> payload{};
    } region_;
    static_assert(offsetof(Region, control) == 0);
    alignas(128) GOIEntry goi_[2]{};
    alignas(128) CompletionVectorEntry completion_[NUM_MAX_BROKERS]{};
    Boundary boundary_;
    std::vector<std::unique_ptr<Environment>> environment_;
    std::unique_ptr<ChainReplicationManager> manager_;
    const bool previous_flush_{CXL::ExplicitFlushRequired()};

    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "embarcadero-chain-disk-XXXXXX").string();
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back('\0');
        ASSERT_NE(::mkdtemp(path.data()), nullptr);
        boundary_.directory = path.data();
        for (const auto& entry : std::vector<std::pair<const char*, std::string>>{
                 {"EMBARCADERO_REPLICA_DISK_DIRS", boundary_.directory},
                 {"EMBARCADERO_CHAIN_REPLICATION_SINK", "disk-durable"},
                 {"EMBARCADERO_CHAIN_SYNC_INTERVAL_MS", "1"},
                 {"EMBARCADERO_CHAIN_SYNC_BYTES", "4096"},
                 {"EMBARCADERO_SYNC_SLEEP_MS", "0"},
                 {"EMBARCADERO_CXL_COHERENT", "1"}})
            environment_.push_back(std::make_unique<Environment>(entry.first, entry.second));
        CXL::SetExplicitFlushRequired(false);
        region_.control.epoch.store(1);
        region_.control.committed_seq.store(0);
        completion_[0].completed_pbr_head.store(UINT64_MAX);
        for (size_t i = 0; i < 2; ++i) {
            region_.payload[i].fill(static_cast<unsigned char>(0x41 + i));
            auto& entry = goi_[i];
            entry.global_seq = i;
            entry.broker_id = 0;
            entry.epoch_sequenced = 1;
            entry.blog_offset = reinterpret_cast<unsigned char*>(region_.payload[i].data()) -
                                reinterpret_cast<unsigned char*>(&region_);
            entry.payload_size = region_.payload[i].size();
            entry.message_count = 2;
            entry.pbr_index = i;
            entry.cumulative_message_count = 2 * (i + 1);
        }
        active.store(&boundary_, std::memory_order_release);
    }

    void TearDown() override {
        boundary_.Release();  // Also unblocks early ASSERT failures.
        if (manager_) manager_->Stop();
        active.store(nullptr, std::memory_order_release);
        CXL::SetExplicitFlushRequired(previous_flush_);
        if (!boundary_.directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(boundary_.directory, error);
            EXPECT_FALSE(error);
            EXPECT_FALSE(std::filesystem::exists(boundary_.directory));
        }
    }

    void CheckPrefix() {
        EXPECT_EQ(goi_[0].num_replicated.load(std::memory_order_acquire), 1u);
        EXPECT_EQ(goi_[1].num_replicated.load(std::memory_order_acquire), 0u);
        EXPECT_EQ(completion_[0].completed_logical_offset.load(std::memory_order_acquire), 2u);
        EXPECT_EQ(completion_[0].completed_pbr_head.load(std::memory_order_acquire), 0u);
    }

    void Run(Fault fault) {
        boundary_.fault = fault;
        manager_ = std::make_unique<ChainReplicationManager>(0, 1, 0, 1,
            &region_, goi_, completion_, boundary_.directory);
        ASSERT_TRUE(manager_->config().IsMediaDurable());
        manager_->Start();
        ASSERT_TRUE(Eventually([&] {
            return completion_[0].completed_logical_offset.load(std::memory_order_acquire) == 2;
        })) << "real first pwrite+fdatasync must establish a durable-prefix control";
        ASSERT_EQ(boundary_.writes.load(), 1u);
        ASSERT_EQ(boundary_.syncs.load(), 1u);
        ASSERT_FALSE(boundary_.closed.load());
        CheckPrefix();

        region_.control.committed_seq.store(1, std::memory_order_release);
        ASSERT_TRUE(boundary_.Await()) << "selected actual syscall boundary was never reached";
        CheckPrefix();  // Cannot ACK the second task before its sink/sync completes.
        boundary_.Release();
        const bool failure = fault == Fault::ShortWrite || fault == Fault::WriteEio || fault == Fault::SyncEio;
        if (failure) {
            // Observe automatic manager cleanup BEFORE calling Stop. Otherwise
            // test-induced cancellation could conceal erroneous success handling.
            ASSERT_TRUE(Eventually([&] { return boundary_.closed.load(std::memory_order_acquire); }))
                << "production error path did not fail-stop and close its owned replica file";
            CheckPrefix();
            EXPECT_EQ(boundary_.writes.load(), 2u);
            EXPECT_EQ(boundary_.syncs.load(), fault == Fault::SyncEio ? 2u : 1u);
        } else {
            ASSERT_TRUE(Eventually([&] {
                return completion_[0].completed_logical_offset.load(std::memory_order_acquire) == 4;
            })) << "successful sync must advance the real token and completion frontier";
            EXPECT_EQ(goi_[1].num_replicated.load(), 1u);
            EXPECT_EQ(completion_[0].completed_pbr_head.load(), 1u);
            EXPECT_EQ(boundary_.writes.load(), fault == Fault::InterruptedWrite ? 3u : 2u);
            EXPECT_EQ(boundary_.syncs.load(), 2u);
        }
        const auto stop_start = std::chrono::steady_clock::now();
        manager_->Stop();
        EXPECT_LT(std::chrono::steady_clock::now() - stop_start, 3s);
        EXPECT_TRUE(boundary_.closed.load());

        const auto file = std::filesystem::path(boundary_.directory) / "replica_b0_src0.dat";
        const size_t expected_size = fault == Fault::WriteEio ? 128 : fault == Fault::ShortWrite ? 192 : 256;
        ASSERT_EQ(std::filesystem::file_size(file), expected_size);
        std::ifstream stream(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
        ASSERT_EQ(bytes.size(), expected_size);
        EXPECT_EQ(bytes.substr(0, 128), std::string(128, 'A'));
        EXPECT_EQ(bytes.substr(128), std::string(expected_size - 128, 'B'));
        if (fault == Fault::ShortWrite) EXPECT_EQ(boundary_.short_result.load(), 64);
    }
};
}  // namespace

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t size, off_t offset) {
    Boundary* boundary = active.load(std::memory_order_acquire);
    if (!boundary || !boundary->Owns(fd)) return __real_pwrite(fd, data, size, offset);
    const unsigned call = boundary->writes.fetch_add(1) + 1;
    if (call == 2 && (boundary->fault == Fault::ShortWrite || boundary->fault == Fault::WriteEio ||
                      boundary->fault == Fault::InterruptedWrite)) {
        boundary->Pause();
        if (boundary->fault == Fault::ShortWrite) {
            // Real bytes reach the owned file, then the production code sees a
            // genuine short return. Its current contract is fail-closed, not retry.
            const ssize_t written = __real_pwrite(fd, data, size / 2, offset);
            boundary->short_result.store(written);
            return written;
        }
        errno = boundary->fault == Fault::InterruptedWrite ? EINTR : EIO;
        return -1;
    }
    return __real_pwrite(fd, data, size, offset);
}

extern "C" int __wrap_fdatasync(int fd) {
    Boundary* boundary = active.load(std::memory_order_acquire);
    if (!boundary || !boundary->Owns(fd)) return __real_fdatasync(fd);
    const unsigned call = boundary->syncs.fetch_add(1) + 1;
    if (call == 2 && (boundary->fault == Fault::SyncEio || boundary->fault == Fault::SuccessfulSync)) {
        boundary->Pause();
        if (boundary->fault == Fault::SyncEio) { errno = EIO; return -1; }
    }
    return __real_fdatasync(fd);
}

extern "C" int __wrap_close(int fd) {
    Boundary* boundary = active.load(std::memory_order_acquire);
    const bool owned = boundary && boundary->Owns(fd);
    const int result = __real_close(fd);
    if (owned && result == 0) boundary->closed.store(true, std::memory_order_release);
    return result;
}

TEST_F(ChainDiskFault, ShortWriteFailsClosedWithoutAcknowledgingPartialBytes) { Run(Fault::ShortWrite); }
TEST_F(ChainDiskFault, WriteEioPreservesTheSyncedPrefix) { Run(Fault::WriteEio); }
TEST_F(ChainDiskFault, SyncEioDoesNotAcknowledgeWrittenButUnsyncedBytes) { Run(Fault::SyncEio); }
TEST_F(ChainDiskFault, SuccessfulSyncAdvancesOnlyAfterTheRealBoundary) { Run(Fault::SuccessfulSync); }
TEST_F(ChainDiskFault, InterruptedWriteRetriesBeforeSuccessfulSync) { Run(Fault::InterruptedWrite); }
