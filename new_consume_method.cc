// New Consume method implementation that uses proper buffer management
// This replaces the broken implementation in subscriber.cc

void* Subscriber::Consume(int timeout_ms) {
    static size_t next_expected_order = 0;
    
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);
    
    // For Sequencer 5: Maintain per-connection batch state
    static absl::Mutex g_batch_states_mutex;
    static absl::flat_hash_map<int, ConnectionBatchState> g_batch_states;
    
    LOG(INFO) << "Consume: Starting with timeout=" << timeout_ms << "ms, order_level=" << order_level_;
    
    while (std::chrono::steady_clock::now() - start_time < timeout) {
        // Try to acquire data from any available connection
        absl::ReaderMutexLock map_lock(&connection_map_mutex_);
        for (auto const& [fd, conn_ptr] : connections_) {
            if (!conn_ptr) continue;
            
            // Try to acquire a buffer with data
            BufferState* buffer = conn_ptr->acquire_read_buffer();
            if (!buffer) continue; // No data available on this connection
            
            // We have data! Process it
            size_t buffer_write_offset = buffer->write_offset.load(std::memory_order_acquire);
            if (buffer_write_offset < sizeof(Embarcadero::MessageHeader)) {
                // Not enough data for even a message header
                conn_ptr->release_read_buffer(buffer);
                continue;
            }
            
            uint8_t* buffer_data = static_cast<uint8_t*>(buffer->buffer);
            size_t current_pos = 0;
            
            LOG(INFO) << "Consume: Processing buffer from fd=" << fd 
                     << ", buffer_size=" << buffer_write_offset << ", order_level=" << order_level_;
            
            // For Sequencer 5: Handle batch metadata processing
            if (order_level_ == 5) {
                absl::MutexLock batch_lock(&g_batch_states_mutex);
                ConnectionBatchState& batch_state = g_batch_states[fd];
                
                // Check if we need to process batch metadata first
                if (!batch_state.has_pending_metadata && 
                    buffer_write_offset >= current_pos + sizeof(BatchMetadata)) {
                    
                    BatchMetadata* metadata = reinterpret_cast<BatchMetadata*>(buffer_data + current_pos);
                    
                    // Better validation: Check if this could be batch metadata
                    // Look for reasonable values that indicate this is metadata, not message data
                    if (metadata->num_messages > 0 && metadata->num_messages <= 10000 &&
                        metadata->batch_total_order < 10000000) {  // Reasonable upper bound
                        
                        // This looks like batch metadata
                        batch_state.pending_metadata = *metadata;
                        batch_state.has_pending_metadata = true;
                        batch_state.current_batch_messages_processed = 0;
                        batch_state.next_message_order_in_batch = metadata->batch_total_order;
                        
                        LOG(INFO) << "Consume: Found batch metadata, total_order=" << metadata->batch_total_order
                                 << ", num_messages=" << metadata->num_messages << ", fd=" << fd;
                        
                        // Skip past metadata to first message
                        current_pos += sizeof(BatchMetadata);
                    } else {
                        LOG(INFO) << "Consume: Data doesn't look like batch metadata, num_messages=" 
                                 << metadata->num_messages << ", batch_total_order=" << metadata->batch_total_order 
                                 << ", fd=" << fd;
                    }
                }
            }
            
            // Process messages in the buffer
            while (current_pos + sizeof(Embarcadero::MessageHeader) <= buffer_write_offset) {
                Embarcadero::MessageHeader* header = 
                    reinterpret_cast<Embarcadero::MessageHeader*>(buffer_data + current_pos);
                
                // Check if we have the complete message
                if (current_pos + header->paddedSize > buffer_write_offset) {
                    // Incomplete message, wait for more data
                    LOG(INFO) << "Consume: Incomplete message, need " << header->paddedSize 
                             << " bytes, have " << (buffer_write_offset - current_pos) << ", fd=" << fd;
                    break;
                }
                
                // For Sequencer 5: Assign total_order from batch metadata
                if (order_level_ == 5) {
                    absl::MutexLock batch_lock(&g_batch_states_mutex);
                    ConnectionBatchState& batch_state = g_batch_states[fd];
                    
                    if (batch_state.has_pending_metadata && header->total_order == 0) {
                        header->total_order = batch_state.next_message_order_in_batch++;
                        batch_state.current_batch_messages_processed++;
                        
                        if (batch_state.current_batch_messages_processed >= 
                            batch_state.pending_metadata.num_messages) {
                            batch_state.has_pending_metadata = false;
                        }
                        
                        LOG(INFO) << "Consume: Assigned total_order=" << header->total_order 
                                 << " for Sequencer 5, fd=" << fd;
                    }
                }
                
                // Check if this is the message we want
                bool should_consume = false;
                if (order_level_ == 5) {
                    // For Sequencer 5: consume messages as they come
                    should_consume = true;
                    next_expected_order = header->total_order + 1;
                } else {
                    // For other order levels: enforce strict ordering
                    should_consume = (header->total_order == next_expected_order);
                }
                
                if (should_consume) {
                    LOG(INFO) << "Consume: Returning message with total_order=" << header->total_order
                             << ", paddedSize=" << header->paddedSize << ", fd=" << fd;
                    
                    // Release the buffer - the caller can process the message
                    conn_ptr->release_read_buffer(buffer);
                    
                    if (order_level_ != 5) {
                        next_expected_order++;
                    }
                    
                    return static_cast<void*>(header);
                }
                
                // Move to next message in buffer
                current_pos += header->paddedSize;
            }
            
            // No suitable message found in this buffer, release it
            conn_ptr->release_read_buffer(buffer);
        }
        
        // No data available from any connection, wait a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    LOG(INFO) << "Consume: Timeout reached after " << timeout_ms << "ms";
    return nullptr;
}

