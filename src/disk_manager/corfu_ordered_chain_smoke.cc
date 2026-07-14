#include "disk_manager/corfu_ordered_chain.h"
#include "disk_manager/corfu_replica_store.h"
#include <cstdio>
#include <algorithm>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace Corfu;
struct Fake final : CorfuChainEndpoint {
  CorfuProbeResult state{}; bool fail{false}; std::vector<std::string>* events; int index;
  Fake(std::vector<std::string>* event_log, int chain_index) : events(event_log), index(chain_index) {}
  CorfuProbeResult Probe(const CorfuSlotKey&) override { return state; }
  CorfuWriteStatus WriteOnce(const CorfuAppendDescriptor& d) override { events->push_back("start"+std::to_string(index)); if(fail)return CorfuWriteStatus::kIoError; state={CorfuSlotState::kValue,d.value};events->push_back("done"+std::to_string(index));return CorfuWriteStatus::kWritten; }
  CorfuWriteStatus WriteJunkOnce(const CorfuSlotKey&) override { state={CorfuSlotState::kJunk,{}}; return CorfuWriteStatus::kWritten; }
};
bool Check(bool b, const char* message) { if(!b) std::cerr << "FAIL: " << message << '\n'; return b; }
std::string Temp(const char* suffix) { return "/tmp/corfu_chain_smoke_" + std::to_string(getpid()) + suffix; }
}
int main() {
  const CorfuAppendDescriptor d{{"topic", 2, 9}, {7, 11, 42, 3, 4}, 128, "data", 4};
  std::vector<std::string> events; Fake a(&events,1), b(&events,2); std::vector<CorfuChainEndpoint*> chain{&a,&b};
  CorfuOrderedChain ordered([&](int i,bool done){events.push_back(std::string(done?"pdone":"pstart")+std::to_string(i));});
  if (!Check(ordered.Append(d,{}),"RF1 primary only") || !Check(events==std::vector<std::string>({"pstart0","pdone0"}),"RF1 event order")) return 1;
  events.clear(); if(!Check(ordered.Append(d,chain),"RF3 append") || !Check(events==std::vector<std::string>({"pstart0","pdone0","pstart1","start1","done1","pdone1","pstart2","start2","done2","pdone2"}),"RF3 strictly serial")) return 1;
  events.clear(); a.state={};b.state={};a.fail=true; if(!Check(!ordered.Append(d,chain),"failure fails") || !Check(std::find(events.begin(),events.end(),"start2")==events.end(),"tail never starts after prefix failure")) return 1;
  events.clear(); a.fail=false; b.fail=true; a.state={}; b.state={}; if(!Check(!ordered.Append(d,chain),"tail failure fails") || !Check(std::find(events.begin(),events.end(),"done2")==events.end(),"tail failure has no tail completion")) return 1;
  b.fail=false; a.state={CorfuSlotState::kValue,d.value}; b.state={}; events.clear(); if(!Check(ordered.CompleteHole(d,chain),"suffix completion") || !Check(std::find(events.begin(),events.end(),"start1")==events.end(),"written prefix untouched"))return 1;
  a.state={}; b.state={CorfuSlotState::kValue,d.value}; if(!Check(!ordered.CompleteHole(d,chain),"non-prefix rejected"))return 1;
  const std::string data=Temp(".data"), side=Temp(".side"); unlink(data.c_str());unlink(side.c_str());
  { CorfuReplicaStore store(data,side); if(!Check(store.WriteOnce(d.slot,d.value,d.source_offset,d.payload,d.size)==CorfuWriteStatus::kWritten,"durable value") || !Check(store.WriteOnce(d.slot,d.value,d.source_offset,d.payload,d.size)==CorfuWriteStatus::kAlreadySame,"idempotent same") || !Check(store.WriteOnce(d.slot,{8,11,42,3,4},d.source_offset,d.payload,d.size)==CorfuWriteStatus::kConflict,"conflict") || !Check(store.WriteJunkOnce({"topic",2,10})==CorfuWriteStatus::kWritten,"durable junk"))return 1; }
  { CorfuReplicaStore store(data,side); if(!Check(store.Probe(d.slot).state==CorfuSlotState::kValue,"replay value") || !Check(store.Probe({"topic",2,10}).state==CorfuSlotState::kJunk,"replay junk"))return 1; }
  // A torn final record is deliberately ignored; it must not poison earlier state.
  int fd=open(side.c_str(),O_WRONLY|O_APPEND); const char torn[]="partial"; if (write(fd,torn,sizeof(torn)) < 0) return 1; close(fd);
  { CorfuReplicaStore store(data,side); if(!Check(store.Probe(d.slot).state==CorfuSlotState::kValue,"ignore torn final") || !Check(store.WriteJunkOnce({"topic",2,11})==CorfuWriteStatus::kWritten,"append after truncating torn tail"))return 1; }
  { CorfuReplicaStore store(data,side); if(!Check(store.Probe({"topic",2,11}).state==CorfuSlotState::kJunk,"replay append after torn tail"))return 1; }
  // Corruption in a non-final record is never treated as a torn tail.
  const std::string bad_data=Temp(".bad.data"), bad_side=Temp(".bad.side"); unlink(bad_data.c_str()); unlink(bad_side.c_str());
  { CorfuReplicaStore store(bad_data,bad_side); if (store.WriteOnce(d.slot,d.value,d.source_offset,d.payload,d.size)!=CorfuWriteStatus::kWritten || store.WriteJunkOnce({"topic",2,12})!=CorfuWriteStatus::kWritten) return 1; }
  fd=open(bad_side.c_str(),O_RDWR); unsigned char zero=0; if (pwrite(fd,&zero,1,0)!=1) return 1; close(fd); bool rejected=false;
  try { CorfuReplicaStore store(bad_data,bad_side); } catch (const std::exception&) { rejected=true; }
  if(!Check(rejected,"reject corruption before final record"))return 1;
  // A partial append failure rolls itself back, so a retry is durable and
  // replayable rather than hidden behind an earlier malformed fragment.
  const std::string rollback_data=Temp(".rollback.data"), rollback_side=Temp(".rollback.side"); unlink(rollback_data.c_str()); unlink(rollback_side.c_str());
  setenv("EMBARCADERO_CORFU_SIDECAR_FAIL_AFTER_BYTES", "8", 1);
  { CorfuReplicaStore store(rollback_data,rollback_side); if(!Check(store.WriteOnce(d.slot,d.value,d.source_offset,d.payload,d.size)==CorfuWriteStatus::kIoError,"inject partial sidecar append")) return 1; }
  unsetenv("EMBARCADERO_CORFU_SIDECAR_FAIL_AFTER_BYTES");
  { CorfuReplicaStore store(rollback_data,rollback_side); if(!Check(store.WriteOnce(d.slot,d.value,d.source_offset,d.payload,d.size)==CorfuWriteStatus::kWritten,"retry after rolled-back append")) return 1; }
  { CorfuReplicaStore store(rollback_data,rollback_side); if(!Check(store.Probe(d.slot).state==CorfuSlotState::kValue,"restart after retry")) return 1; }
  unlink(rollback_data.c_str()); unlink(rollback_side.c_str()); unlink(bad_data.c_str()); unlink(bad_side.c_str());
  unlink(data.c_str());unlink(side.c_str()); std::cout << "corfu ordered-chain/sidecar smoke passed\n"; return 0;
}
