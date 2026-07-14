#include "disk_manager/corfu_ordered_chain.h"
namespace Corfu {
namespace { bool ValueComplete(CorfuWriteStatus s) { return s == CorfuWriteStatus::kWritten || s == CorfuWriteStatus::kAlreadySame; } }
bool CorfuOrderedChain::Append(const CorfuAppendDescriptor& d, const std::vector<CorfuChainEndpoint*>& chain) {
  if (event_) { event_(0, false); event_(0, true); }  // CXL primary publication precedes all RPCs.
  for (size_t i=0;i<chain.size();++i) { if (!chain[i]) return false; if(event_) event_(static_cast<int>(i+1),false); const auto s=chain[i]->WriteOnce(d); if(!ValueComplete(s)) return false; if(event_) event_(static_cast<int>(i+1),true); } return true;
}
bool CorfuOrderedChain::CompleteHole(const CorfuAppendDescriptor& d, const std::vector<CorfuChainEndpoint*>& chain) {
  bool saw_unwritten=false;
  for (size_t i=0;i<chain.size();++i) { if(!chain[i]) return false; const auto p=chain[i]->Probe(d.slot); if (!saw_unwritten && p.state==CorfuSlotState::kValue && p.value==d.value) continue; if (!saw_unwritten && p.state==CorfuSlotState::kUnwritten) saw_unwritten=true; else if (!saw_unwritten) return false; else if (p.state != CorfuSlotState::kUnwritten) return false; if(event_)event_(static_cast<int>(i+1),false); const auto s=chain[i]->WriteOnce(d); if(!ValueComplete(s))return false; if(event_)event_(static_cast<int>(i+1),true); } return true;
}
bool CorfuOrderedChain::CompleteJunkHole(const CorfuSlotKey& key, const std::vector<CorfuChainEndpoint*>& chain) {
  bool saw_unwritten=false; for(size_t i=0;i<chain.size();++i) { if(!chain[i]) return false; const auto p=chain[i]->Probe(key); if(!saw_unwritten && p.state==CorfuSlotState::kJunk) continue; if(!saw_unwritten && p.state==CorfuSlotState::kUnwritten) saw_unwritten=true; else if(!saw_unwritten || p.state!=CorfuSlotState::kUnwritten) return false; if(event_)event_(static_cast<int>(i+1),false); const auto s=chain[i]->WriteJunkOnce(key); if(s!=CorfuWriteStatus::kWritten && s!=CorfuWriteStatus::kAlreadyJunk)return false; if(event_)event_(static_cast<int>(i+1),true); } return true;
}
}  // namespace Corfu
