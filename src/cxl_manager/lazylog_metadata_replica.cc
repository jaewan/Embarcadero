#include "lazylog_metadata_replica.h"
#include "common/durable_sync.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <thread>
#include <limits>

#include <grpcpp/grpcpp.h>

namespace LazyLog {
namespace {

constexpr uint32_t kRecordMagic = 0x4c4c4d44U;  // "LLMD"
constexpr uint32_t kRecordVersion = 1;
constexpr size_t kRecordHeaderBytes = 16;

void PutLe32(uint8_t* dst, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) dst[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint32_t GetLe32(const uint8_t* src) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(src[i]) << (i * 8);
  return value;
}

uint32_t Crc32c(const uint8_t* data, size_t len) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0x82f63b78U & static_cast<uint32_t>(-(crc & 1U)));
    }
  }
  return ~crc;
}

bool WriteFully(int fd, const uint8_t* data, size_t len, std::string* error) {
  size_t written = 0;
  while (written < len) {
    const ssize_t rc = write(fd, data + written, len - written);
    if (rc < 0 && errno == EINTR) continue;
    if (rc <= 0) {
      if (error) *error = "sidecar write failed: " + std::string(strerror(errno));
      return false;
    }
    written += static_cast<size_t>(rc);
  }
  return true;
}

}  // namespace

MetadataReplicaStore::MetadataReplicaStore(std::string sidecar_path)
    : sidecar_path_(std::move(sidecar_path)) {}

MetadataReplicaStore::~MetadataReplicaStore() {
  if (fd_ >= 0) close(fd_);
}

bool MetadataReplicaStore::Open(std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ >= 0) return true;
  std::error_code ec;
  const auto parent = std::filesystem::path(sidecar_path_).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  if (ec) {
    if (error) *error = "cannot create metadata sidecar directory: " + ec.message();
    return false;
  }
  fd_ = open(sidecar_path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
  if (fd_ < 0) {
    if (error) *error = "cannot open metadata sidecar: " + std::string(strerror(errno));
    return false;
  }
  if (Replay(error)) return true;
  close(fd_);
  fd_ = -1;
  return false;
}

std::string MetadataReplicaStore::SlotKey(
    const lazylogmetadata::MetadataAppendRequest& request) {
  return request.topic() + "\x1f" + std::to_string(request.source_broker_id()) + "\x1f" +
         std::to_string(request.source_batch_seq());
}

bool MetadataReplicaStore::Replay(std::string* error) {
  records_.clear();
  if (lseek(fd_, 0, SEEK_SET) < 0) {
    if (error) *error = "metadata sidecar seek failed: " + std::string(strerror(errno));
    return false;
  }
  off_t offset = 0;
  bool discard_torn_tail = false;
  while (true) {
    std::array<uint8_t, kRecordHeaderBytes> header{};
    const ssize_t got = pread(fd_, header.data(), header.size(), offset);
    if (got == 0) break;
    if (got < 0) {
      if (error) *error = "metadata sidecar read failed: " + std::string(strerror(errno));
      return false;
    }
    if (static_cast<size_t>(got) != header.size()) {
      discard_torn_tail = true;
      break;
    }
    const uint32_t magic = GetLe32(header.data());
    const uint32_t version = GetLe32(header.data() + 4);
    const uint32_t length = GetLe32(header.data() + 8);
    const uint32_t crc = GetLe32(header.data() + 12);
    if (magic != kRecordMagic || version != kRecordVersion || length == 0 ||
        length > (1U << 20)) {
      if (error) *error = "metadata sidecar corrupt record at offset " + std::to_string(offset);
      return false;
    }
    std::string encoded(length, '\0');
    const ssize_t body = pread(fd_, encoded.data(), length, offset + kRecordHeaderBytes);
    if (body < 0) {
      if (error) *error = "metadata sidecar body read failed: " + std::string(strerror(errno));
      return false;
    }
    if (static_cast<uint32_t>(body) != length) {
      discard_torn_tail = true;
      break;
    }
    if (Crc32c(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()) != crc) {
      if (error) *error = "metadata sidecar checksum failure at offset " + std::to_string(offset);
      return false;
    }
    lazylogmetadata::MetadataAppendRequest request;
    if (!request.ParseFromString(encoded) || request.topic().empty()) {
      if (error) *error = "metadata sidecar invalid protobuf at offset " + std::to_string(offset);
      return false;
    }
    records_[SlotKey(request)] = encoded;
    offset += static_cast<off_t>(kRecordHeaderBytes + length);
  }
  // A torn final record is not a valid append.  Remove it before admitting a
  // later append; otherwise the next recovery would encounter that torn record
  // in the middle of the file and correctly reject the sidecar as corrupt.
  if (discard_torn_tail && ftruncate(fd_, offset) != 0) {
    if (error) *error = "metadata sidecar truncate failed: " + std::string(strerror(errno));
    return false;
  }
  if (lseek(fd_, 0, SEEK_END) < 0) {
    if (error) *error = "metadata sidecar seek end failed: " + std::string(strerror(errno));
    return false;
  }
  return true;
}

bool MetadataReplicaStore::AppendRecord(const std::string& encoded, std::string* error) {
  if (encoded.empty() || encoded.size() > std::numeric_limits<uint32_t>::max()) {
    if (error) *error = "invalid metadata sidecar record length";
    return false;
  }
  std::array<uint8_t, kRecordHeaderBytes> header{};
  PutLe32(header.data(), kRecordMagic);
  PutLe32(header.data() + 4, kRecordVersion);
  PutLe32(header.data() + 8, static_cast<uint32_t>(encoded.size()));
  PutLe32(header.data() + 12, Crc32c(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()));
  if (!WriteFully(fd_, header.data(), header.size(), error) ||
      !WriteFully(fd_, reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(), error)) {
    return false;
  }
  if (Embarcadero::DurableFdatasync(fd_) != 0) {
    if (error) *error = "metadata sidecar fdatasync failed: " + std::string(strerror(errno));
    return false;
  }
  return true;
}

bool MetadataReplicaStore::Append(const lazylogmetadata::MetadataAppendRequest& request,
                                  bool* already_present,
                                  std::string* error) {
  if (already_present) *already_present = false;
  if (request.topic().empty()) {
    if (error) *error = "metadata topic must not be empty";
    return false;
  }
  std::string encoded;
  if (!request.SerializeToString(&encoded)) {
    if (error) *error = "cannot serialize metadata request";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ < 0) {
    if (error) *error = "metadata sidecar not open";
    return false;
  }
  const std::string key = SlotKey(request);
  const auto it = records_.find(key);
  if (it != records_.end()) {
    if (it->second == encoded) {
      if (already_present) *already_present = true;
      return true;
    }
    if (error) *error = "metadata slot conflicts with a different descriptor";
    return false;
  }
  if (!AppendRecord(encoded, error)) return false;
  records_.emplace(key, std::move(encoded));
  return true;
}

size_t MetadataReplicaStore::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return records_.size();
}

grpc::Status LazyLogMetadataReplicaService::AppendMetadata(
    grpc::ServerContext* /*context*/,
    const lazylogmetadata::MetadataAppendRequest* request,
    lazylogmetadata::MetadataAppendResponse* response) {
  bool already_present = false;
  std::string error;
  if (!store_->Append(*request, &already_present, &error)) {
    response->set_success(false);
    response->set_error(error);
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error);
  }
  response->set_success(true);
  response->set_already_present(already_present);
  return grpc::Status::OK;
}

LazyLogMetadataReplicaServer::LazyLogMetadataReplicaServer(
    std::string address, std::string sidecar_path)
    : address_(std::move(address)),
      store_(std::make_shared<MetadataReplicaStore>(std::move(sidecar_path))) {}

LazyLogMetadataReplicaServer::~LazyLogMetadataReplicaServer() { Shutdown(); }

bool LazyLogMetadataReplicaServer::Start(std::string* error) {
  if (server_) return true;
  if (!store_->Open(error)) return false;
  service_ = std::make_unique<LazyLogMetadataReplicaService>(store_);
  grpc::ServerBuilder builder;
  int selected_port = 0;
  builder.AddListeningPort(address_, grpc::InsecureServerCredentials(), &selected_port);
  builder.RegisterService(service_.get());
  server_ = builder.BuildAndStart();
  if (!server_ || selected_port <= 0) {
    if (error) *error = "cannot start LazyLog metadata replica at " + address_;
    server_.reset();
    service_.reset();
    return false;
  }
  endpoint_ = address_;
  const auto separator = endpoint_.rfind(':');
  if (separator != std::string::npos && endpoint_.substr(separator + 1) == "0") {
    endpoint_.replace(separator + 1, std::string::npos, std::to_string(selected_port));
  }
  return true;
}

void LazyLogMetadataReplicaServer::Wait() {
  if (server_) server_->Wait();
}

void LazyLogMetadataReplicaServer::Shutdown() {
  if (server_) server_->Shutdown();
  server_.reset();
  service_.reset();
}

LazyLogMetadataReplicaClient::LazyLogMetadataReplicaClient(
    std::vector<std::string> endpoints, std::chrono::milliseconds rpc_timeout,
    uint32_t max_attempts, std::chrono::milliseconds retry_backoff)
    : endpoints_(std::move(endpoints)),
      rpc_timeout_(rpc_timeout),
      max_attempts_(std::max<uint32_t>(1, max_attempts)),
      retry_backoff_(std::max(std::chrono::milliseconds::zero(), retry_backoff)) {}

bool LazyLogMetadataReplicaClient::AppendToAll(
    const lazylogmetadata::MetadataAppendRequest& request, std::string* error) const {
  if (endpoints_.empty()) {
    if (error) *error = "no LazyLog metadata replica endpoints configured";
    return false;
  }
  std::vector<std::future<std::string>> calls;
  calls.reserve(endpoints_.size());
  for (const auto& endpoint : endpoints_) {
    calls.emplace_back(std::async(std::launch::async,
        [endpoint, &request, timeout = rpc_timeout_, attempts = max_attempts_,
         retry_backoff = retry_backoff_] {
      std::string last_failure;
      for (uint32_t attempt = 1; attempt <= attempts; ++attempt) {
        auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
        auto stub = lazylogmetadata::LazyLogMetadataReplica::NewStub(channel);
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + timeout);
        lazylogmetadata::MetadataAppendResponse response;
        const grpc::Status status = stub->AppendMetadata(&context, request, &response);
        if (status.ok() && response.success()) return std::string{};

        // A conflicting immutable descriptor is a safety failure, not a transient
        // transport failure. Retrying it cannot repair the disagreement.
        if (status.ok()) {
          return endpoint + ": " + response.error();
        }
        last_failure = endpoint + ": " + status.error_message();
        if (attempt < attempts && retry_backoff > std::chrono::milliseconds::zero()) {
          std::this_thread::sleep_for(retry_backoff * attempt);
        }
      }
      return last_failure.empty() ? endpoint + ": metadata append failed" : last_failure;
    }));
  }
  for (auto& call : calls) {
    const std::string failure = call.get();
    if (!failure.empty()) {
      if (error) *error = failure;
      return false;
    }
  }
  return true;
}

}  // namespace LazyLog
