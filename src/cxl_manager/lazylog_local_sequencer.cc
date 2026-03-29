#include "lazylog_local_sequencer.h"
#include "cxl_manager.h"
#include <cstdlib>
#include <cstring>
#include <limits>

namespace LazyLog {

LazyLogLocalSequencer::LazyLogLocalSequencer(
    TInode* tinode,
    int broker_id,
    void* cxl_addr,
    std::string /*topic_str*/,
    BatchHeader* batch_header)
    : tinode_(tinode),
      broker_id_(broker_id),
      cxl_addr_(cxl_addr),
      batch_header_(batch_header) {
  const char* sequencer_ip_env = std::getenv("EMBARCADERO_LAZYLOG_SEQ_IP");
  const char* sequencer_port_env = std::getenv("EMBARCADERO_LAZYLOG_SEQ_PORT");
  const std::string sequencer_ip =
      (sequencer_ip_env && std::strlen(sequencer_ip_env) > 0) ? sequencer_ip_env : LAZYLOG_SEQUENCER_IP;
  int sequencer_port = LAZYLOG_SEQ_PORT;
  if (sequencer_port_env && std::strlen(sequencer_port_env) > 0) {
    try {
      sequencer_port = std::stoi(sequencer_port_env);
    } catch (const std::exception& e) {
      LOG(WARNING) << "Invalid EMBARCADERO_LAZYLOG_SEQ_PORT='" << sequencer_port_env
                   << "', falling back to " << LAZYLOG_SEQ_PORT;
    }
  }
  std::string sequencer_addr = sequencer_ip + ":" + std::to_string(sequencer_port);
  auto channel = grpc::CreateChannel(sequencer_addr, grpc::InsecureChannelCredentials());
  stub_ = lazylogsequencer::LazyLogSequencer::NewStub(channel);
  Register();
}

void LazyLogLocalSequencer::Register() {
  lazylogsequencer::RegisterBrokerRequest request;
  request.set_broker_id(broker_id_);
  constexpr int kRegisterMaxAttempts = 10;
  constexpr auto kRegisterRetryDelay = std::chrono::milliseconds(200);
  for (int attempt = 1; attempt <= kRegisterMaxAttempts; ++attempt) {
    lazylogsequencer::RegisterBrokerResponse response;
    grpc::ClientContext context;
    grpc::Status status = stub_->HandleRegisterBroker(&context, request, &response);
    if (status.ok()) {
      return;
    }
    if (attempt == kRegisterMaxAttempts) {
      LOG(ERROR) << "LazyLog register failed broker=" << broker_id_
                 << " attempts=" << kRegisterMaxAttempts
                 << " err=" << status.error_message();
      return;
    }
    LOG(WARNING) << "LazyLog register retry broker=" << broker_id_
                 << " attempt=" << attempt
                 << " err=" << status.error_message();
    std::this_thread::sleep_for(kRegisterRetryDelay);
  }
}

void LazyLogLocalSequencer::TerminateGlobalSequencer() {
  lazylogsequencer::TerminateGlobalSequencerRequest request;
  lazylogsequencer::TerminateGlobalSequencerResponse response;
  grpc::ClientContext context;
  grpc::Status status = stub_->HandleTerminateGlobalSequencer(&context, request, &response);
  if (!status.ok()) {
    LOG(ERROR) << "LazyLog terminate sequencer failed: " << status.error_message();
  }
}

void LazyLogLocalSequencer::SendLocalProgress(std::string topic_str, volatile bool& stop_thread) {
  static const bool kCxlLazyLogMode = []() {
    const char* env = std::getenv("LAZYLOG_CXL_MODE");
    return env && std::string(env) == "1";
  }();

  grpc::ClientContext context;
  auto stream = stub_->HandleSendLocalProgress(&context);
  std::thread recv_thread(&LazyLogLocalSequencer::ReceiveGlobalBinding, this, std::ref(stream));

  while (!stop_thread) {
    int64_t local_progress = 0;
    const bool track_replication_progress =
        (kCxlLazyLogMode && tinode_->replication_factor > 0);
    if (track_replication_progress) {
      volatile uint64_t* rep_done_ptr = &tinode_->offsets[broker_id_].replication_done[broker_id_];
      Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
          reinterpret_cast<const volatile void*>(rep_done_ptr)));
      Embarcadero::CXL::full_fence();
      const uint64_t rep_done = *rep_done_ptr;
      Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
          reinterpret_cast<const volatile void*>(&tinode_->offsets[broker_id_].validated_written_byte_offset)));
      Embarcadero::CXL::full_fence();
      const size_t validated = tinode_->offsets[broker_id_].validated_written_byte_offset;
      const size_t log_start = tinode_->offsets[broker_id_].log_offset;
      local_progress = (validated <= log_start || rep_done == std::numeric_limits<uint64_t>::max())
          ? 0
          : static_cast<int64_t>(rep_done + 1);
    } else {
      // No replication durability frontier is available (e.g., replication_factor=0),
      // so progress must follow locally delegated message count.
      local_progress = static_cast<int64_t>(tinode_->offsets[broker_id_].written);
    }

    lazylogsequencer::LocalProgress request;
    request.set_local_progress(local_progress);
    request.set_topic(topic_str);
    request.set_broker_id(broker_id_);
    request.set_epoch(local_epoch_++);
    if (!stream->Write(request)) {
      LOG(ERROR) << "LazyLog local progress stream closed broker=" << broker_id_;
      break;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(LAZYLOG_SEQ_LOCAL_CUT_INTERVAL));
  }

  stream->WritesDone();
  grpc::Status finish_status = stream->Finish();
  if (!finish_status.ok()) {
    LOG(ERROR) << "LazyLog local progress stream finish error broker=" << broker_id_
               << " err=" << finish_status.error_message();
  }
  stop_reading_from_stream_.store(true, std::memory_order_release);
  if (recv_thread.joinable()) recv_thread.join();
  if (broker_id_ == 0) {
    TerminateGlobalSequencer();
  }
}

void LazyLogLocalSequencer::ReceiveGlobalBinding(
    std::unique_ptr<grpc::ClientReaderWriter<lazylogsequencer::LocalProgress, lazylogsequencer::GlobalBinding>>& stream) {
  while (!stop_reading_from_stream_.load(std::memory_order_acquire)) {
    lazylogsequencer::GlobalBinding binding;
    if (!stream->Read(&binding)) {
      break;
    }
    absl::btree_map<int, int> binding_map;
    for (const auto& entry : binding.global_binding()) {
      binding_map[static_cast<int>(entry.first)] = static_cast<int>(entry.second);
    }
    ApplyGlobalBinding(binding_map);
  }
}

void LazyLogLocalSequencer::ApplyGlobalBinding(const absl::btree_map<int, int>& global_binding) {
  if (msg_to_order_ == nullptr) {
    msg_to_order_ = reinterpret_cast<MessageHeader*>(
        reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].log_offset);
  }

  const size_t kNumBatchSlots = BATCHHEADERS_SIZE / sizeof(BatchHeader);
  size_t total_size = 0;
  void* start_addr = reinterpret_cast<void*>(msg_to_order_);
  bool local_progress = false;
  uint32_t current_batch_num_messages = 0;
  uint64_t current_batch_first_total_order = next_global_sequence_;

  auto publish_batch = [&](void* batch_start_addr,
                           size_t publish_size,
                           uint32_t publish_num_messages,
                           uint64_t publish_first_total_order) {
    if (publish_size == 0 || batch_start_addr == nullptr) return;
    const size_t slot = batch_header_idx_ % kNumBatchSlots;
    batch_header_[slot].batch_off_to_export = 0;
    batch_header_[slot].total_size = publish_size;
    batch_header_[slot].num_msg = publish_num_messages;
    batch_header_[slot].total_order = publish_first_total_order;
    batch_header_[slot].log_idx = static_cast<size_t>(
        static_cast<uint8_t*>(batch_start_addr) - static_cast<uint8_t*>(cxl_addr_));
    batch_header_[slot].ordered = 1;
    Embarcadero::CXL::flush_cacheline(&batch_header_[slot]);
    Embarcadero::CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(&batch_header_[slot]) + 64);
    Embarcadero::CXL::store_fence();
    batch_header_idx_++;
  };

  for (const auto& cut : global_binding) {
    if (cut.first == broker_id_) {
      for (int i = 0; i < cut.second; ++i) {
        local_progress = true;
        if (current_batch_num_messages == 0) {
          current_batch_first_total_order = next_global_sequence_;
        }
        current_batch_num_messages++;
        total_size += msg_to_order_->paddedSize;
        msg_to_order_->total_order = next_global_sequence_;
        std::atomic_thread_fence(std::memory_order_release);
        tinode_->offsets[broker_id_].ordered = msg_to_order_->logical_offset + 1;
        tinode_->offsets[broker_id_].ordered_offset =
            reinterpret_cast<uint8_t*>(msg_to_order_) - reinterpret_cast<uint8_t*>(cxl_addr_);
        Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
            static_cast<const volatile void*>(&tinode_->offsets[broker_id_].ordered)));
        Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
            static_cast<const volatile void*>(&tinode_->offsets[broker_id_].ordered_offset)));
        Embarcadero::CXL::store_fence();
        msg_to_order_ = reinterpret_cast<MessageHeader*>(
            reinterpret_cast<uint8_t*>(msg_to_order_) + msg_to_order_->next_msg_diff);
        ++next_global_sequence_;
        if (total_size >= BATCH_SIZE) {
          publish_batch(start_addr, total_size, current_batch_num_messages, current_batch_first_total_order);
          start_addr = reinterpret_cast<void*>(msg_to_order_);
          total_size = 0;
          current_batch_num_messages = 0;
          current_batch_first_total_order = next_global_sequence_;
        }
      }
    } else {
      next_global_sequence_ += static_cast<size_t>(cut.second);
    }
  }

  if (local_progress && total_size > 0) {
    publish_batch(start_addr,
                  total_size,
                  std::max<uint32_t>(current_batch_num_messages, 1),
                  current_batch_first_total_order);
  }
}

}  // namespace LazyLog
