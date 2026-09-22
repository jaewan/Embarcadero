#include "embarlet/topic.h"
#include <iostream>
#include <stdexcept>

static void Check(bool ok) { if (!ok) throw std::runtime_error("epoch shutdown invariant"); }

int main() {
    using Embarcadero::EpochBuffer5;
    try {
        EpochBuffer5 final;
        uint64_t current = 7, next_to_extract = 7;
        Check(final.reset_and_start());
        Check(!final.CanDrainAt(next_to_extract, current));
        Check(final.enter_collection(0));
        Embarcadero::PendingBatch5 pending{};
        pending.client_id = 1001;
        pending.batch_seq = 42;
        pending.num_msg = 2;
        {
            std::lock_guard<std::mutex> lock(final.per_broker[0].mu);
            final.per_broker[0].batches.push_back(pending);
        }
        // A seal with a live collector rolls back; the new predicate must not
        // admit that buffer or drop its queued work.
        Check(!final.seal());
        Check(!final.CanDrainAt(next_to_extract, current));
        final.exit_collection(0);
        Check(final.seal());
        // Exact production predicate: shutdown seals current but opens no
        // successor. The old next_to_extract < current test stranded this work.
        Check(final.CanDrainAt(next_to_extract, current));
        Check(!final.broker_active[0].load(std::memory_order_acquire));
        Embarcadero::PendingBatch5 extracted{};
        {
            std::lock_guard<std::mutex> lock(final.per_broker[0].mu);
            Check(final.per_broker[0].batches.size() == 1);
            extracted = std::move(final.per_broker[0].batches.front());
            final.per_broker[0].batches.clear();
        }
        final.state.store(EpochBuffer5::State::IDLE, std::memory_order_release);
        ++next_to_extract;
        Check(extracted.client_id == 1001 && extracted.batch_seq == 42 && extracted.num_msg == 2);
        Check(next_to_extract > current && !final.CanDrainAt(next_to_extract, current));
        EpochBuffer5 trailing;
        Check(trailing.reset_and_start());
        current = next_to_extract;
        Check(trailing.enter_collection(0));
        trailing.exit_collection(0);
        Check(trailing.seal() && trailing.CanDrainAt(next_to_extract, current));
        std::cout << "epoch shutdown predicate with retained work passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
