#include "storage_fault_scenarios.h"
#include <set>

namespace Embarcadero::testdriver {
namespace {
constexpr size_t kLargeMessages = 480;
constexpr size_t kMainBatches = 340;
constexpr uint32_t kLateClient = 1003;
struct LargeBatch {
    BatchHeader header{};
    std::vector<uint8_t> body;
    LargeBatch(uint32_t client, uint64_t sequence, uint64_t first_identity) {
        header.client_id = client; header.batch_seq = sequence;
        header.original_client_batch_seq = sequence; header.session_epoch = 1;
        header.num_msg = kLargeMessages;
        header.total_size = kLargeMessages * wire::ComputeStrideV2(kPayloadBytes);
        header.start_logical_offset = sizeof(BatchHeader);
        body.resize(header.total_size);
        for (size_t i = 0; i < kLargeMessages; ++i) {
            BlogMessageHeader message{};
            message.size = kPayloadBytes; message.client_id = client; message.batch_seq = sequence;
            const size_t offset = i * wire::ComputeStrideV2(kPayloadBytes);
            std::memcpy(body.data() + offset, &message, sizeof(message));
            const auto payload = Payload(first_identity + i);
            std::memcpy(body.data() + offset + sizeof(message), payload.data(), payload.size());
        }
    }
    void Publish(int fd) const { Send(fd, &header, sizeof(header)); Send(fd, body.data(), body.size()); }
};

void AuditLarge(int fd, size_t ordinal) {
    wire::BatchMetadata metadata{};
    Receive(fd, &metadata, sizeof(metadata));
    Require(metadata.header_version == wire::HEADER_VERSION_V2 && metadata.flags == 0 &&
            metadata.num_messages == kLargeMessages && metadata.batch_total_order == ordinal * kLargeMessages,
            "rollover subscriber metadata mismatch");
    std::vector<uint8_t> body(kLargeMessages * wire::ComputeStrideV2(kPayloadBytes));
    Receive(fd, body.data(), body.size());
    for (size_t i = 0; i < kLargeMessages; ++i) {
        const size_t offset = i * wire::ComputeStrideV2(kPayloadBytes);
        BlogMessageHeader header{};
        std::memcpy(&header, body.data() + offset, sizeof(header));
        Require(header.size == kPayloadBytes, "retained payload header changed");
        const auto expected = Payload(ordinal * kLargeMessages + i);
        Require(std::memcmp(body.data() + offset + sizeof(header), expected.data(), expected.size()) == 0,
                "retained payload overwritten/corrupted/duplicated");
    }
}

void CheckLargePrefix(const RegionObserver& region, size_t main_count, bool late, size_t expected_segments) {
    const size_t count = main_count + (late ? 1 : 0);
    const auto deadline = After();
    while (region.Committed() != count - 1 && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(region.Committed() == count - 1, "storage committed prefix differs from oracle");
    std::set<size_t> segments;
    std::vector<std::pair<size_t,size_t>> reservations;
    for (size_t i = 0; i < count; ++i) {
        const auto* entry = region.At<GOIEntry>(kGOIOffset + i * sizeof(GOIEntry));
        Require(entry->global_seq == i && entry->client_id == (i < main_count ? kPrefixClient : kLateClient) &&
                entry->client_seq == (i < main_count ? i : 0) && entry->session_epoch == 1 &&
                entry->total_order == i * kLargeMessages && entry->message_count == kLargeMessages &&
                entry->pbr_index == i, "storage GOI sequence/PBR identity mismatch across wrap");
        const size_t offset = entry->blog_offset;
        const size_t bytes = kLargeMessages * wire::ComputeStrideV2(kPayloadBytes);
        Require(offset >= region.layout.metadata_bytes && offset < region.size && bytes <= region.size - offset,
                "storage GOI payload outside mapping");
        const size_t relative = offset - region.layout.metadata_bytes;
        const size_t segment = relative / region.layout.segment_size;
        Require(relative % region.layout.segment_size >= 64 &&
                bytes <= region.layout.segment_size - relative % region.layout.segment_size,
                "reservation crosses segment boundary");
        if (late && i == main_count) Require(segment == 0, "late receive lost its original segment identity");
        segments.insert(segment);
        reservations.emplace_back(offset, offset + bytes);
    }
    Require(segments.size() == expected_segments, "unexpected number of retained segments");
    std::sort(reservations.begin(), reservations.end());
    for (size_t i = 1; i < reservations.size(); ++i)
        Require(reservations[i-1].second <= reservations[i].first, "BLog reservations overlap");
    std::cout << "[STORAGE_PREFIX] batches=" << count << " segments=" << segments.size()
              << " pbr_laps=" << count / 64 << " reservation_overlap=0" << std::endl;
}

void RequireCapacityRejection(const Options& options) {
    const auto deadline = After();
    while (Clock::now() < deadline) {
        auto fd = Connect(options.data_port);
        auto request = Handshake(1004, 0); request.ack = 0;
        Send(fd.value, &request, sizeof(request));
        const auto answer = Open(fd.value, 1004, options, false, 1, false);
        if (answer.status() == embarcadero::session::SessionOpenAck::RESOURCE_EXHAUSTED) return;
        Require(answer.status() == embarcadero::session::SessionOpenAck::OK, "unexpected capacity admission response");
        std::this_thread::yield();
    }
    throw std::runtime_error("capacity exhaustion did not stop new admission");
}

void RunGoiCapacity(const Options& options, RegionObserver& region) {
    CreateTopic(options);
    PublisherSocket publisher(options, kPrefixClient);
    Batch(kPrefixClient, 0, 0).Publish(publisher.ingress.value);
    Batch(kPrefixClient, 1, 2).Publish(publisher.ingress.value);
    publisher.AwaitAck(4);
    region.CheckPrefix(2);
    Batch(kPrefixClient, 2, 10000).Publish(publisher.ingress.value);
    Stage(options, "capacity_fault_sent");
    Continue(options);
    RequireCapacityRejection(options);
    region.CheckPrefix(2);
    publisher.RejectExtraAck();
    auto subscriber = Connect(options.data_port);
    auto subscribe = Handshake(2001, 0, Subscribe);
    Send(subscriber.value, &subscribe, sizeof(subscribe));
    AuditBatch(subscriber.value, 0); AuditBatch(subscriber.value, 1);
    Require(!Wait(subscriber.value, POLLIN, After(100)), "capacity-rejected work delivered");
    std::cout << "[FAULT_RESULT] status=passed case=" << options.test_case
              << " messages=4 exact_prefix=1 false_ack=0 admission_rejected=1 replay=1" << std::endl;
}
}  // namespace

void RunStorageFault(const Options& options, RegionObserver& region) {
    if (options.test_case == "goi_capacity") { RunGoiCapacity(options, region); return; }
    const auto& config = Configuration::getInstance().config();
    Require(region.layout.segment_size == (size_t{256} << 20) &&
            config.storage.batch_headers_size.get() == 64 * sizeof(BatchHeader),
            "storage schedule requires 256MiB segments and exactly64 PBR slots");
    const bool allocation_failure = options.test_case == "blog_capacity";
    Require(allocation_failure || options.test_case == "rollover_retention", "unknown storage fault");
    CreateTopic(options);
    auto subscriber = Connect(options.data_port);
    auto subscribe = Handshake(2001, 0, Subscribe);
    Send(subscriber.value, &subscribe, sizeof(subscribe));
    PublisherSocket publisher(options, kPrefixClient);
    const size_t first_count = allocation_failure ? 134 : 133;
    for (size_t i = 0; i < first_count; ++i)
        LargeBatch(kPrefixClient, i, i * kLargeMessages).Publish(publisher.ingress.value);
    publisher.AwaitAck(first_count * kLargeMessages);
    Stage(options, "storage_prefix_ready");
    if (allocation_failure) {
        LargeBatch rejected(kPrefixClient, first_count, 10000000);
        Send(publisher.ingress.value, &rejected.header, sizeof(rejected.header));
        Stage(options, "allocation_fault_sent");
        ExpectClose(publisher.ingress.value);
        RequireCapacityRejection(options);
        CheckLargePrefix(region, first_count, false, 1);
        publisher.RejectExtraAck();
        Stage(options, "storage_rejection_verified");
        Continue(options);
        for (size_t i = 0; i < first_count; ++i) AuditLarge(subscriber.value, i);
    } else {
        PublisherSocket late(options, kLateClient);
        LargeBatch late_batch(kLateClient, 0, kMainBatches * kLargeMessages);
        Send(late.ingress.value, &late_batch.header, sizeof(late_batch.header));
        Stage(options, "writer_header_sent");
        Continue(options);
        for (size_t i = first_count; i < kMainBatches; ++i)
            LargeBatch(kPrefixClient, i, i * kLargeMessages).Publish(publisher.ingress.value);
        publisher.AwaitAck(kMainBatches * kLargeMessages);
        Stage(options, "storage_rollovers_done");
        Continue(options);
        Send(late.ingress.value, late_batch.body.data(), late_batch.body.size());
        late.AwaitAck(kLargeMessages);
        CheckLargePrefix(region, kMainBatches, true, 3);
        Stage(options, "late_writer_done");
        Continue(options);
        for (size_t i = 0; i <= kMainBatches; ++i) AuditLarge(subscriber.value, i);
    }
    Require(!Wait(subscriber.value, POLLIN, After(100)), "extra retained delivery");
    // Independent replay begins after rollover/exhaustion, so old bytes cannot
    // have been hidden in this connection's socket buffers before the fault.
    auto replay = Connect(options.data_port);
    Send(replay.value, &subscribe, sizeof(subscribe));
    AuditLarge(replay.value, 0); AuditLarge(replay.value, 1);

    const size_t batches = allocation_failure ? first_count : kMainBatches + 1;
    std::cout << "[FAULT_RESULT] status=passed case=" << options.test_case
              << " messages=" << batches * kLargeMessages
              << " payload_bytes=" << batches * kLargeMessages * kPayloadBytes
              << " exact_prefix=1 false_ack=0 retained_payload=1 segments=" << (allocation_failure ? 1 : 3)
              << " pbr_laps=" << batches / 64 << std::endl;
}
}  // namespace Embarcadero::testdriver
