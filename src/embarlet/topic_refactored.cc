#include "topic_refactored.h"
#include <glog/logging.h>

namespace Embarcadero {

TopicRefactored::TopicRefactored(const std::string& topic_name,
                                 void* cxl_addr,
                                 TInode* tinode,
                                 TInode* replica_tinode,
                                 int broker_id,
                                 SequencerType seq_type,
                                 int order,
                                 int ack_level,
                                 int replication_factor)
    : topic_name_(topic_name),
      cxl_addr_(cxl_addr),
      tinode_(tinode),
      replica_tinode_(replica_tinode),
      broker_id_(broker_id),
      seq_type_(seq_type),
      order_(order),
      ack_level_(ack_level),
      replication_factor_(replication_factor) {}

TopicRefactored::~TopicRefactored() {
    Stop();
}

bool TopicRefactored::Initialize() {
    // Calculate first message address
    first_message_addr_ = reinterpret_cast<uint8_t*>(cxl_addr_) + 
                         tinode_->offsets[broker_id_].log_offset + CACHELINE_SIZE;

    // Initialize buffer manager
    buffer_manager_ = std::make_unique<BufferManager>(
        cxl_addr_,
        tinode_,
        broker_id_,
        seq_type_,
        order_
    );

    // Initialize replication manager
    replication_manager_ = std::make_unique<ReplicationManager>(
        topic_name_,
        broker_id_,
        replication_factor_,
        seq_type_,
        tinode_,
        replica_tinode_
    );

    if (!replication_manager_->Initialize()) {
        LOG(ERROR) << "Failed to initialize replication manager";
        return false;
    }

    // Set replication clients in buffer manager
    if (seq_type_ == CORFU && replication_manager_->GetCorfuClient()) {
        buffer_manager_->SetCorfuReplicationClient(replication_manager_->GetCorfuClient());
    } else if (seq_type_ == SCALOG && replication_manager_->GetScalogClient()) {
        buffer_manager_->SetScalogReplicationClient(replication_manager_->GetScalogClient());
    }

    // Initialize message ordering
    message_ordering_ = std::make_unique<MessageOrdering>(
        cxl_addr_,
        tinode_,
        broker_id_
    );

    // Initialize message export
    message_export_ = std::make_unique<MessageExport>(
        cxl_addr_,
        first_message_addr_,
        tinode_,
        broker_id_,
        order_,
        ack_level_,
        replication_factor_
    );

    // Initialize combiner for non-ordered messages
    if (order_ == 0) {
        message_combiner_ = std::make_unique<MessageCombiner>(
            cxl_addr_,
            first_message_addr_,
            tinode_,
            replica_tinode_,
            broker_id_
        );
    }

    return true;
}

void TopicRefactored::Start() {
    // Start message ordering/sequencer
    if (message_ordering_) {
        message_ordering_->StartSequencer(seq_type_, order_, topic_name_);
    }

    // Start combiner for non-ordered messages
    if (message_combiner_) {
        message_combiner_->Start();
    }
}

void TopicRefactored::Stop() {
    // Stop all components
    if (message_ordering_) {
        message_ordering_->StopSequencer();
    }

    if (message_combiner_) {
        message_combiner_->Stop();
    }
}

void TopicRefactored::GetCXLBuffer(BatchHeader& batch_header,
                                   void*& log,
                                   size_t& logical_offset,
                                   std::function<void(size_t, size_t)>& callback) {
    // Delegate to buffer manager
    buffer_manager_->GetCXLBuffer(batch_header, log, logical_offset, callback);

    // Update replication done if needed
    if (callback && replication_factor_ > 0 && get_num_brokers_callback_) {
        auto original_callback = callback;
        callback = [this, original_callback](size_t start_offset, size_t end_offset) {
            original_callback(start_offset, end_offset);
            replication_manager_->UpdateReplicationDone(end_offset, get_num_brokers_callback_);
        };
    }
}

bool TopicRefactored::GetMessageAddr(size_t& last_offset,
                                    void*& last_addr,
                                    void*& messages,
                                    size_t& messages_size) {
    // Update message export with combiner state if using combiner
    if (message_combiner_) {
        message_export_->SetWrittenState(
            message_combiner_->GetWrittenLogicalOffset(),
            message_combiner_->GetWrittenPhysicalAddr()
        );
    }

    return message_export_->GetMessageAddr(last_offset, last_addr, messages, messages_size);
}

bool TopicRefactored::GetBatchToExport(size_t& expected_batch_offset,
                                      void*& batch_addr,
                                      size_t& batch_size) {
    return message_export_->GetBatchToExport(expected_batch_offset, batch_addr, batch_size);
}

} // namespace Embarcadero
