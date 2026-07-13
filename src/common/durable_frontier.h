#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace Embarcadero {

// Collects out-of-order durable completions but releases only the contiguous
// source sequence prefix. Callers own synchronization around this object and
// apply the returned batches to their externally visible ACK frontier.
struct DurableBatch {
  uint32_t message_count{0};
  uint32_t client_id{0};
};

class DurableFrontier {
 public:
  bool Record(uint64_t sequence, DurableBatch batch,
              std::vector<DurableBatch>* newly_contiguous) {
    if (newly_contiguous == nullptr || batch.message_count == 0) return false;
    // A sequence that has already crossed the frontier is a retry/duplicate;
    // never retain it behind the frontier where it could consume memory or be
    // mistaken for a future completion after recovery.
    if (sequence < next_sequence_) return false;
    auto [it, inserted] = pending_.emplace(sequence, batch);
    if (!inserted) return false;
    while (true) {
      auto next = pending_.find(next_sequence_);
      if (next == pending_.end()) break;
      newly_contiguous->push_back(next->second);
      pending_.erase(next);
      ++next_sequence_;
    }
    return true;
  }

  uint64_t next_sequence() const { return next_sequence_; }
  size_t pending_size() const { return pending_.size(); }

 private:
  uint64_t next_sequence_{0};
  std::map<uint64_t, DurableBatch> pending_;
};

}  // namespace Embarcadero
