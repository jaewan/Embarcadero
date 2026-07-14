#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace Corfu {

// Durable, architecture-independent identities used by the Corfu remote chain.
struct CorfuSlotKey {
  std::string topic;
  uint32_t broker_id{};
  uint64_t broker_batch_seq{};
  bool operator<(const CorfuSlotKey& other) const;
};
struct CorfuValueId {
  uint64_t client_id{};
  uint64_t original_client_batch_seq{};
  uint64_t total_order{};
  uint32_t num_msg{};
  uint64_t total_size{};
  bool operator==(const CorfuValueId& other) const;
};
enum class CorfuSlotState : uint8_t { kUnwritten = 0, kValue = 1, kJunk = 2 };
enum class CorfuWriteStatus : uint8_t { kWritten, kAlreadySame, kAlreadyJunk, kConflict, kIoError };
struct CorfuProbeResult { CorfuSlotState state{CorfuSlotState::kUnwritten}; CorfuValueId value{}; };

// A sidecar is the source of truth for completed remote slots.  Data is synced
// before VALUE is appended and synced; replay ignores only a torn final record.
class CorfuReplicaStore {
 public:
  CorfuReplicaStore(std::string data_path, std::string sidecar_path);
  ~CorfuReplicaStore();
  CorfuReplicaStore(const CorfuReplicaStore&) = delete;
  CorfuProbeResult Probe(const CorfuSlotKey& key) const;
  CorfuWriteStatus WriteOnce(const CorfuSlotKey& key, const CorfuValueId& value,
                             uint64_t data_offset, const void* payload, uint64_t size);
  CorfuWriteStatus WriteJunkOnce(const CorfuSlotKey& key);

 private:
  struct Entry { CorfuSlotState state; CorfuValueId value; uint64_t offset; uint64_t size; };
  void Replay();
  bool AppendRecord(const CorfuSlotKey& key, const Entry& entry);
  int data_fd_{-1};
  int sidecar_fd_{-1};
  std::string sidecar_path_;
  mutable std::mutex mu_;
  std::map<CorfuSlotKey, Entry> slots_;
};

}  // namespace Corfu
