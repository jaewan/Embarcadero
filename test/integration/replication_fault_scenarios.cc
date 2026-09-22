#include "replication_fault_scenarios.h"

namespace Embarcadero::testdriver {
namespace {
void AwaitPublishableReplicationCluster(const Options& options) {
    auto stub = heartbeat_system::HeartBeat::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(options.control_port), grpc::InsecureChannelCredentials()));
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    std::string last_observation = "no cluster response";
    while (Clock::now() < deadline) {
        grpc::ClientContext context;
        const auto remaining = deadline - Clock::now();
        context.set_deadline(std::chrono::system_clock::now() +
            std::min(remaining, std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(1))));
        heartbeat_system::ClientInfo request;
        heartbeat_system::ClusterStatus response;
        // The unary GetClusterStatus only fills legacy new_nodes/removed_nodes.
        // The production subscription sends a complete broker_info snapshot
        // immediately. Reopen for each observation rather than depending on the
        // stream's update-version timing after that first snapshot.
        auto reader = stub->SubscribeToCluster(&context, request);
        const bool received = reader->Read(&response);
        context.TryCancel();
        const auto status = reader->Finish();
        // Cancellation is deliberate after a successful initial Read; its final
        // status does not invalidate the snapshot we actually received.
        if (received) {
            std::array<bool, 3> publishable{};
            bool valid = response.broker_info_size() == 3;
            last_observation.clear();
            for (const auto& broker : response.broker_info()) {
                const int id = broker.broker_id();
                last_observation += " id=" + std::to_string(id) +
                    " accepts_publishes=" + std::to_string(broker.accepts_publishes());
                if (id < 0 || id >= 3 || !broker.accepts_publishes() || publishable[id]) {
                    valid = false;
                } else {
                    publishable[id] = true;
                }
            }
            if (valid && std::all_of(publishable.begin(), publishable.end(), [](bool ready) { return ready; })) {
                std::cout << "[FAULT_CLUSTER_READY] publishable_brokers=0,1,2" << std::endl;
                return;
            }
        } else {
            last_observation = status.error_message();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("three-broker publishable membership did not become ready: " + last_observation);
}

void CheckUncompletedReplica(const RegionObserver& region, PublisherSocket& publisher,
                            uint32_t minimum_token, uint32_t maximum_token) {
    region.CheckPrefix(1);
    const auto* entry = region.At<GOIEntry>(kGOIOffset);
    Require(entry->broker_id == 0 && entry->client_id == kPrefixClient &&
                entry->client_seq == 0 && entry->session_epoch == 1,
            "replication target identity changed");
    const auto stride = wire::ComputeStrideV2(kPayloadBytes);
    Require(entry->payload_size == stride * kMessagesPerBatch &&
                entry->blog_offset <= region.size && entry->payload_size <= region.size - entry->blog_offset,
            "replication payload extent mismatch");
    for (size_t i = 0; i < kMessagesPerBatch; ++i) {
        const auto offset = entry->blog_offset + i * stride;
        Require(region.At<BlogMessageHeader>(offset)->size == kPayloadBytes &&
                    *region.At<std::array<uint8_t, kPayloadBytes>>(offset + sizeof(BlogMessageHeader)) == Payload(i),
                "replication source payload identity mismatch");
    }
    const auto token = entry->num_replicated.load(std::memory_order_acquire);
    Require(token >= minimum_token && token <= maximum_token,
            "missing predecessor/tail fabricated a replication token");
    const auto* completion = region.At<CompletionVectorEntry>(kCompletionVectorOffset);
    Require(completion->completed_logical_offset.load(std::memory_order_acquire) == 0 &&
                completion->completed_pbr_head.load(std::memory_order_acquire) == UINT64_MAX,
            "incomplete replication advanced tail completion frontier");
    publisher.RejectExtraAck();
    Require(publisher.acknowledged == 0, "incomplete replication produced ACK2");
    std::cout << "[FAULT_REPLICATION] goi=0 source=0 client=" << kPrefixClient
              << " epoch=1 batch=0 token=" << token
              << " ordered_messages=2 completed_messages=0 ack2=0" << std::endl;
}
}  // namespace

// Runner owns three brokers/controllers. The driver owns only a native source-0
// connection and a read-only observer; it never edits GOI, token, or CV state.
void RunReplicationTokenShutdown(const Options& options, RegionObserver& region) {
    Require(Configuration::getInstance().config().broker.max_brokers.get() == 3,
            "replication shutdown profile requires exactly three configured brokers");
    // A follower's local listener marker precedes its next heartbeat update.
    // Wait for the same head membership that production topic admission checks.
    AwaitPublishableReplicationCluster(options);
    CreateTopic(options, 2, 3);
    PublisherSocket publisher(options, kPrefixClient, false, 2);
    Batch(kPrefixClient, 0, 0).Publish(publisher.ingress.value);
    Stage(options, "replication_batch_sent");
    // Runner waits for broker1 before-token HIT(token=1,GOI=0) and broker2
    // predecessor-wait HIT(token=1,GOI=0), then grants this observation.
    Continue(options, 20000);
    CheckUncompletedReplica(region, publisher, 1, 1);
    Stage(options, "replication_wait_verified");

    // Keep broker1 paused. Stop broker2 and observe its successful exit before
    // releasing this barrier; its stop flag must cancel the actual token wait.
    Continue(options, 20000);
    CheckUncompletedReplica(region, publisher, 1, 1);
    Stage(options, "replication_successor_stopped_verified");

    // Runner may release broker1, then stops the remaining owned brokers. Its
    // legitimate completion can advance token to2, but no live tail can make3.
    Continue(options, 20000);
    CheckUncompletedReplica(region, publisher, 1, 2);
    std::cout << "[FAULT_RESULT] status=passed case=" << options.test_case
              << " messages=2 goi=0 sink=memory-copy ack_claim=replicated_ack_emulated"
              << " missing_token_shutdown=1 false_ack=0 false_tail_completion=0" << std::endl;
}
}  // namespace Embarcadero::testdriver
