// Optional live-protocol scenarios. --self-test uses socketpairs only.
#include "production_fault_support.h"
#include "session_fault_scenarios.h"
#include "replication_fault_scenarios.h"
#include "session_capacity_scenario.h"
#include "storage_fault_scenarios.h"

namespace {
using namespace Embarcadero::testdriver;
void RunIngress(const Options& options, RegionObserver& region) {
    CreateTopic(options);
    auto subscriber = Connect(options.data_port);
    const auto subscribe = Handshake(2001, 0, Subscribe);
    Send(subscriber.value, &subscribe, sizeof(subscribe));
    PublisherSocket prefix(options, kPrefixClient, options.test_case == "fragmented_open");
    Batch(kPrefixClient, 0, 0).Publish(prefix.ingress.value);
    Batch(kPrefixClient, 1, 2).Publish(prefix.ingress.value);
    prefix.AwaitAck(4);
    AuditBatch(subscriber.value, 0);
    AuditBatch(subscriber.value, 1);
    region.CheckPrefix(2);
    Stage(options, "prefix_verified");

    if (options.test_case == "repeated_rejected_connections") {
        Stage(options, "fd_baseline");
        Continue(options);
        for (int attempt = 0; attempt < 128; ++attempt) {
            auto rejected = Connect(options.data_port);
            // A complete native request with an empty topic is rejected before
            // session admission or ACK-thread creation.
            auto request = Handshake(kBadClient, 0);
            std::memset(request.topic, 0, sizeof(request.topic));
            Send(rejected.value, &request, sizeof(request));
            ExpectClose(rejected.value);
        }
        Stage(options, "fd_completed");
        Continue(options);
    } else if (options.test_case == "truncated_control") {
        auto rejected = Connect(options.data_port);
        const auto request = Handshake(kBadClient, prefix.listener.port);
        Send(rejected.value, &request, sizeof(request));
        const uint32_t magic = network::kSessionControlMagic;
        Send(rejected.value, &magic, 1);
        Stage(options, "partial_control_sent");
        ExpectClose(rejected.value, After(5000));
    } else if (options.test_case != "control" && options.test_case != "fragmented_open") {
        Batch bad(kPrefixClient, 2, 10000);
        if (options.test_case == "mismatched_client") {
            bad.header.client_id = kBadClient;
            Send(prefix.ingress.value, &bad.header, sizeof(bad.header));
        } else if (options.test_case == "incomplete_payload") {
            Send(prefix.ingress.value, &bad.header, sizeof(bad.header));
            Send(prefix.ingress.value, bad.body.data(), bad.body.size() / 2);
            Require(shutdown(prefix.ingress.value, SHUT_WR) == 0, "half-close failed");
        } else if (options.test_case == "malformed_body") {
            uint32_t invalid = std::numeric_limits<uint32_t>::max();
            std::memcpy(bad.body.data(), &invalid, sizeof(invalid));
            bad.Publish(prefix.ingress.value);
        } else {
            throw std::runtime_error("unknown ingress case");
        }
        ExpectClose(prefix.ingress.value);
    }
    region.CheckPrefix(2);
    prefix.RejectExtraAck();
    Stage(options, "rejection_verified");
    PublisherSocket control(options, kControlClient);
    Batch(kControlClient, 0, 4).Publish(control.ingress.value);
    control.AwaitAck(2);
    AuditBatch(subscriber.value, 2);
    region.CheckPrefix(3);
    prefix.RejectExtraAck();
    Require(!Wait(subscriber.value, POLLIN, After(100)), "unexpected delivery beyond valid prefix");
    std::cout << "[FAULT_RESULT] status=passed case=" << options.test_case
              << " messages=6 payload_bytes=" << 6 * kPayloadBytes
              << " exact_prefix=1 false_ack=0 recovery_control=1" << std::endl;
}

void RunShutdown(const Options& options, RegionObserver& region) {
    std::vector<Fd> sockets;
    std::unique_ptr<Listener> unavailable;
    std::unique_ptr<PublisherSocket> publisher;
    if (options.test_case == "shutdown_partial_handshake") {
        sockets.push_back(Connect(options.data_port));
        const auto request = Handshake(kBadClient, 0);
        Send(sockets.back().value, &request, 8);
    } else if (options.test_case == "shutdown_partial_control") {
        CreateTopic(options);
        sockets.push_back(Connect(options.data_port));
        const auto request = Handshake(kBadClient, 0);
        Send(sockets.back().value, &request, sizeof(request));
        const uint32_t magic = network::kSessionControlMagic;
        Send(sockets.back().value, &magic, 1);
    } else if (options.test_case == "shutdown_partial_body") {
        CreateTopic(options);
        publisher = std::make_unique<PublisherSocket>(options, kPrefixClient);
        Batch partial(kPrefixClient, 0, 0);
        Send(publisher->ingress.value, &partial.header, sizeof(partial.header));
        Send(publisher->ingress.value, partial.body.data(), 1);
    } else if (options.test_case == "shutdown_ack_connect") {
        CreateTopic(options);
        unavailable = std::make_unique<Listener>(false);  // Bound, owned, never listening.
        sockets.push_back(Connect(options.data_port));
        const auto request = Handshake(kBadClient, unavailable->port);
        Send(sockets.back().value, &request, sizeof(request));
        Open(sockets.back().value, kBadClient, options);
    } else if (options.test_case == "shutdown_queue") {
        for (int i = 0; i < 3; ++i) sockets.push_back(Connect(options.data_port));
    } else {
        throw std::runtime_error("unknown shutdown case");
    }
    region.CheckPrefix(0);
    Stage(options, "shutdown_sockets_open");
    for (auto& socket : sockets) ExpectClose(socket.value, After(20000));
    if (publisher) {
        ExpectClose(publisher->ingress.value, After(20000));
        publisher->RejectExtraAck();
    }
    region.CheckPrefix(0);
    std::cout << "[FAULT_RESULT] status=passed case=" << options.test_case
              << " messages=0 exact_prefix=1 false_ack=0 peers_closed=1" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace Embarcadero::testdriver;
    try {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string name = argv[i];
            if (name == "--self-test") { SelfTest(); return 0; }
            if (name == "--print-fault-geometry") {
                std::cout << "{\"batch_header_bytes\":" << sizeof(BatchHeader)
                          << ",\"message_payload_bytes\":" << kPayloadBytes
                          << ",\"message_wire_bytes\":" << wire::ComputeStrideV2(kPayloadBytes)
                          << "}" << std::endl;
                return 0;
            }
            Require(i + 1 < argc, "option lacks value: " + name);
            const std::string value = argv[++i];
            if (name == "--case") options.test_case = value;
            else if (name == "--config") options.config = value;
            else if (name == "--shm-name") options.shm = value;
            else if (name == "--controller-fd") options.controller = std::stoi(value);
            else throw std::runtime_error("unknown option: " + name);
        }
        Require(!options.config.empty() && !options.shm.empty(), "--config and --shm-name are required");
        RegionObserver region(options);
        if (options.controller >= 0) {
            Stage(options, "driver_ready");
            Continue(options);
        }
        if (options.test_case == "goi_capacity" || options.test_case == "blog_capacity" ||
            options.test_case == "rollover_retention") RunStorageFault(options, region);
        else if (options.test_case == "session_capacity") RunSessionCapacity(options, region);
        else if (options.test_case == "shutdown_replication_token") RunReplicationTokenShutdown(options, region);
        else if (options.test_case == "fence_before_commit" || options.test_case == "commit_before_fence" ||
            options.test_case == "fence_empty_prefix") RunSessionFault(options, region);
        else if (options.test_case.rfind("shutdown_", 0) == 0) RunShutdown(options, region);
        else RunIngress(options, region);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAULT_RESULT] status=failed reason=" << error.what() << std::endl;
        return 1;
    }
}
