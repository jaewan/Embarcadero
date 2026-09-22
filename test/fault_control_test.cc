#include "common/fault_injection.h"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <gtest/gtest.h>

namespace {
using Embarcadero::fault::Context;
using Embarcadero::fault::Controller;
struct CancelOnExit { Controller& controller; ~CancelOnExit() { controller.Cancel(); } };
class ControlTest : public ::testing::Test {
protected:
    int fds[2]{-1, -1};
    std::unique_ptr<Controller> controller;
    void SetUp() override {
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds), 0);
        controller = std::make_unique<Controller>(fds[1], "owned-test");
        close(fds[1]); fds[1] = -1;
        ASSERT_EQ(Read(), "READY owned-test");
    }
    void TearDown() override {
        controller->Cancel(); controller.reset();
        if (fds[0] >= 0) close(fds[0]);
    }
    void Send(const std::string& value) {
        ASSERT_EQ(send(fds[0], value.data(), value.size(), MSG_NOSIGNAL), static_cast<ssize_t>(value.size()));
    }
    std::string Read() {
        pollfd pfd{fds[0], POLLIN, 0};
        if (poll(&pfd, 1, 2000) != 1) throw std::runtime_error("missing control event");
        std::array<char, 1024> data{};
        const auto bytes = recv(fds[0], data.data(), data.size(), 0);
        if (bytes <= 0) throw std::runtime_error("closed control endpoint");
        return std::string(data.data(), bytes);
    }
    void Start() { Send("START"); ASSERT_EQ(Read(), "STARTED"); }
};

TEST_F(ControlTest, FirstHookCannotOutrunArmingAndOnlyMatchingIdentityPauses) {
    uint64_t injected = 0;
    auto task = std::async(std::launch::async, [&] {
        return controller->Pause("commit.before", Context{7, 3, 9, 11, 12}, nullptr, &injected);
    });
    CancelOnExit cancel{*controller};
    EXPECT_EQ(task.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
    Send("ARM 1 commit.before 7 3 9 123"); ASSERT_EQ(Read(), "ARMED 1");
    Start();
    ASSERT_EQ(Read(), "HIT 1 commit.before 7 3 9 11 12");
    EXPECT_EQ(task.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
    Send("RELEASE 1"); EXPECT_EQ(Read(), "RELEASED 1");
    EXPECT_TRUE(task.get()); EXPECT_EQ(injected, 123u);
    EXPECT_TRUE(controller->Pause("commit.before", Context{7, 3, 9}));
}
TEST_F(ControlTest, NonmatchingCallDoesNotConsumeAnArm) {
    Send("ARM 1 point 7 3 9 0"); ASSERT_EQ(Read(), "ARMED 1"); Start();
    EXPECT_TRUE(controller->Pause("point", Context{8, 3, 9}));
    auto task = std::async(std::launch::async, [&] { return controller->Pause("point", Context{7, 3, 9}); });
    CancelOnExit cancel{*controller};
    ASSERT_EQ(Read(), "HIT 1 point 7 3 9 0 0");
    Send("RELEASE 1"); EXPECT_EQ(Read(), "RELEASED 1"); EXPECT_TRUE(task.get());
}
TEST_F(ControlTest, ShutdownCancelsPauseWithoutReleaseOrFalseSuccess) {
    Send("ARM 1 blocked * * * 0"); ASSERT_EQ(Read(), "ARMED 1"); Start();
    std::atomic<bool> stop{false};
    auto task = std::async(std::launch::async, [&] { return controller->Pause("blocked", {}, &stop); });
    CancelOnExit cancel{*controller};
    ASSERT_EQ(Read().substr(0, 13), "HIT 1 blocked");
    stop.store(true);
    EXPECT_EQ(task.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(task.get());
}
TEST_F(ControlTest, ControllerLossCancelsAnArmedHook) {
    Send("ARM 1 blocked * * * 0"); ASSERT_EQ(Read(), "ARMED 1"); Start();
    auto task = std::async(std::launch::async, [&] { return controller->Pause("blocked"); });
    CancelOnExit cancel{*controller};
    ASSERT_EQ(Read().substr(0, 13), "HIT 1 blocked");
    close(fds[0]); fds[0] = -1;
    EXPECT_EQ(task.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(task.get());
}
TEST_F(ControlTest, EarlyReleaseAndOverlappingSelectorsFailClosed) {
    Send("ARM 1 blocked * 3 * 0"); ASSERT_EQ(Read(), "ARMED 1");
    Send("ARM 2 blocked 7 * * 0"); EXPECT_EQ(Read(), "ERROR invalid-or-closed-control");
    EXPECT_FALSE(controller->Pause("blocked"));
}
TEST_F(ControlTest, ReleaseBeforeHitIsNotSuccess) {
    Send("ARM 1 blocked * * * 0"); ASSERT_EQ(Read(), "ARMED 1");
    Send("RELEASE 1"); EXPECT_EQ(Read(), "ERROR invalid-or-closed-control");
    EXPECT_FALSE(controller->Pause("blocked"));
}
TEST_F(ControlTest, OversizedPacketsDoNotTruncateIntoValidCommands) {
    Send("ARM 1 blocked * * * 0" + std::string(600, ' '));
    EXPECT_EQ(Read(), "ERROR invalid-or-closed-control");
    EXPECT_FALSE(controller->Pause("blocked"));
}
}  // namespace
