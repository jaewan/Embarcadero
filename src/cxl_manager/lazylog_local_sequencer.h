#ifndef LAZYLOG_LOCAL_SEQUENCER_H
#define LAZYLOG_LOCAL_SEQUENCER_H

#include "common/config.h"
#include "cxl_datastructure.h"
#include <lazylog_sequencer.grpc.pb.h>
#include <atomic>

namespace LazyLog {

using Embarcadero::TInode;
using Embarcadero::MessageHeader;
using Embarcadero::BatchHeader;

class LazyLogLocalSequencer {
 public:
  LazyLogLocalSequencer(TInode* tinode, int broker_id, void* cxl_addr, std::string topic_str, BatchHeader* batch_header);

  void Register();
  void SendLocalProgress(std::string topic_str, volatile bool& stop_thread);
  void TerminateGlobalSequencer();
  void ReceiveGlobalBinding(std::unique_ptr<grpc::ClientReaderWriter<lazylogsequencer::LocalProgress, lazylogsequencer::GlobalBinding>>& stream);
  void ApplyGlobalBinding(const absl::btree_map<int, int>& global_binding);

 private:
  TInode* tinode_;
  int broker_id_;
  void* cxl_addr_;
  BatchHeader* batch_header_;
  std::unique_ptr<lazylogsequencer::LazyLogSequencer::Stub> stub_;
  std::atomic<bool> stop_reading_from_stream_{false};
  int local_epoch_ = 0;
  size_t next_global_sequence_{0};
  MessageHeader* msg_to_order_ = nullptr;
  size_t batch_header_idx_ = 0;
};

}  // namespace LazyLog

#endif
