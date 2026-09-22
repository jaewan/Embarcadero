#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace Embarcadero::fault {

// Test-only context. Details are observations captured under the caller's
// production synchronization, never an invitation to mutate shared state.
struct Context {
    uint64_t client_id = UINT64_MAX;
    uint64_t epoch = UINT64_MAX;
    uint64_t batch_seq = UINT64_MAX;
    uint64_t detail0 = 0;
    uint64_t detail1 = 0;
};

#if defined(EMBARCADERO_ENABLE_FAULT_INJECTION) && EMBARCADERO_ENABLE_FAULT_INJECTION
// Owns a duplicate of one inherited AF_UNIX/SOCK_SEQPACKET endpoint. A
// controller disconnect cancels all pauses. Commands and events are bounded;
// unarmed hooks do not send events or wait. No TCP control listener exists.
class Controller {
public:
    Controller(int fd, const std::string& run_token);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    bool Pause(const char* name, Context context = {},
               const std::atomic<bool>* stop = nullptr, uint64_t* injected_value = nullptr);
    void Cancel();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool Pause(const char* name, Context context = {},
           const std::atomic<bool>* stop = nullptr, uint64_t* injected_value = nullptr);
#else
// Normal builds neither link the controller nor evaluate an environment switch
// in a hot path. Keep Context construction free of side effects at call sites.
inline bool Pause(const char*, Context = {}, const std::atomic<bool>* = nullptr,
                  uint64_t* = nullptr) { return true; }
#endif

}  // namespace Embarcadero::fault
