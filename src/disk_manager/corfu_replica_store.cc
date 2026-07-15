#include "disk_manager/corfu_replica_store.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <tuple>
#include <vector>

namespace Corfu {
namespace {
constexpr uint32_t kMagic = 0x43524653;  // "CRFS", little-endian on disk
constexpr uint16_t kVersion = 1;
constexpr uint32_t kMaxTopic = 1024;

uint32_t Crc32c(const uint8_t* p, size_t n) {
  uint32_t crc = ~0u;
  while (n--) { crc ^= *p++; for (int i = 0; i != 8; ++i) crc = (crc >> 1) ^ (0x82f63b78u & -(crc & 1)); }
  return ~crc;
}
template <typename T> void Put(std::vector<uint8_t>* out, T v) {
  for (size_t i = 0; i < sizeof(T); ++i) out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}
template <typename T> bool Get(const uint8_t*& p, const uint8_t* end, T* v) {
  if (static_cast<size_t>(end - p) < sizeof(T)) return false;
  *v = 0; for (size_t i = 0; i < sizeof(T); ++i) *v |= static_cast<T>(*p++) << (8 * i); return true;
}
bool WriteAll(int fd, const void* data, size_t bytes) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (bytes) { const ssize_t n = write(fd, p, bytes); if (n <= 0) return false; p += n; bytes -= n; }
  return true;
}
}

bool CorfuSlotKey::operator<(const CorfuSlotKey& o) const { return std::tie(topic, broker_id, broker_batch_seq) < std::tie(o.topic, o.broker_id, o.broker_batch_seq); }
bool CorfuValueId::operator==(const CorfuValueId& o) const { return client_id == o.client_id && original_client_batch_seq == o.original_client_batch_seq && total_order == o.total_order && num_msg == o.num_msg && total_size == o.total_size; }

CorfuReplicaStore::CorfuReplicaStore(std::string data_path, std::string sidecar_path,
                                     bool media_durable)
    : media_durable_(media_durable), sidecar_path_(std::move(sidecar_path)) {
  if (!media_durable_) return;
  data_fd_ = open(data_path.c_str(), O_RDWR | O_CREAT, 0644);
  sidecar_fd_ = open(sidecar_path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (data_fd_ < 0 || sidecar_fd_ < 0) throw std::runtime_error("cannot open Corfu replica data/sidecar: " + std::string(strerror(errno)));
  Replay();
}
CorfuReplicaStore::~CorfuReplicaStore() { if (data_fd_ >= 0) close(data_fd_); if (sidecar_fd_ >= 0) close(sidecar_fd_); }
CorfuProbeResult CorfuReplicaStore::Probe(const CorfuSlotKey& key) const { std::lock_guard<std::mutex> l(mu_); auto it = slots_.find(key); return it == slots_.end() ? CorfuProbeResult{} : CorfuProbeResult{it->second.state, it->second.value}; }

bool CorfuReplicaStore::AppendRecord(const CorfuSlotKey& key, const Entry& e) {
  if (key.topic.size() > kMaxTopic) return false;
  std::vector<uint8_t> body;
  Put<uint16_t>(&body, static_cast<uint16_t>(key.topic.size())); body.insert(body.end(), key.topic.begin(), key.topic.end());
  Put<uint32_t>(&body, key.broker_id); Put<uint64_t>(&body, key.broker_batch_seq); Put<uint8_t>(&body, static_cast<uint8_t>(e.state));
  Put<uint64_t>(&body, e.value.client_id); Put<uint64_t>(&body, e.value.original_client_batch_seq); Put<uint64_t>(&body, e.value.total_order); Put<uint32_t>(&body, e.value.num_msg); Put<uint64_t>(&body, e.value.total_size); Put<uint64_t>(&body, e.offset); Put<uint64_t>(&body, e.size);
  std::vector<uint8_t> rec; Put<uint32_t>(&rec, kMagic); Put<uint16_t>(&rec, kVersion); Put<uint16_t>(&rec, 0); Put<uint32_t>(&rec, static_cast<uint32_t>(body.size())); rec.insert(rec.end(), body.begin(), body.end()); Put<uint32_t>(&rec, Crc32c(rec.data(), rec.size()));
  const off_t start = lseek(sidecar_fd_, 0, SEEK_END);
  if (start < 0) return false;
  // Test-only fault injection exercises the same rollback path a short/failed
  // write takes in production.  A failed append must never become an earlier
  // corrupt record that masks a subsequent retry after restart.
  size_t bytes = rec.size();
  if (const char* fail_after = std::getenv("EMBARCADERO_CORFU_SIDECAR_FAIL_AFTER_BYTES")) {
    const long n = std::strtol(fail_after, nullptr, 10);
    if (n >= 0 && static_cast<size_t>(n) < bytes) bytes = static_cast<size_t>(n);
  }
  const bool complete = WriteAll(sidecar_fd_, rec.data(), bytes) && bytes == rec.size() && fdatasync(sidecar_fd_) == 0;
  if (complete) return true;
  const bool rolled_back = ftruncate(sidecar_fd_, start) == 0 && fdatasync(sidecar_fd_) == 0;
  (void)rolled_back;  // caller receives IO_ERROR even if rollback itself also failed.
  return false;
}
CorfuWriteStatus CorfuReplicaStore::WriteOnce(const CorfuSlotKey& key, const CorfuValueId& value, uint64_t offset, const void* payload, uint64_t size) {
  std::lock_guard<std::mutex> l(mu_); auto it = slots_.find(key); if (it != slots_.end()) return it->second.state == CorfuSlotState::kValue && it->second.value == value ? CorfuWriteStatus::kAlreadySame : CorfuWriteStatus::kConflict;
  if (!payload || size != value.total_size) return CorfuWriteStatus::kIoError;
  if (!media_durable_) {
    // This is an actual remote memory-copy sink, not an accounting shortcut:
    // retain a private payload copy before acknowledging the ordered slot.
    Entry e{CorfuSlotState::kValue, value, offset, size};
    try {
      const auto* begin = static_cast<const uint8_t*>(payload);
      e.payload.assign(begin, begin + size);
    } catch (const std::exception&) {
      return CorfuWriteStatus::kIoError;
    }
    slots_.emplace(key, std::move(e));
    return CorfuWriteStatus::kWritten;
  }
  if (pwrite(data_fd_, payload, size, offset) != static_cast<ssize_t>(size) || fdatasync(data_fd_) != 0) return CorfuWriteStatus::kIoError;
  Entry e{CorfuSlotState::kValue, value, offset, size}; if (!AppendRecord(key, e)) return CorfuWriteStatus::kIoError; slots_.emplace(key, e); return CorfuWriteStatus::kWritten;
}
CorfuWriteStatus CorfuReplicaStore::WriteJunkOnce(const CorfuSlotKey& key) { std::lock_guard<std::mutex> l(mu_); auto it = slots_.find(key); if (it != slots_.end()) return it->second.state == CorfuSlotState::kJunk ? CorfuWriteStatus::kAlreadyJunk : CorfuWriteStatus::kConflict; Entry e{CorfuSlotState::kJunk,{ },0,0}; if (media_durable_ && !AppendRecord(key,e)) return CorfuWriteStatus::kIoError; slots_.emplace(key,e); return CorfuWriteStatus::kWritten; }
void CorfuReplicaStore::Replay() {
  const off_t end = lseek(sidecar_fd_, 0, SEEK_END); if (end < 0) throw std::runtime_error("cannot seek Corfu sidecar"); std::vector<uint8_t> bytes(static_cast<size_t>(end)); if (end && pread(sidecar_fd_, bytes.data(), bytes.size(), 0) != end) throw std::runtime_error("cannot read Corfu sidecar");
  size_t off = 0; while (off < bytes.size()) { const size_t start = off; if (bytes.size()-off < 12) break; const uint8_t* p=bytes.data()+off; const uint8_t* endp=bytes.data()+bytes.size(); uint32_t magic,len; uint16_t ver,res; if(!Get(p,endp,&magic)||!Get(p,endp,&ver)||!Get(p,endp,&res)||!Get(p,endp,&len)||magic!=kMagic||ver!=kVersion) throw std::runtime_error("corrupt Corfu sidecar header"); if (len > bytes.size() || static_cast<size_t>(endp-p) < len+4) break; const uint8_t* bodyend=p+len; uint16_t topic_len; CorfuSlotKey k; Entry e{}; uint8_t state; if(!Get(p,bodyend,&topic_len)||topic_len>kMaxTopic||static_cast<size_t>(bodyend-p)<topic_len) throw std::runtime_error("corrupt Corfu sidecar body"); k.topic.assign(reinterpret_cast<const char*>(p),topic_len);p+=topic_len; if(!Get(p,bodyend,&k.broker_id)||!Get(p,bodyend,&k.broker_batch_seq)||!Get(p,bodyend,&state)||!Get(p,bodyend,&e.value.client_id)||!Get(p,bodyend,&e.value.original_client_batch_seq)||!Get(p,bodyend,&e.value.total_order)||!Get(p,bodyend,&e.value.num_msg)||!Get(p,bodyend,&e.value.total_size)||!Get(p,bodyend,&e.offset)||!Get(p,bodyend,&e.size)||p!=bodyend) throw std::runtime_error("corrupt Corfu sidecar body"); uint32_t crc; const uint8_t* c=bodyend; if(!Get(c,endp,&crc)||crc!=Crc32c(bytes.data()+start, static_cast<size_t>(bodyend-(bytes.data()+start)))) { if (bodyend+4==endp) break; throw std::runtime_error("corrupt Corfu sidecar checksum"); } e.state=static_cast<CorfuSlotState>(state); if (e.state!=CorfuSlotState::kValue && e.state!=CorfuSlotState::kJunk) throw std::runtime_error("corrupt Corfu sidecar state"); if(!slots_.emplace(k,e).second) throw std::runtime_error("duplicate Corfu sidecar slot"); off=static_cast<size_t>(c-bytes.data()); }
  // Discard a torn tail before O_APPEND is used again.  Otherwise a valid later
  // record would permanently sit after the unreadable suffix.
  if (off != bytes.size() && ftruncate(sidecar_fd_, static_cast<off_t>(off)) != 0) {
    throw std::runtime_error("cannot truncate torn Corfu sidecar tail");
  }
}
}  // namespace Corfu
