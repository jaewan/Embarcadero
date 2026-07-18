#include "client/corfu_ordered_token_gate.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

bool OrderedAcquisition() {
  CorfuOrderedTokenGate gate;
  std::atomic<bool> shutdown{false};
  const auto first = gate.Acquire(0, shutdown);
  if (!Expect(first.acquired, "ticket 0 must acquire first")) return false;

  auto second = std::async(std::launch::async, [&] { return gate.Acquire(1, shutdown); });
  if (!Expect(second.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
              "ticket 1 must wait while ticket 0 is held")) return false;
  if (!Expect(gate.Complete(0, shutdown), "ticket 0 completion must succeed")) return false;
  if (!Expect(second.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
              "ticket 1 must wake after ticket 0 completes")) return false;
  const auto result = second.get();
  return Expect(result.acquired, "ticket 1 must acquire next") &&
         Expect(gate.Complete(1, shutdown), "ticket 1 completion must succeed") &&
         Expect(gate.AdmitPayload(shutdown), "healthy gate must admit payload");
}

bool DuplicateTicketPoisonsGate() {
  CorfuOrderedTokenGate gate;
  std::atomic<bool> shutdown{false};
  if (!Expect(gate.Acquire(0, shutdown).acquired, "first ticket 0 must acquire")) return false;
  const auto duplicate = gate.Acquire(0, shutdown);
  const auto snapshot = gate.GetSnapshot();
  return Expect(!duplicate.acquired, "duplicate held ticket must be denied") &&
         Expect(snapshot.aborted, "duplicate held ticket must poison gate") &&
         Expect(snapshot.abort_transitions == 1, "gate must record one abort transition") &&
         Expect(!gate.Complete(0, shutdown), "holder completion after poison must fail") &&
         Expect(!gate.AdmitPayload(shutdown), "poisoned gate must deny payload");
}

bool AbortWhileHolderActive() {
  CorfuOrderedTokenGate gate;
  std::atomic<bool> shutdown{false};
  if (!Expect(gate.Acquire(0, shutdown).acquired, "holder must acquire")) return false;
  auto waiter = std::async(std::launch::async, [&] { return gate.Acquire(1, shutdown); });
  if (!Expect(gate.Abort("injected token RPC race"), "first abort must transition")) return false;
  if (!Expect(!gate.Abort("second abort"), "abort must be idempotent")) return false;
  if (!Expect(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
              "abort must wake waiter")) return false;
  return Expect(!waiter.get().acquired, "waiter must fail after abort") &&
         Expect(!gate.Complete(0, shutdown), "late successful grant must be burned") &&
         Expect(!gate.AdmitPayload(shutdown), "late grant must not admit payload");
}

bool ShutdownWakesWaiter() {
  CorfuOrderedTokenGate gate;
  std::atomic<bool> shutdown{false};
  if (!Expect(gate.Acquire(0, shutdown).acquired, "holder must acquire before shutdown")) return false;
  auto waiter = std::async(std::launch::async, [&] { return gate.Acquire(1, shutdown); });
  shutdown.store(true, std::memory_order_release);
  if (!Expect(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
              "bounded CV wait must observe shutdown")) return false;
  const auto snapshot = gate.GetSnapshot();
  return Expect(!waiter.get().acquired, "shutdown waiter must be denied") &&
         Expect(snapshot.aborted, "shutdown must poison gate") &&
         Expect(!gate.Complete(0, shutdown), "shutdown must reject holder completion");
}

}  // namespace

int main() {
  if (!OrderedAcquisition() || !DuplicateTicketPoisonsGate() ||
      !AbortWhileHolderActive() || !ShutdownWakesWaiter()) {
    return 1;
  }
  std::cout << "corfu_ordered_token_gate_test: PASS\n";
  return 0;
}
