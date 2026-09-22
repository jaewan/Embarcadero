#include "common/fault_injection.h"

#if defined(EMBARCADERO_ENABLE_FAULT_INJECTION) && EMBARCADERO_ENABLE_FAULT_INJECTION
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace Embarcadero::fault {
namespace {
constexpr size_t kMaxPacket = 512;
constexpr size_t kMaxArms = 64;
bool Number(const std::string& value, uint64_t& result) {
    if (value.empty()) return false;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
bool Word(const std::string& value) {
    if (value.empty() || value.size() > 96) return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}
bool Selector(const std::string& value, uint64_t& result) {
    if (value == "*") { result = UINT64_MAX; return true; }
    return Number(value, result);
}
}  // namespace

struct Controller::Impl {
    struct Arm {
        std::string name;
        Context selector;
        uint64_t value = 0;
        bool hit = false;
        bool released = false;
    };
    int fd = -1;
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::map<uint64_t, std::shared_ptr<Arm>> arms;
    bool started = false;
    std::thread reader;

    void Cancel() {
        cancelled.store(true, std::memory_order_release);
        cv.notify_all();
    }
    bool Send(const std::string& packet) {
        const auto sent = send(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(packet.size())) { Cancel(); return false; }
        return true;
    }
    bool Command(const std::string& packet) {
        std::istringstream input(packet);
        std::string command, id_text, extra;
        input >> command;
        if (command == "CANCEL") {
            if (input >> extra) return false;
            Cancel();
            return true;
        }
        if (command == "START") {
            if (input >> extra) return false;
            std::lock_guard<std::mutex> lock(mutex);
            if (started) return false;
            started = true;
            cv.notify_all();
            return Send("STARTED");
        }
        uint64_t id = 0;
        if (!(input >> id_text) || !Number(id_text, id)) return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (command == "RELEASE") {
            if (input >> extra) return false;
            const auto found = arms.find(id);
            // Release before HIT is a protocol error, not a skipped schedule.
            if (found == arms.end() || !found->second->hit || found->second->released) return false;
            found->second->released = true;
            cv.notify_all();
            return Send("RELEASED " + id_text);
        }
        if (command != "ARM" || arms.size() >= kMaxArms || arms.count(id)) return false;
        auto arm = std::make_shared<Arm>();
        std::string client, epoch, batch, value;
        if (!(input >> arm->name >> client >> epoch >> batch >> value) || (input >> extra) ||
            !Word(arm->name) || !Selector(client, arm->selector.client_id) ||
            !Selector(epoch, arm->selector.epoch) || !Selector(batch, arm->selector.batch_seq) ||
            !Number(value, arm->value)) return false;
        const auto overlap = [](uint64_t a, uint64_t b) {
            return a == UINT64_MAX || b == UINT64_MAX || a == b;
        };
        for (const auto& [unused_id, existing] : arms) {
            (void)unused_id;
            if (!existing->hit && existing->name == arm->name &&
                overlap(existing->selector.client_id, arm->selector.client_id) &&
                overlap(existing->selector.epoch, arm->selector.epoch) &&
                overlap(existing->selector.batch_seq, arm->selector.batch_seq)) return false;
        }
        arms.emplace(id, std::move(arm));
        return Send("ARMED " + id_text);
    }
    void Run() {
        while (!cancelled.load(std::memory_order_acquire)) {
            pollfd pfd{fd, POLLIN, 0};
            const int ready = poll(&pfd, 1, 20);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (pfd.revents & (POLLERR | POLLNVAL))) break;
            if (ready == 0) continue;
            std::array<char, kMaxPacket + 1> buffer{};
            const auto received = recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT | MSG_TRUNC);
            if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (received <= 0 || static_cast<size_t>(received) > kMaxPacket ||
                !Command(std::string(buffer.data(), received))) {
                Send("ERROR invalid-or-closed-control");
                break;
            }
        }
        Cancel();
    }
};

Controller::Controller(int fd, const std::string& token) : impl_(new Impl) {
    int type = 0, domain = 0;
    socklen_t length = sizeof(int);
    if (!Word(token) || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 ||
        type != SOCK_SEQPACKET || getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &length) != 0 ||
        domain != AF_UNIX)
        throw std::invalid_argument("fault control requires a local SOCK_SEQPACKET FD and bounded run token");
    impl_->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (impl_->fd < 0) throw std::runtime_error("cannot duplicate fault control FD");
    if (!impl_->Send("READY " + token)) {
        close(impl_->fd);
        throw std::runtime_error("cannot announce fault control readiness");
    }
    try { impl_->reader = std::thread([this] { impl_->Run(); }); }
    catch (...) { close(impl_->fd); throw; }
}

Controller::~Controller() {
    Cancel();
    if (impl_->reader.joinable()) impl_->reader.join();
    close(impl_->fd);
}
void Controller::Cancel() { impl_->Cancel(); }

bool Controller::Pause(const char* name, Context context, const std::atomic<bool>* stop,
                       uint64_t* injected_value) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!impl_->started && !impl_->cancelled.load(std::memory_order_acquire) &&
           !(stop && stop->load(std::memory_order_acquire))) {
        if (std::chrono::steady_clock::now() >= startup_deadline) { impl_->Cancel(); break; }
        impl_->cv.wait_for(lock, std::chrono::milliseconds(20));
    }
    if (impl_->cancelled.load(std::memory_order_acquire) || (stop && stop->load(std::memory_order_acquire))) return false;
    for (auto& [id, arm] : impl_->arms) {
        const auto matches = [](uint64_t selected, uint64_t actual) {
            return selected == UINT64_MAX || selected == actual;
        };
        if (arm->hit || arm->name != name || !matches(arm->selector.client_id, context.client_id) ||
            !matches(arm->selector.epoch, context.epoch) || !matches(arm->selector.batch_seq, context.batch_seq)) continue;
        arm->hit = true;
        std::ostringstream event;
        event << "HIT " << id << ' ' << name << ' ' << context.client_id << ' ' << context.epoch << ' '
              << context.batch_seq << ' ' << context.detail0 << ' ' << context.detail1
              << ' ' << syscall(SYS_gettid);
        if (!impl_->Send(event.str())) return false;
        while (!arm->released && !impl_->cancelled.load(std::memory_order_acquire) &&
               !(stop && stop->load(std::memory_order_acquire)))
            impl_->cv.wait_for(lock, std::chrono::milliseconds(20));
        const bool proceed = arm->released && !impl_->cancelled.load(std::memory_order_acquire) &&
                             !(stop && stop->load(std::memory_order_acquire));
        if (proceed && injected_value) *injected_value = arm->value;
        // Keep the used ID as a tombstone: arms are bounded and one-shot even
        // if a caller reuses an ID after the target resumes.
        return proceed;
    }
    return true;
}

bool Pause(const char* name, Context context, const std::atomic<bool>* stop, uint64_t* value) {
    static std::unique_ptr<Controller> controller = []() -> std::unique_ptr<Controller> {
        const char* descriptor = std::getenv("EMBARCADERO_FAULT_CONTROL_FD");
        if (!descriptor) return nullptr;
        uint64_t fd = 0;
        const char* token = std::getenv("EMBARCADERO_FAULT_CONTROL_TOKEN");
        if (!Number(descriptor, fd) || fd > INT_MAX || !token)
            throw std::invalid_argument("invalid inherited fault controller settings");
        auto result = std::make_unique<Controller>(static_cast<int>(fd), token);
        close(static_cast<int>(fd));
        return result;
    }();
    return !controller || controller->Pause(name, context, stop, value);
}
}  // namespace Embarcadero::fault
#endif
