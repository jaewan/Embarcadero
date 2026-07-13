#pragma once

#include <memory>
#include <mutex>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <lazylog_metadata.grpc.pb.h>

namespace grpc {
class Server;
}

namespace LazyLog {

// Durable, immutable metadata ledger backing one sequencing replica.  The
// ledger owns the idempotency boundary: a retry of the same logical descriptor
// succeeds, while a different descriptor for the same source slot conflicts.
class MetadataReplicaStore {
 public:
  explicit MetadataReplicaStore(std::string sidecar_path);
  ~MetadataReplicaStore();

  MetadataReplicaStore(const MetadataReplicaStore&) = delete;
  MetadataReplicaStore& operator=(const MetadataReplicaStore&) = delete;

  bool Open(std::string* error);
  bool Append(const lazylogmetadata::MetadataAppendRequest& request,
              bool* already_present,
              std::string* error);
  size_t size() const;

 private:
  static std::string SlotKey(const lazylogmetadata::MetadataAppendRequest& request);
  bool Replay(std::string* error);
  bool AppendRecord(const std::string& encoded, std::string* error);

  std::string sidecar_path_;
  int fd_{-1};
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::string> records_;
};

class LazyLogMetadataReplicaService final
    : public lazylogmetadata::LazyLogMetadataReplica::Service {
 public:
  explicit LazyLogMetadataReplicaService(std::shared_ptr<MetadataReplicaStore> store)
      : store_(std::move(store)) {}

  grpc::Status AppendMetadata(grpc::ServerContext* context,
                              const lazylogmetadata::MetadataAppendRequest* request,
                              lazylogmetadata::MetadataAppendResponse* response) override;

 private:
  std::shared_ptr<MetadataReplicaStore> store_;
};

// Server wrapper used by a sequencing-replica process or broker-local service.
class LazyLogMetadataReplicaServer {
 public:
  LazyLogMetadataReplicaServer(std::string address, std::string sidecar_path);
  ~LazyLogMetadataReplicaServer();

  bool Start(std::string* error);
  void Wait();
  void Shutdown();
  const std::string& endpoint() const { return endpoint_; }

 private:
  std::string address_;
  std::string endpoint_;
  std::shared_ptr<MetadataReplicaStore> store_;
  std::unique_ptr<LazyLogMetadataReplicaService> service_;
  std::unique_ptr<grpc::Server> server_;
};

// Client-side fanout for the sequencing layer.  Requests are sent in parallel
// and append completion is returned only after every configured replica has
// durably accepted the same immutable descriptor.
class LazyLogMetadataReplicaClient {
 public:
  explicit LazyLogMetadataReplicaClient(
      std::vector<std::string> endpoints,
      std::chrono::milliseconds rpc_timeout = std::chrono::seconds(10),
      uint32_t max_attempts = 3,
      std::chrono::milliseconds retry_backoff = std::chrono::milliseconds(50));

  bool AppendToAll(const lazylogmetadata::MetadataAppendRequest& request,
                   std::string* error) const;
  size_t replica_count() const { return endpoints_.size(); }

 private:
  std::vector<std::string> endpoints_;
  std::chrono::milliseconds rpc_timeout_;
  uint32_t max_attempts_;
  std::chrono::milliseconds retry_backoff_;
};

}  // namespace LazyLog
