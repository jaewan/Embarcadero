#include "cxl_manager/shared_mapping.h"

#include <functional>
#include <csignal>
#include <sys/resource.h>
#include <iostream>
#include <sys/wait.h>

using namespace Embarcadero::cxl_manager;

static void Check(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}
static void Reject(const std::function<void()>& action, const char* text) {
    try { action(); } catch (const std::exception& e) {
        Check(std::string(e.what()).find(text) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error("expected rejection");
}

int main() {
    try {
        const rlimit no_core{0, 0};
        Check(setrlimit(RLIMIT_CORE, &no_core) == 0, "disable test core dumps");
        const size_t bytes = 4 * static_cast<size_t>(sysconf(_SC_PAGESIZE));
        const int fd = memfd_create("shared-mapping-test", MFD_CLOEXEC);
        Check(fd >= 0 && ftruncate(fd, bytes) == 0, "memfd creation");
        auto* view = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        Check(view != MAP_FAILED, "descriptor view");
        SharedMapping view_owner(view, bytes);
        auto* descriptor = new (static_cast<char*>(view) + kRegionDescriptorOffset) RegionDescriptor{};
        descriptor->version = 7;
        descriptor->region_bytes = bytes;
        auto validate = [&](const RegionDescriptor& d) {
            if (d.version != 7 || d.region_bytes != bytes)
                throw std::runtime_error("descriptor mismatch before attachment");
        };
        auto invalidate = [](const RegionDescriptor*) {};

        // Reserve real occupied pages, and verify BOTH the NOREPLACE and
        // compatibility hint-only branch preserve their data and protections.
        auto* sentinel = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(sentinel != MAP_FAILED, "sentinel allocation");
        SharedMapping sentinel_owner(sentinel, bytes);
        memset(sentinel, 0xa5, bytes);
        Check(mprotect(sentinel, bytes, PROT_READ) == 0, "sentinel read protection");
        const auto occupied = reinterpret_cast<uintptr_t>(sentinel);
        for (bool noreplace : {true, false}) {
            Reject([&] { SelectAndMapShared(fd, bytes, false, {occupied}, noreplace); }, "without replacing");
            for (size_t i = 0; i < bytes; ++i)
                Check(static_cast<unsigned char*>(sentinel)[i] == 0xa5, "sentinel overwritten");
            const auto pid = fork();
            Check(pid >= 0, "protection fork");
            if (pid == 0) {
                signal(SIGSEGV, SIG_DFL);
                *static_cast<volatile unsigned char*>(sentinel) = 0;
                _exit(1);
            }
            int status = 0;
            Check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
                WTERMSIG(status) == SIGSEGV, "sentinel protection changed");
        }

        // Pick two vacant addresses from a disposable reservation; collision at
        // the first head preference forces fallback to the second. The follower
        // must discover that second base rather than independently selecting.
        auto* spare = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(spare != MAP_FAILED, "vacant candidate");
        const auto chosen = reinterpret_cast<uintptr_t>(spare);
        munmap(spare, bytes);
        SharedMapping head(SelectAndMapShared(fd, bytes, false, {occupied, chosen}), bytes);
        Check(reinterpret_cast<uintptr_t>(head.get()) == chosen, "head fallback");
        descriptor->mapping_base = chosen;
        Reject([&] { DiscoverPublishedBase(fd, bytes, {}, validate, invalidate,
                     std::chrono::milliseconds(0)); }, "not ready");
        descriptor->ready.store(kRegionReady, std::memory_order_release);
        Check(DiscoverPublishedBase(fd, bytes, {}, validate, invalidate) == chosen, "head discovery");
        Reject([&] { DiscoverPublishedBase(fd, bytes, occupied, validate, invalidate); }, "conflicts");
        descriptor->version = 8;
        Reject([&] { DiscoverPublishedBase(fd, bytes, {}, validate, invalidate); }, "descriptor mismatch");
        descriptor->version = 7;

        // Separate process uses the production discovery and exact mapping.
        // Its inherited head mapping is removed, just as independent startup
        // has no head mapping. A second child keeps that mapping to test safe
        // follower rejection without modifying shared descriptor/data.
        for (bool collision : {false, true}) {
            const auto pid = fork();
            Check(pid >= 0, "follower fork");
            if (pid == 0) {
                try {
                    if (!collision) munmap(head.get(), bytes);
                    const auto base = DiscoverPublishedBase(fd, bytes, {}, validate, invalidate);
                    if (collision) {
                        Reject([&] { SelectAndMapShared(fd, bytes, false, {base}); }, "without replacing");
                    } else {
                        SharedMapping follower(SelectAndMapShared(fd, bytes, false, {base}), bytes);
                        Check(reinterpret_cast<uintptr_t>(follower.get()) == chosen, "follower base");
                    }
                    _exit(0);
                } catch (...) { _exit(1); }
            }
            int status = 0;
            Check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "follower outcome");
            Check(descriptor->ready.load() == kRegionReady && descriptor->mapping_base == chosen,
                "failed follower changed head descriptor");
        }
        for (const auto value : {"-1", " 4096", "0", "garbage", "0x10000000000000000"})
            Reject([&] { ParseMappingAddress(value); }, "invalid EMBARCADERO");
        Reject([&] { MapExactShared(fd, bytes, 17, false); }, "page aligned");
        Reject([&] { DiscoverPublishedBase(fd, 1, {}, validate, invalidate); }, "too small");
        close(fd);
        std::cout << "shared mapping tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
