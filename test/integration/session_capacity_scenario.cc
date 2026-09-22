#include "session_capacity_scenario.h"

#include <atomic>
#include <future>
#include <set>

namespace Embarcadero::testdriver {
namespace {
Fd CapacityConnection(const Options& options) {
    auto connection = Connect(options.data_port);
    const auto handshake = Handshake(kPrefixClient, 0, Publish, 0);
    Send(connection.value, &handshake, sizeof(handshake));
    return connection;
}

void CheckCapacityTable(const RegionObserver& region) {
    const auto slot = TopicSlot(kTopic.c_str(), Configuration::getInstance().config().storage.max_topics.get());
    const auto start = region.layout.metadata_bytes - region.layout.session_table_bytes +
                       slot * kMaxSessions * sizeof(SessionEntry);
    std::set<uint64_t> identities;
    for (size_t i = 0; i < kMaxSessions; ++i) {
        const auto* entry = region.At<SessionEntry>(start + i * sizeof(SessionEntry));
        const auto key = entry->session_key.load(std::memory_order_acquire);
        const auto epoch = static_cast<uint32_t>(key);
        Require(key >> 32 == kPrefixClient && epoch >= 1 && epoch <= kMaxSessions,
                "session capacity table contains unexpected or empty identity");
        Require(identities.insert(key).second, "duplicate session identity consumed more than one slot");
        Require(entry->expected_seq.load(std::memory_order_acquire) == (epoch == 1 ? 2u : 0u),
                "OPEN-only identity changed committed progress");
        if (epoch == 1)
            Require(entry->committed_hwm.load(std::memory_order_acquire) == 1,
                    "capacity overflow changed original committed prefix");
    }
    Require(identities.size() == kMaxSessions, "authoritative table is not exactly full");
}
}  // namespace

void RunSessionCapacity(const Options& options, RegionObserver& region) {
    static_assert(kMaxSessions == 4096, "scenario must exercise actual configured table capacity");
    CreateTopic(options);
    PublisherSocket prefix(options, kPrefixClient);
    Batch(kPrefixClient, 0, 0).Publish(prefix.ingress.value);
    Batch(kPrefixClient, 1, 2).Publish(prefix.ingress.value);
    prefix.AwaitAck(4);
    region.CheckPrefix(2);

    // One short-lived ACK0 connection at a time, so OPEN fills authoritative
    // identities without allocating thousands of ACK sockets/threads.
    const auto fill_deadline = After(45000);
    for (uint32_t epoch = 2; epoch < kMaxSessions; ++epoch) {
        Require(Clock::now() < fill_deadline, "session fill exceeded its 45-second budget");
        auto connection = CapacityConnection(options);
        Open(connection.value, kPrefixClient, options, false, epoch);
    }
    Stage(options, "session_last_slot_race");
    {
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::array<std::future<void>, 4> contenders;
        // Releases every waiting contender before future destructors join on
        // any exception, including thread creation or pre-barrier socket error.
        struct Release {
            std::atomic<bool>& flag;
            ~Release() { flag.store(true, std::memory_order_release); }
        } release{go};
        for (auto& contender : contenders) {
            contender = std::async(std::launch::async, [&] {
                auto connection = CapacityConnection(options);
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                Open(connection.value, kPrefixClient, options, false, kMaxSessions);
            });
        }
        const auto deadline = After();
        while (ready.load(std::memory_order_acquire) != contenders.size() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(ready.load(std::memory_order_acquire) == contenders.size(), "session race failed to reach start barrier");
        go.store(true, std::memory_order_release);
        for (auto& contender : contenders) contender.get();
    }
    {
        auto extra = CapacityConnection(options);
        const auto rejected = Open(extra.value, kPrefixClient, options, false, kMaxSessions + 1, false);
        Require(rejected.status() == embarcadero::session::SessionOpenAck::RESOURCE_EXHAUSTED &&
                    rejected.assigned_session_epoch() == kMaxSessions + 1 && !rejected.has_committed_prefix(),
                "session table overflow was not rejected by authoritative admission");
        ExpectClose(extra.value);
    }
    {
        auto existing = CapacityConnection(options);
        const auto reopened = Open(existing.value, kPrefixClient, options, false, 1, false);
        Require(reopened.status() == embarcadero::session::SessionOpenAck::OK &&
                    reopened.assigned_session_epoch() == 1 && reopened.has_committed_prefix() &&
                    reopened.committed_hwm() == 1,
                "full table failed to reopen the original committed identity");
    }
    CheckCapacityTable(region);
    region.CheckPrefix(2);
    prefix.RejectExtraAck();
    auto subscriber = Connect(options.data_port);
    const auto subscribe = Handshake(2001, 0, Subscribe);
    Send(subscriber.value, &subscribe, sizeof(subscribe));
    AuditBatch(subscriber.value, 0);
    AuditBatch(subscriber.value, 1);
    Require(!Wait(subscriber.value, POLLIN, After(100)), "OPEN-only sessions produced extra delivery");
    region.CheckPrefix(2);
    std::cout << "[FAULT_RESULT] status=passed case=" << options.test_case
              << " messages=4 sessions=4096 distinct_keys=4096 last_slot_contenders=4"
              << " overflow_rejected=1 committed_reopen=1 exact_replay=1 false_ack=0" << std::endl;
}
}  // namespace Embarcadero::testdriver
