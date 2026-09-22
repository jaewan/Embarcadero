#include "session_fault_scenarios.h"

namespace Embarcadero::testdriver {
namespace {
void CheckSessionPrefix(const RegionObserver& region, size_t prefix, bool fenced, bool control) {
    const size_t count = prefix + (control ? 1 : 0);
    const uint64_t expected = count ? count - 1 : UINT64_MAX;
    const auto deadline = After();
    while (region.Committed() != expected && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(region.Committed() == expected, "GOI committed prefix differs from exact oracle");
    for (size_t i = 0; i < count; ++i) {
        const auto* entry = region.At<GOIEntry>(kGOIOffset + i * sizeof(GOIEntry));
        Require(entry->global_seq == i && entry->client_id == (i < prefix ? kPrefixClient : kControlClient) &&
                entry->client_seq == (i < prefix ? i : 0) && entry->session_epoch == 1 &&
                entry->total_order == i * kMessagesPerBatch && entry->message_count == kMessagesPerBatch,
                "GOI committed an unapproved identity/order");
    }
    const auto slot = TopicSlot(kTopic.c_str(), Configuration::getInstance().config().storage.max_topics.get());
    const size_t table_offset = region.layout.metadata_bytes - region.layout.session_table_bytes +
                               slot * kMaxSessions * sizeof(SessionEntry);
    bool found = false;
    for (size_t i = 0; i < kMaxSessions; ++i) {
        const auto* entry = region.At<SessionEntry>(table_offset + i * sizeof(SessionEntry));
        if (entry->session_key.load(std::memory_order_acquire) != (uint64_t{kPrefixClient} << 32 | 1)) continue;
        found = true;
        Require(entry->expected_seq.load(std::memory_order_acquire) == prefix &&
                entry->committed_hwm.load(std::memory_order_acquire) == (prefix ? prefix - 1 : 0) &&
                static_cast<bool>(entry->state_word.load(std::memory_order_acquire) & 1) == fenced,
                "authoritative session prefix/fence mismatch");
    }
    Require(found, "authoritative session not found");
    // CV sequencer_logical_offset retires physical ingress slots, including fenced
    // work. It is not the client committed-message frontier. Record separately.
    const auto* cv = region.At<CompletionVectorEntry>(kCompletionVectorOffset);
    std::cout << "[SESSION_PREFIX] batches=" << prefix << " fenced=" << fenced
              << " control=" << control << " goi=" << region.Committed()
              << " cv_ingress_consumed=" << cv->sequencer_logical_offset.load(std::memory_order_acquire)
              << std::endl;
}

void AwaitFence(PublisherSocket& publisher, size_t prefix) {
    const auto deadline = After();
    while (true) {
        uint64_t first = 0;
        Receive(publisher.ack.value, &first, sizeof(first), deadline);
        if (static_cast<uint32_t>(first) != network::kSessionControlMagic) {
            Require(first == prefix * kMessagesPerBatch, "fenced session falsely advanced ACK");
            continue;
        }
        const uint32_t size = static_cast<uint32_t>(first >> 32);
        Require(size > 0 && size <= 65536, "invalid fence envelope");
        std::string bytes(size, '\0');
        Receive(publisher.ack.value, bytes.data(), size, deadline);
        embarcadero::session::SessionFenced fence;
        Require(fence.ParseFromString(bytes) && fence.has_committed_prefix() == (prefix > 0) &&
                (!prefix || fence.committed_batch_seq() == prefix - 1) &&
                fence.committed_msg_hwm() == prefix * kMessagesPerBatch &&
                fence.reason() == embarcadero::session::SessionFenced::HOLD_EXPIRY,
                "fence reply includes uncommitted work or wrong prefix");
        return;
    }
}
}  // namespace

void RunSessionFault(const Options& options, RegionObserver& region) {
    const bool before = options.test_case == "fence_before_commit";
    const bool empty = options.test_case == "fence_empty_prefix";
    Require(before || empty || options.test_case == "commit_before_fence", "unknown session fault case");
    const size_t prefix_count = empty ? 0 : before ? 2 : 3;
    CreateTopic(options);
    auto subscriber = Connect(options.data_port);
    const auto subscribe = Handshake(2001, 0, Subscribe);
    Send(subscriber.value, &subscribe, sizeof(subscribe));
    PublisherSocket publisher(options, kPrefixClient);
    for (size_t i = 0; i < prefix_count; ++i) Batch(kPrefixClient, i, i * 2).Publish(publisher.ingress.value);
    if (prefix_count) publisher.AwaitAck(prefix_count * kMessagesPerBatch);
    for (size_t i = 0; i < prefix_count; ++i) AuditBatch(subscriber.value, i);
    CheckSessionPrefix(region, prefix_count, false, false);
    Stage(options, "session_prefix_verified");
    Continue(options);  // Controller holds epoch seal before sending the selected inputs.
    if (before) Batch(kPrefixClient, 2, 10000).Publish(publisher.ingress.value);
    Batch(kPrefixClient, empty ? 1 : 4, 20000).Publish(publisher.ingress.value);
    Stage(options, "session_fault_sent");
    AwaitFence(publisher, prefix_count);
    CheckSessionPrefix(region, prefix_count, true, false);

    // A fresh connection asks for the same epoch. OPEN must preserve its fence
    // and report only initialized committed GOI work; no local classifier guess.
    auto reopened = Connect(options.data_port);
    const auto request = Handshake(kPrefixClient, publisher.listener.port);
    Send(reopened.value, &request, sizeof(request));
    const auto answer = Open(reopened.value, kPrefixClient, options, false, 1, false);
    Require(answer.status() == embarcadero::session::SessionOpenAck::FENCED &&
            answer.has_committed_prefix() == (prefix_count > 0) &&
            (!prefix_count || answer.committed_hwm() == prefix_count - 1),
            "old epoch OPEN unfenced or reports uncommitted prefix");
    PublisherSocket control(options, kControlClient);
    Batch(kControlClient, 0, prefix_count * 2).Publish(control.ingress.value);
    control.AwaitAck(2);
    AuditBatch(subscriber.value, prefix_count);
    CheckSessionPrefix(region, prefix_count, true, true);
    Require(!Wait(subscriber.value, POLLIN, After(100)), "fenced work was delivered");
    std::cout << "[FAULT_RESULT] status=passed case=" << options.test_case
              << " messages=" << (prefix_count + 1) * kMessagesPerBatch
              << " exact_prefix=1 false_ack=0 fenced_open=1 recovery_control=1" << std::endl;
}
}  // namespace Embarcadero::testdriver
