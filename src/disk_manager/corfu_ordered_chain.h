#pragma once

#include "disk_manager/corfu_replica_store.h"
#include <functional>
#include <string>
#include <vector>

namespace Corfu {
struct CorfuAppendDescriptor { CorfuSlotKey slot; CorfuValueId value; uint64_t source_offset{}; const void* payload{}; uint64_t size{}; };
struct CorfuReplicaTarget { int chain_index{}; int broker_id{}; std::string endpoint; };
class CorfuChainEndpoint {
 public:
  virtual ~CorfuChainEndpoint() = default;
  virtual CorfuProbeResult Probe(const CorfuSlotKey&) = 0;
  virtual CorfuWriteStatus WriteOnce(const CorfuAppendDescriptor&) = 0;
  virtual CorfuWriteStatus WriteJunkOnce(const CorfuSlotKey&) = 0;
};
// Calls target i only after i-1 returned success.  The optional event callback
// exists for deterministic tests and has no role in protocol correctness.
class CorfuOrderedChain {
 public:
  using Event = std::function<void(int, bool)>;
  explicit CorfuOrderedChain(Event event = {}) : event_(std::move(event)) {}
  bool Append(const CorfuAppendDescriptor&, const std::vector<CorfuChainEndpoint*>&);
  bool CompleteHole(const CorfuAppendDescriptor&, const std::vector<CorfuChainEndpoint*>&);
  bool CompleteJunkHole(const CorfuSlotKey&, const std::vector<CorfuChainEndpoint*>&);
 private: Event event_;
};
}  // namespace Corfu
