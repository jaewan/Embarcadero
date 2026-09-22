#include "embarlet/single_topic_admission.h"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// REQUIRE stays active in Release builds.
#define REQUIRE(condition) do { if (!(condition)) std::abort(); } while (0)

int main() {
  Embarcadero::SingleTopicAdmission gate;
  REQUIRE(!gate.TryAdmit(""));
  REQUIRE(!gate.TryAdmit(std::string(256, 'x')));
  REQUIRE(!gate.TryAdmit(std::string_view("a\0b", 3)));
  REQUIRE(gate.TryAdmit("orders"));
  REQUIRE(gate.TryAdmit("orders"));
  REQUIRE(!gate.TryAdmit("payments"));
  REQUIRE(gate.TryAdmit("orders"));

  // These are the actual production admission transitions, raced across
  // distinct names. At most one name can proceed to manager allocation.
  Embarcadero::SingleTopicAdmission concurrent;
  std::atomic<bool> start{false};
  std::atomic<int> admitted{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 32; ++i) threads.emplace_back([&, i] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    const std::string name = "topic-" + std::to_string(i);
    if (concurrent.TryAdmit(name)) {
      admitted.fetch_add(1);
      for (int n = 0; n < 100; ++n) REQUIRE(concurrent.TryAdmit(name));
    }
  });
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();
  REQUIRE(admitted.load() == 1);
  REQUIRE(!concurrent.TryAdmit("another-region-topic"));
  std::cout << "single topic admission tests passed\n";
}
