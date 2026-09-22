#pragma once
// Shared native protocol helpers for optional owned production fault tests.
#include "network_manager/protocol.h"
#include "network_manager/batch_validation.h"
#include "common/configuration.h"
#include "common/topic_identity.h"
#include "common/wire_formats.h"
#include "cxl_manager/region_layout.h"
#include "heartbeat.grpc.pb.h"
#include "session.pb.h"

#include <grpcpp/grpcpp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Embarcadero::testdriver {
using namespace Embarcadero;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr size_t kPayloadBytes = 4096;
constexpr size_t kMessagesPerBatch = 2;
constexpr uint32_t kPrefixClient = 1001;
constexpr uint32_t kControlClient = 1002;
constexpr uint32_t kBadClient = 1003;
inline const std::string kTopic = "fault_protocol";

inline void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value >= 0) close(value);
            value = std::exchange(other.value, -1);
        }
        return *this;
    }
};

inline Deadline After(int milliseconds = 10000) {
    return Clock::now() + std::chrono::milliseconds(milliseconds);
}

inline bool Wait(int fd, short events, Deadline deadline) {
    while (Clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        pollfd item{fd, events, 0};
        int n = poll(&item, 1, static_cast<int>(std::min<int64_t>(remaining + 1, 100)));
        if (n > 0) return true;
        if (n < 0 && errno != EINTR) throw std::runtime_error("poll failed");
    }
    return false;
}

inline void Send(int fd, const void* data, size_t bytes, Deadline deadline = After()) {
    size_t done = 0;
    while (done < bytes) {
        Require(Wait(fd, POLLOUT, deadline), "send deadline");
        const auto n = send(fd, static_cast<const char*>(data) + done, bytes - done,
                            MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) { done += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        throw std::runtime_error("socket send failed");
    }
}

inline void Receive(int fd, void* data, size_t bytes, Deadline deadline = After()) {
    size_t done = 0;
    while (done < bytes) {
        Require(Wait(fd, POLLIN, deadline), "receive deadline");
        const auto n = recv(fd, static_cast<char*>(data) + done, bytes - done, MSG_DONTWAIT);
        if (n > 0) { done += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        throw std::runtime_error("socket closed during required frame");
    }
}

inline void ExpectClose(int fd, Deadline deadline = After()) {
    Require(Wait(fd, POLLIN, deadline), "broker did not reject/close connection");
    char byte{};
    const auto n = recv(fd, &byte, 1, MSG_DONTWAIT);
    Require(n == 0 || (n < 0 && (errno == ECONNRESET || errno == ENOTCONN)),
            "expected connection rejection, received data or another error");
}

inline Fd Connect(uint16_t port) {
    Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    Require(fd.value >= 0, "socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int flag = 1;
    setsockopt(fd.value, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int result = connect(fd.value, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    Require(result == 0 || errno == EINPROGRESS, "connect failed");
    Require(Wait(fd.value, POLLOUT, After()), "connect deadline");
    int error = 0;
    socklen_t size = sizeof(error);
    Require(getsockopt(fd.value, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0,
            "connect completion failed");
    return fd;
}

struct Listener {
    Fd fd;
    uint16_t port;
    explicit Listener(bool listen_now = true) : fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)), port(0) {
        Require(fd.value >= 0, "listener socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(bind(fd.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind failed");
        socklen_t size = sizeof(address);
        Require(getsockname(fd.value, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname failed");
        port = ntohs(address.sin_port);
        if (listen_now) Require(listen(fd.value, 4) == 0, "listen failed");
    }
    Fd Accept() {
        Require(Wait(fd.value, POLLIN, After()), "ACK connect deadline");
        Fd result(accept4(fd.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
        Require(result.value >= 0, "ACK accept failed");
        int broker = -1;
        Receive(result.value, &broker, sizeof(broker));
        Require(broker == 0, "unexpected ACK broker id");
        return result;
    }
};

struct Options {
    std::string test_case = "control";
    std::string config;
    std::string shm;
    uint16_t data_port = 1214;
    uint16_t control_port = 12140;
    int controller = -1;
};

inline void Stage(const Options& options, const std::string& stage) {
    std::cout << "[FAULT_DRIVER] case=" << options.test_case << " stage=" << stage << std::endl;
    if (options.controller >= 0) {
        const auto message = "STAGE " + stage;
        Require(send(options.controller, message.data(), message.size(), MSG_NOSIGNAL) ==
                    static_cast<ssize_t>(message.size()), "controller send failed");
    }
}

inline void Continue(const Options& options, int timeout_ms = 10000) {
    Require(options.controller >= 0, "this deterministic case requires --controller-fd");
    Require(Wait(options.controller, POLLIN, After(timeout_ms)), "controller release deadline");
    std::array<char, 64> message{};
    const auto n = recv(options.controller, message.data(), message.size(), 0);
    Require(n == 8 && std::string(message.data(), n) == "CONTINUE", "controller cancelled or invalid release");
}

inline EmbarcaderoReq Handshake(uint32_t client, uint16_t ack_port, ClientRequestType request = Publish, int ack_level = 1) {
    EmbarcaderoReq value{};
    value.client_id = client;
    value.ack = request == Publish ? ack_level : 0;
    value.num_msg = request == Publish ? 1 : 0;
    value.port = ack_port;
    value.client_req = request;
    std::memcpy(value.topic, kTopic.data(), kTopic.size());
    return value;
}

inline std::string OpenPayload(uint32_t client, uint32_t requested_epoch = 1) {
    embarcadero::session::SessionOpen open;
    open.set_client_id(client);
    open.set_requested_session_epoch(requested_epoch);
    open.set_topic(kTopic);
    return open.SerializeAsString();
}

inline embarcadero::session::SessionOpenAck ReadOpenAck(int fd) {
    network::SessionControlHeader reply{};
    Receive(fd, &reply, sizeof(reply));
    Require(reply.magic == network::kSessionControlMagic && reply.length > 0 && reply.length <= 65536,
            "invalid OPEN ACK envelope");
    std::string bytes(reply.length, '\0');
    Receive(fd, bytes.data(), bytes.size());
    embarcadero::session::SessionOpenAck ack;
    Require(ack.ParseFromString(bytes), "invalid OPEN ACK protobuf");
    return ack;
}

inline embarcadero::session::SessionOpenAck Open(int fd, uint32_t client, const Options& options,
        bool fragment = false, uint32_t requested_epoch = 1, bool require_fresh = true) {
    const auto payload = OpenPayload(client, requested_epoch);
    network::SessionControlHeader header{network::kSessionControlMagic, static_cast<uint32_t>(payload.size())};
    if (fragment) {
        Send(fd, &header, 1);
        Stage(options, "fragment_sent");
        Continue(options);
        Send(fd, reinterpret_cast<const char*>(&header) + 1, sizeof(header) - 1);
    } else {
        Send(fd, &header, sizeof(header));
    }
    Send(fd, payload.data(), payload.size());
    auto ack = ReadOpenAck(fd);
    if (require_fresh)
        Require(ack.status() == embarcadero::session::SessionOpenAck::OK &&
                ack.assigned_session_epoch() == requested_epoch && !ack.has_committed_prefix(), "unexpected fresh OPEN ACK");
    return ack;
}

struct PublisherSocket {
    Listener listener;
    Fd ingress;
    Fd ack;
    uint32_t client;
    size_t acknowledged = 0;
    PublisherSocket(const Options& options, uint32_t id, bool fragment = false, int ack_level = 1)
        : ingress(Connect(options.data_port)), client(id) {
        const auto request = Handshake(client, listener.port, Publish, ack_level);
        Send(ingress.value, &request, sizeof(request));
        Open(ingress.value, client, options, fragment);
        ack = listener.Accept();
    }
    void AwaitAck(size_t expected) {
        const auto deadline = After();
        while (acknowledged < expected) {
            size_t next = 0;
            Receive(ack.value, &next, sizeof(next), deadline);
            Require(next >= acknowledged && next <= expected, "false or regressing ACK");
            acknowledged = next;
        }
    }
    void RejectExtraAck() {
        while (Wait(ack.value, POLLIN, After(20))) {
            size_t next = 0;
            const auto n = recv(ack.value, &next, sizeof(next), MSG_PEEK | MSG_DONTWAIT);
            if (n == 0) return;
            Require(n >= 0, "ACK read error");
            Receive(ack.value, &next, sizeof(next));
            Require(next == acknowledged, "rejected ingress advanced ACK");
        }
    }
};

inline std::array<uint8_t, kPayloadBytes> Payload(uint64_t identity) {
    std::array<uint8_t, kPayloadBytes> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>((identity * 17 + i) % 251);
    std::memcpy(bytes.data(), &identity, sizeof(identity));
    return bytes;
}

struct Batch {
    BatchHeader header{};
    std::vector<uint8_t> body;
    Batch(uint32_t client, size_t sequence, uint64_t first_identity) {
        header.client_id = client;
        header.batch_seq = sequence;
        header.original_client_batch_seq = sequence;
        header.num_msg = kMessagesPerBatch;
        header.session_epoch = 1;
        header.total_size = kMessagesPerBatch * wire::ComputeStrideV2(kPayloadBytes);
        header.start_logical_offset = sizeof(BatchHeader);
        body.resize(header.total_size);
        for (size_t i = 0; i < kMessagesPerBatch; ++i) {
            BlogMessageHeader message{};
            message.size = kPayloadBytes;
            message.client_id = client;
            message.batch_seq = static_cast<uint32_t>(sequence);
            const auto offset = i * wire::ComputeStrideV2(kPayloadBytes);
            std::memcpy(body.data() + offset, &message, sizeof(message));
            const auto payload = Payload(first_identity + i);
            std::memcpy(body.data() + offset + sizeof(message), payload.data(), payload.size());
        }
    }
    void Publish(int fd) const {
        Send(fd, &header, sizeof(header));
        Send(fd, body.data(), body.size());
    }
};

inline void CreateTopic(const Options& options, int ack_level = 1, int replication_factor = 0) {
    auto stub = heartbeat_system::HeartBeat::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(options.control_port), grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    heartbeat_system::CreateTopicRequest request;
    request.set_topic(kTopic);
    request.set_order(5);
    request.set_ack_level(ack_level);
    request.set_replication_factor(replication_factor);
    request.set_replicate_tinode(false);
    request.set_sequencer_type(heartbeat_system::EMBARCADERO);
    heartbeat_system::CreateTopicResponse response;
    const auto result = stub->CreateNewTopic(&context, request, &response);
    Require(result.ok() && response.success(), "CreateNewTopic failed: " + result.error_message());
}

struct RegionObserver {
    void* base = MAP_FAILED;
    size_t size = 0;
    cxl_manager::RegionLayout layout{};
    explicit RegionObserver(const Options& options) {
        auto& config = Configuration::getInstance();
        Require(config.loadFromFile(options.config) && config.finalize(), "invalid observer configuration");
        layout = cxl_manager::CalculateRegionLayout(config.config());
        size = layout.region_bytes;
        Require(size == 64ULL * 1024 * 1024 * 1024, "fault profile requires 64GiB region");
        Fd fd(shm_open(options.shm.c_str(), O_RDONLY, 0));
        struct stat info{};
        Require(fd.value >= 0 && fstat(fd.value, &info) == 0 && static_cast<size_t>(info.st_size) == size &&
                    info.st_uid == getuid(), "observer region ownership/size mismatch");
        base = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd.value, 0);
        Require(base != MAP_FAILED, "read-only observer mapping failed");
        const auto* descriptor = At<cxl_manager::RegionDescriptor>(cxl_manager::kRegionDescriptorOffset);
        if (descriptor->ready.load(std::memory_order_acquire) != cxl_manager::kRegionReady ||
            !cxl_manager::RegionDescriptorMatches(*descriptor, layout, config.config().broker.max_brokers.get(),
                config.config().storage.max_topics.get(), config.config().storage.batch_headers_size.get(),
                0 /* Emul */, descriptor->mapping_base)) {
            munmap(base, size);
            base = MAP_FAILED;
            throw std::runtime_error("observer region descriptor not ready or incompatible");
        }
    }
    ~RegionObserver() { if (base != MAP_FAILED) munmap(base, size); }
    template<class T> const T* At(size_t offset) const {
        Require(offset <= size && sizeof(T) <= size - offset, "observer offset outside region");
        return reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
    }
    uint64_t Committed() const { return At<ControlBlock>(0)->committed_seq.load(std::memory_order_acquire); }
    void CheckPrefix(size_t batches) const {
        const auto expected = batches ? batches - 1 : UINT64_MAX;
        const auto deadline = After();
        while (Committed() != expected && Clock::now() < deadline) {
            Require(Committed() == UINT64_MAX || (batches && Committed() < expected), "unexpected extra committed GOI entry");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(Committed() == expected, "committed prefix did not reach expected boundary");
        for (size_t i = 0; i < batches; ++i) {
            const auto* entry = At<GOIEntry>(kGOIOffset + i * sizeof(GOIEntry));
            Require(entry->global_seq == i && entry->client_id == (i < 2 ? kPrefixClient : kControlClient) &&
                        entry->client_seq == (i < 2 ? i : 0) && entry->session_epoch == 1 &&
                        entry->total_order == i * kMessagesPerBatch && entry->message_count == kMessagesPerBatch &&
                        entry->pbr_index == i, "GOI identity/count/PBR prefix mismatch");
        }
        const auto* cv = At<CompletionVectorEntry>(kCompletionVectorOffset);
        Require(cv->sequencer_logical_offset.load(std::memory_order_acquire) == batches * kMessagesPerBatch,
                "false or incomplete CV logical progress");
        if (batches >= 2) {
            const auto slot = TopicSlot(kTopic.c_str(), Configuration::getInstance().config().storage.max_topics.get());
            const auto offset = layout.metadata_bytes - layout.session_table_bytes + slot * kMaxSessions * sizeof(SessionEntry);
            bool found = false;
            for (size_t i = 0; i < kMaxSessions; ++i) {
                const auto* entry = At<SessionEntry>(offset + i * sizeof(SessionEntry));
                if (entry->session_key.load(std::memory_order_acquire) == (uint64_t{kPrefixClient} << 32 | 1)) {
                    Require(entry->expected_seq.load(std::memory_order_acquire) == 2 &&
                                entry->committed_hwm.load(std::memory_order_acquire) == 1,
                            "rejected ingress changed committed session prefix");
                    found = true;
                }
            }
            Require(found, "committed session missing from authoritative table");
        }
        std::cout << "[FAULT_PREFIX] batches=" << batches << " messages=" << batches * kMessagesPerBatch
                  << " goi_cv_session=passed" << std::endl;
    }
};

inline void AuditBatch(int fd, size_t batch_index) {
    wire::BatchMetadata metadata{};
    Receive(fd, &metadata, sizeof(metadata));
    Require(metadata.header_version == wire::HEADER_VERSION_V2 && metadata.flags == 0 &&
                metadata.num_messages == kMessagesPerBatch && metadata.batch_total_order == batch_index * kMessagesPerBatch,
            "subscriber metadata gap/order/count mismatch");
    for (size_t i = 0; i < kMessagesPerBatch; ++i) {
        BlogMessageHeader header{};
        Receive(fd, &header, sizeof(header));
        Require(header.size == kPayloadBytes, "subscriber payload length mismatch");
        std::array<uint8_t, kPayloadBytes> payload{};
        Receive(fd, payload.data(), payload.size());
        Require(payload == Payload(batch_index * kMessagesPerBatch + i), "subscriber payload identity/corruption");
    }
}

void SelfTest();
}  // namespace Embarcadero::testdriver
