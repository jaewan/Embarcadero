#include "topic.h"
#include "../cxl_manager/scalog_local_sequencer.h"

#include <cstring>
#include <chrono>
#include <thread>

namespace Embarcadero {

Topic::Topic(
		GetNewSegmentCallback get_new_segment,
		GetNumBrokersCallback get_num_brokers_callback,
		GetRegisteredBrokersCallback get_registered_brokers_callback,
		void* TInode_addr,
		TInode* replica_tinode,
		const char* topic_name,
		int broker_id,
		int order,
		SequencerType seq_type,
		void* cxl_addr,
		void* segment_metadata):
	get_new_segment_callback_(get_new_segment),
	get_num_brokers_callback_(get_num_brokers_callback),
	get_registered_brokers_callback_(get_registered_brokers_callback),
	tinode_(static_cast<struct TInode*>(TInode_addr)),
	replica_tinode_(replica_tinode),
	topic_name_(topic_name),
	broker_id_(broker_id),
	order_(order),
	seq_type_(seq_type),
	cxl_addr_(cxl_addr),
	logical_offset_(0),
	written_logical_offset_((size_t)-1),
	current_segment_(segment_metadata) {

		// Validate tinode pointer first
		if (!tinode_) {
			LOG(FATAL) << "TInode is null for topic: " << topic_name;
		}
		
		// Validate offsets before using them
		if (tinode_->offsets[broker_id_].log_offset == 0) {
			LOG(ERROR) << "Invalid log_offset for broker " << broker_id_ << " in topic: " << topic_name
			           << ". Waiting for tinode initialization...";
			
			// Wait for tinode to be initialized with a timeout
			int wait_count = 0;
			const int max_wait = 100; // 10 seconds max wait (100 * 100ms)
			while (tinode_->offsets[broker_id_].log_offset == 0 && wait_count < max_wait) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				wait_count++;
			}
			
			if (tinode_->offsets[broker_id_].log_offset == 0) {
				LOG(FATAL) << "Tinode not initialized after " << (max_wait * 100) 
				           << "ms for broker " << broker_id_ << " in topic: " << topic_name;
			}
			
			LOG(INFO) << "Tinode initialized after " << (wait_count * 100) 
			          << "ms for broker " << broker_id_ << " in topic: " << topic_name;
		}
		
		// Initialize addresses based on offsets
		log_addr_.store(static_cast<unsigned long long int>(
					reinterpret_cast<uintptr_t>(cxl_addr_) + tinode_->offsets[broker_id_].log_offset));

		batch_headers_ = static_cast<unsigned long long int>(
				reinterpret_cast<uintptr_t>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);

		first_message_addr_ = reinterpret_cast<uint8_t*>(cxl_addr_) + 
			tinode_->offsets[broker_id_].log_offset;

		first_batch_headers_addr_ = reinterpret_cast<uint8_t*>(cxl_addr_) + 
			tinode_->offsets[broker_id_].batch_headers_offset;

		ack_level_ = tinode_->ack_level;
		replication_factor_ = tinode_->replication_factor;
		ordered_offset_addr_ = nullptr;
		ordered_offset_ = 0;

		// Set appropriate get buffer function based on sequencer type
		if (seq_type == KAFKA) {
			GetCXLBufferFunc = &Topic::KafkaGetCXLBuffer;
		} else if (seq_type == CORFU) {
			// Initialize Corfu replication client
			// TODO(Jae) change this to actual replica address
			corfu_replication_client_ = std::make_unique<Corfu::CorfuReplicationClient>(
					topic_name, 
					replication_factor_, 
					"127.0.0.1:" + std::to_string(CORFU_REP_PORT)
					);

			if (!corfu_replication_client_->Connect()) {
				LOG(ERROR) << "Corfu replication client failed to connect to replica";
			}

			GetCXLBufferFunc = &Topic::CorfuGetCXLBuffer;
		} else if (seq_type == SCALOG) {
			if (replication_factor_ > 0) {
				scalog_replication_client_ = std::make_unique<Scalog::ScalogReplicationClient>(
					topic_name, 
					replication_factor_, 
					"localhost",
					broker_id_ // broker_id used to determine the port
				);
				
				if (!scalog_replication_client_->Connect()) {
					LOG(ERROR) << "Scalog replication client failed to connect to replica";
				}
			}
			GetCXLBufferFunc = &Topic::ScalogGetCXLBuffer;
		} else {
			// Set buffer function based on order
			if (order_ == 3) {
				GetCXLBufferFunc = &Topic::Order3GetCXLBuffer;
			} else if (order_ == 4) {
				GetCXLBufferFunc = &Topic::Order4GetCXLBuffer;
			} else {
				GetCXLBufferFunc = &Topic::EmbarcaderoGetCXLBuffer;
			}
		}


		// Ensure all initialization is complete before starting threads
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		
		// Start combiner if needed
		if (seq_type == CORFU || (seq_type != KAFKA && order_ != 4)) {
			combiningThreads_.emplace_back(&Topic::CombinerThread, this);
		}

		// Head node runs sequencer
		if(broker_id == 0){
			LOG(INFO) << "Topic constructor: broker_id=" << broker_id << ", order=" << order << ", seq_type=" << seq_type;
			switch(seq_type){
				case KAFKA: // Kafka is just a way to not run CombinerThread, not actual sequencer
				case EMBARCADERO:
					if (order == 1)
						LOG(ERROR) << "Sequencer 1 is not ported yet from cxl_manager";
						//sequencerThread_ = std::thread(&Topic::Sequencer1, this);
					else if (order == 2)
						LOG(ERROR) << "Sequencer 2 is not ported yet";
						//sequencerThread_ = std::thread(&Topic::Sequencer2, this);
					else if (order == 3)
						LOG(ERROR) << "Sequencer 3 is not ported yet";
						//sequencerThread_ = std::thread(&Topic::Sequencer3, this);
					else if (order == 4){
						sequencerThread_ = std::thread(&Topic::Sequencer4, this);
					}
					else if (order == 5){
						LOG(INFO) << "Creating Sequencer5 thread for order level 5";
						sequencerThread_ = std::thread(&Topic::Sequencer5, this);
					}
					break;
				case SCALOG:
					if (order == 1){
						sequencerThread_ = std::thread(&Topic::StartScalogLocalSequencer, this);
						// Already started when creating topic instance from topic manager
					}else if (order == 2)
						LOG(ERROR) << "Order is set 2 at scalog";
					break;
				case CORFU:
					if (order == 0 || order == 4)
						VLOG(3) << "Order " << order << 
							" for Corfu is right as messages are written ordered. Combiner combining is enough";
					else 
						LOG(ERROR) << "Wrong Order is set for corfu " << order;
					break;
				default:
					LOG(ERROR) << "Unknown sequencer:" << seq_type;
					break;
			}
	}
}

void Topic::StartScalogLocalSequencer() {
	// int unique_port = SCALOG_SEQ_PORT + scalog_local_sequencer_port_offset_.fetch_add(1);
	BatchHeader* batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	Scalog::ScalogLocalSequencer scalog_local_sequencer(tinode_, broker_id_, cxl_addr_, topic_name_, batch_header);
	scalog_local_sequencer.SendLocalCut(topic_name_, stop_threads_);
}


inline void Topic::UpdateTInodeWritten(size_t written, size_t written_addr) {
	// Update replica tinode if it exists
	if (tinode_->replicate_tinode && replica_tinode_) {
		replica_tinode_->offsets[broker_id_].written = written;
		replica_tinode_->offsets[broker_id_].written_addr = written_addr;
	}

	// Update primary tinode
	tinode_->offsets[broker_id_].written = written;
	tinode_->offsets[broker_id_].written_addr = written_addr;
}

void Topic::CombinerThread() {
	// Validate first_message_addr_ before using it
	if (!first_message_addr_ || first_message_addr_ == cxl_addr_) {
		LOG(FATAL) << "Invalid first_message_addr_ in CombinerThread for topic: " << topic_name_
		           << ". first_message_addr_=" << first_message_addr_ 
		           << ", cxl_addr_=" << cxl_addr_
		           << ", log_offset=" << tinode_->offsets[broker_id_].log_offset;
		return;
	}
	
	// Additional safety check - ensure we have enough space before the first message for the segment header
	if (reinterpret_cast<uintptr_t>(first_message_addr_) < reinterpret_cast<uintptr_t>(cxl_addr_) + CACHELINE_SIZE) {
		LOG(FATAL) << "first_message_addr_ too close to cxl_addr_ base, cannot access segment header safely. "
		           << "first_message_addr_=" << first_message_addr_ 
		           << ", cxl_addr_=" << cxl_addr_;
		return;
	}
	
	// Initialize header pointers
	void* segment_header = reinterpret_cast<uint8_t*>(first_message_addr_) - CACHELINE_SIZE;
	MessageHeader* header = reinterpret_cast<MessageHeader*>(first_message_addr_);
	
	// Initialize the memory region to ensure it's safe to access
	// Zero out the first message header to ensure complete=0 initially
	memset(header, 0, sizeof(MessageHeader));
	
	// CombinerThread started for topic

	// NEW APPROACH: Use batch-based processing instead of message-by-message
	// Initialize batch header pointer to match EmbarcaderoGetCXLBuffer allocation
	BatchHeader* current_batch = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	
	// Track the first batch header to detect when we've processed all available batches
	BatchHeader* first_batch = current_batch;
	size_t processed_batches = 0;
	
		// CombinerThread: Starting batch processing

	while (!stop_threads_) {
		// NEW: Try batch-based processing first
		if (current_batch && __atomic_load_n(&current_batch->batch_complete, __ATOMIC_ACQUIRE)) {
			// CombinerThread: Found completed batch
			// Process this completed batch
			if (current_batch->num_msg > 0) {
				MessageHeader* batch_first_msg = reinterpret_cast<MessageHeader*>(
					reinterpret_cast<uint8_t*>(cxl_addr_) + current_batch->log_idx);
				
				// Process all messages in this batch efficiently
				MessageHeader* msg_ptr = batch_first_msg;
				for (size_t i = 0; i < current_batch->num_msg; ++i) {
					// Set required fields for each message
					msg_ptr->logical_offset = logical_offset_;
					msg_ptr->segment_header = reinterpret_cast<uint8_t*>(msg_ptr) - CACHELINE_SIZE;
					msg_ptr->next_msg_diff = msg_ptr->paddedSize;

					// Update segment header
					*reinterpret_cast<unsigned long long int*>(msg_ptr->segment_header) =
						static_cast<unsigned long long int>(
							reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(msg_ptr->segment_header));

					// Move to next message in batch
					if (i < current_batch->num_msg - 1) {
						msg_ptr = reinterpret_cast<MessageHeader*>(
							reinterpret_cast<uint8_t*>(msg_ptr) + msg_ptr->paddedSize);
					}
					logical_offset_++;
				}

				// Update TInode and tracking with the last message in the batch
				UpdateTInodeWritten(
					logical_offset_ - 1, 
					static_cast<unsigned long long int>(
						reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_)));

				written_logical_offset_ = logical_offset_ - 1;
				written_physical_addr_ = reinterpret_cast<void*>(msg_ptr);

				processed_batches++;
				VLOG(3) << "CombinerThread: Processed batch " << processed_batches 
				        << " with " << current_batch->num_msg << " messages";

				// Move to next batch
				current_batch = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
				continue; // Skip the old message-by-message processing
			}
		} else if (current_batch && current_batch->num_msg > 0) {
		// CombinerThread: Waiting for batch completion (reduced logging)
		}

		// FALLBACK: Old message-by-message processing for compatibility
		// Safe memory access with bounds checking
		try {
			// Validate header pointer before accessing
			if (reinterpret_cast<uintptr_t>(header) < reinterpret_cast<uintptr_t>(cxl_addr_) ||
			    reinterpret_cast<uintptr_t>(header) >= reinterpret_cast<uintptr_t>(cxl_addr_) + (1ULL << 36)) {
				LOG(ERROR) << "CombinerThread: Invalid header pointer " << header 
				           << " for topic " << topic_name_ << ", broker " << broker_id_;
				break;
			}
		} catch (...) {
			LOG(ERROR) << "CombinerThread: Exception accessing memory at " << header 
			           << " for topic " << topic_name_ << ", broker " << broker_id_;
			break;
		}

		// CRITICAL FIX: Wait for message to be complete before processing
		// CombinerThread must wait for paddedSize to be set by network thread
		// This prevents reading corrupted/partial paddedSize values
		volatile size_t padded_size;
		do {
			if (stop_threads_) return;
			// Use memory barrier to ensure fresh read from memory
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			padded_size = header->paddedSize;
			if (padded_size == 0) {
				std::this_thread::yield();
			}
		} while (padded_size == 0);
		
		// Additional validation: ensure paddedSize is reasonable
		const size_t min_msg_size = sizeof(MessageHeader);
		const size_t max_msg_size = 1024 * 1024; // 1MB max message size
		if (padded_size < min_msg_size || padded_size > max_msg_size) {
			static thread_local size_t error_count = 0;
			if (++error_count % 1000 == 1) {
				LOG(ERROR) << "CombinerThread: Invalid paddedSize=" << padded_size 
				           << " for topic " << topic_name_ << ", broker " << broker_id_
				           << " (error #" << error_count << ")";
			}
			std::this_thread::yield();
			continue;
		}

#ifdef MULTISEGMENT
		// Handle segment transition
		if (header->next_msg_diff != 0) { // Moved to new segment
			header = reinterpret_cast<MessageHeader*>(
					reinterpret_cast<uint8_t*>(header) + header->next_msg_diff);
			segment_header = reinterpret_cast<uint8_t*>(header) - CACHELINE_SIZE;
			continue;
		}
#endif

		// Update message metadata
		header->segment_header = segment_header;
		header->logical_offset = logical_offset_;
		header->next_msg_diff = padded_size;

		// Ensure write ordering with a memory fence
		//std::atomic_thread_fence(std::memory_order_release);

		// Update tinode with write information
		UpdateTInodeWritten(
				logical_offset_, 
				static_cast<unsigned long long int>(
					reinterpret_cast<uint8_t*>(header) - reinterpret_cast<uint8_t*>(cxl_addr_))
				);

		// Update segment header
		*reinterpret_cast<unsigned long long int*>(segment_header) =
			static_cast<unsigned long long int>(
					reinterpret_cast<uint8_t*>(header) - reinterpret_cast<uint8_t*>(segment_header)
					);

		// Update tracking variables
		written_logical_offset_ = logical_offset_;
		written_physical_addr_ = reinterpret_cast<void*>(header);

		// Move to next message using validated padded_size
		MessageHeader* next_header = reinterpret_cast<MessageHeader*>(
				reinterpret_cast<uint8_t*>(header) + padded_size);
		
		// Validate next header pointer
		if (reinterpret_cast<uintptr_t>(next_header) < reinterpret_cast<uintptr_t>(cxl_addr_) ||
		    reinterpret_cast<uintptr_t>(next_header) >= reinterpret_cast<uintptr_t>(cxl_addr_) + (1ULL << 36)) {
			// Log only occasionally to avoid spam
			static thread_local size_t pointer_error_count = 0;
			if (++pointer_error_count % 1000 == 1) {
				LOG(WARNING) << "CombinerThread: Invalid next header pointer " << next_header 
				           << " (diff=" << header->next_msg_diff << ") for topic " 
				           << topic_name_ << ", broker " << broker_id_ 
				           << " (error #" << pointer_error_count << ")";
			}
			// Just yield CPU, don't sleep
			std::this_thread::yield();
			continue;
		}
		
		header = next_header;
		logical_offset_++;
	}
}

void Topic::GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers){
	//TODO(Jae) Placeholder
	if (!get_registered_brokers_callback_(registered_brokers, nullptr /* msg_to_order removed */, tinode_)) {
		LOG(ERROR) << "GetRegisteredBrokerSet: Callback failed to get registered brokers.";
		registered_brokers.clear(); // Ensure set is empty on failure
	}
}

void Topic::Sequencer4() {
	absl::btree_set<int> registered_brokers;
	GetRegisteredBrokerSet(registered_brokers);

	global_seq_ = 0;

	std::vector<std::thread> sequencer4_threads;
	for (int broker_id : registered_brokers) {
		sequencer4_threads.emplace_back(
			&Topic::BrokerScannerWorker,
			this, // Pass pointer to current object
			broker_id
		);
	}

	// Join worker threads
	for(auto &t : sequencer4_threads){
		while(!t.joinable()){
			std::this_thread::yield();
		}
		t.join();
	}
}

// This does not work with multi-segments as it advances to next messaeg with message's size
void Topic::BrokerScannerWorker(int broker_id) {
	// TODO(Jae) tinode it references should be replica_tinode if replcate_tinode
	// Wait until tinode of the broker is initialized by the broker
	// Sequencer4 relies on GetRegisteredBrokerSet that does not wait
	while(tinode_->offsets[broker_id].log_offset == 0){
		std::this_thread::yield();
	}
	// Get the starting point for this broker's batch header log
	BatchHeader* current_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
	if (!current_batch_header) {
		LOG(ERROR) << "Scanner [Broker " << broker_id << "]: Failed to calculate batch header start address.";
		return;
	}
	BatchHeader* header_for_sub = current_batch_header;

	// client_id -> <batch_seq, header*>
	absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>> skipped_batches; 

	while (!stop_threads_) {
		// 1. Check for new Batch Header (Use memory_order_acquire for visibility)
		volatile size_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;

		// No new batch written.
		if (num_msg_check == 0 || current_batch_header->log_idx == 0) {
			if(!ProcessSkipped(skipped_batches, header_for_sub)){
				std::this_thread::yield();
			}
			continue;
		}

		// 2. Check if this batch is the next expected one for the client
		BatchHeader* header_to_process = current_batch_header;
		size_t client_id = current_batch_header->client_id;
		size_t batch_seq = current_batch_header->batch_seq;
		bool ready_to_order = false;
		size_t expected_seq = 0;
		size_t start_total_order = 0;
		bool skip_batch = false;
		{
			absl::MutexLock lock(&global_seq_batch_seq_mu_);
			auto map_it = next_expected_batch_seq_.find(client_id);
			if (map_it == next_expected_batch_seq_.end()) {
				// New client
				if (batch_seq == 0) {
					expected_seq = 0;
					start_total_order = global_seq_;
					global_seq_ += header_to_process->num_msg;
					next_expected_batch_seq_[client_id] = 1; // Expect 1 next
					ready_to_order = true;
				} else {
					skip_batch = true;
					ready_to_order = false;
					VLOG(4) << "Scanner [B" << broker_id << "]: New client " << client_id << ", skipping non-zero first batch " << batch_seq;
				}
			} else {
				// Existing client
				expected_seq = map_it->second;
				if (batch_seq == expected_seq) {
					start_total_order = global_seq_;
					global_seq_ += header_to_process->num_msg;
					map_it->second = expected_seq + 1;
					ready_to_order = true;
				} else if (batch_seq > expected_seq) {
					// Out of order batch, skip (outside lock)
					skip_batch = true;
					ready_to_order = false;
				} else {
					// Duplicate or older batch - ignore
					ready_to_order = false;
					LOG(WARNING) << "Scanner [B" << broker_id << "]: Duplicate/old batch seq "
						<< batch_seq << " detected from client " << client_id << " (expected " << expected_seq << ")";
				}
			}
		}

		if (skip_batch){
			skipped_batches[client_id][batch_seq] = header_to_process;
		}

		// 3. Queue if ready
		if (ready_to_order) {
			AssignOrder(header_to_process, start_total_order, header_for_sub);
			ProcessSkipped(skipped_batches, header_for_sub);
		}

		// 4. Advance to next batch header (handle segment/log wrap around)
		current_batch_header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader)
				);
	} // end of main while loop
}

// Helper to process skipped batches for a specific client after a batch was enqueued
bool Topic::ProcessSkipped(absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>>& skipped_batches,
		BatchHeader* &header_for_sub){

	bool processed_any = false;
	auto client_skipped_it = skipped_batches.begin();
	while (client_skipped_it != skipped_batches.end()){
		size_t client_id = client_skipped_it->first;
		auto& client_skipped_map = client_skipped_it->second; // Ref to btree_map

		size_t start_total_order;
		bool batch_processed;
		do {
			batch_processed = false;
			size_t expected_seq;
			BatchHeader* batch_header = nullptr;
			auto batch_it = client_skipped_map.end();
			{ // --- Critical section START ---
				absl::MutexLock lock(&global_seq_batch_seq_mu_);

				auto map_it = next_expected_batch_seq_.find(client_id);
				// If client somehow disappeared, stop (shouldn't happen)
				if (map_it == next_expected_batch_seq_.end()) break;

				expected_seq = map_it->second;
				batch_it = client_skipped_map.find(expected_seq); // Find expected in skipped

				if (batch_it != client_skipped_map.end()) {
					// Found it! Reserve sequence and update expected batch number
					batch_header = batch_it->second;
					start_total_order = global_seq_;
					global_seq_ += batch_header->num_msg;
					map_it->second = expected_seq + 1;
					batch_processed = true; // Mark to proceed outside lock
					processed_any = true; // Mark that we did *some* work
					VLOG(4) << "ProcessSkipped [B?]: Client " << client_id << ", processing skipped batch " << expected_seq << ", reserving seq [" << start_total_order << ", " << global_seq_ << ")";
				} else {
					// Next expected not found in skipped map for this client, move to next client
					break; // Exit inner do-while loop for this client
				}
			}
			if (batch_processed && batch_header) {
				client_skipped_map.erase(batch_it); // Erase AFTER successful lock/update
				AssignOrder(batch_header, start_total_order, header_for_sub);
			}
			// If batch_processed is true, loop again for same client in case next seq was also skipped
		} while (batch_processed && !client_skipped_map.empty()); // Keep checking if we processed one

		if (client_skipped_map.empty()) {
			skipped_batches.erase(client_skipped_it++);
		}else{
			++client_skipped_it;
		}
	} 
	return processed_any;
}

void Topic::AssignOrder(BatchHeader *batch_to_order, size_t start_total_order, BatchHeader* &header_for_sub) {
	int broker = batch_to_order->broker_id;

	// **Assign Global Order using Atomic fetch_add**
	size_t num_messages = batch_to_order->num_msg;
	if (num_messages == 0) {
		LOG(WARNING) << "!!!! Orderer: Dequeued batch with zero messages. Skipping !!!";
		return;
	}

	// Sequencer 4: Keep per-message completion checking (batch_complete not set by network thread)

	// Get pointer to the first message
	MessageHeader* msg_header = reinterpret_cast<MessageHeader*>(
			batch_to_order->log_idx + reinterpret_cast<uint8_t*>(cxl_addr_)
			);
	if (!msg_header) {
		LOG(ERROR) << "Orderer: Failed to calculate message address for logical offset " << batch_to_order->log_idx;
		return;
	}
	size_t seq = start_total_order;
	batch_to_order->total_order = seq;

	size_t logical_offset = batch_to_order->start_logical_offset;

	for (size_t i = 0; i < num_messages; ++i) {
		// Sequencer 4: Wait for each message to be complete (network thread doesn't set batch_complete)
		while (msg_header->paddedSize == 0) {
			if (stop_threads_) return;
			std::this_thread::yield();
		}

		// 2. Read paddedSize AFTER completion check 
		size_t current_padded_size = msg_header->paddedSize;

		// 3. Assign order and set next pointer difference
		msg_header->logical_offset = logical_offset;
		logical_offset++;
		msg_header->total_order = seq;
		seq++;
		msg_header->next_msg_diff = current_padded_size;

		// 4. Make total_order and next_msg_diff visible before readers might use them
		//std::atomic_thread_fence(std::memory_order_release);

		// With Seq4 with batch optimization these are just counters
		tinode_->offsets[broker].ordered++;
		tinode_->offsets[broker].written++;

		msg_header = reinterpret_cast<MessageHeader*>(
				reinterpret_cast<uint8_t*>(msg_header) + current_padded_size
				);
	} // End message loop
	header_for_sub->batch_off_to_export = (reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(header_for_sub));
	header_for_sub->ordered = 1;
	header_for_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header_for_sub) + sizeof(BatchHeader));
}

/**
 * Check and handle segment boundary crossing
 */
void Topic::CheckSegmentBoundary(
		void* log, 
		size_t msgSize, 
		unsigned long long int segment_metadata) {

	const uintptr_t log_addr = reinterpret_cast<uintptr_t>(log);
	const uintptr_t segment_end = segment_metadata + SEGMENT_SIZE;

	// Check if message would cross segment boundary
	if (segment_end <= log_addr + msgSize) {
		LOG(ERROR) << "Segment size limit reached (" << SEGMENT_SIZE 
			<< "). Increase SEGMENT_SIZE";

		// TODO(Jae) Implement segment boundary crossing
		if (segment_end <= log_addr) {
			// Allocate a new segment when log is entirely in next segment
		} else {
			// Wait for first thread that crossed segment to allocate new segment
		}
	}
}

std::function<void(void*, size_t)> Topic::KafkaGetCXLBuffer(
		BatchHeader &batch_header, 
		const char topic[TOPIC_NAME_SIZE], 
		void* &log, 
		void* &segment_header, 
		size_t &logical_offset,
		BatchHeader* &batch_header_location) {

	// Set batch header location to nullptr (not used by Kafka sequencer)
	batch_header_location = nullptr;
	
	size_t start_logical_offset;

	{
		absl::MutexLock lock(&mutex_);

		// Allocate space in the log
		log = reinterpret_cast<void*>(log_addr_.fetch_add(batch_header.total_size));
		logical_offset = logical_offset_;
		segment_header = current_segment_;
		start_logical_offset = logical_offset_;
		logical_offset_ += batch_header.num_msg;

		// Check for segment boundary issues
		if (reinterpret_cast<unsigned long long int>(current_segment_) + SEGMENT_SIZE <= log_addr_) {
			LOG(ERROR) << "!!!!!!!!! Increase the Segment Size: " << SEGMENT_SIZE;
			// TODO(Jae) Finish below segment boundary crossing code
		}
	}

	// Return completion callback function
	return [this, start_logical_offset](void* log_ptr, size_t logical_offset) {
		absl::MutexLock lock(&written_mutex_);

		if (kafka_logical_offset_.load() != start_logical_offset) {
			// Save for later processing
			written_messages_range_[start_logical_offset] = logical_offset;
		} else {
			// Process now and check for consecutive messages
			size_t start = start_logical_offset;
			bool has_next_messages_written = false;

			do {
				has_next_messages_written = false;

				// Update tracking state
				written_logical_offset_ = logical_offset;
				written_physical_addr_ = log_ptr;

				// Mark message as processed
				reinterpret_cast<MessageHeader*>(log_ptr)->logical_offset = static_cast<size_t>(-1);

				// Update TInode
				UpdateTInodeWritten(
						logical_offset, 
						static_cast<unsigned long long int>(
							reinterpret_cast<uint8_t*>(log_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_))
						);

				// Update segment header
				*reinterpret_cast<unsigned long long int*>(current_segment_) =
					static_cast<unsigned long long int>(
							reinterpret_cast<uint8_t*>(log_ptr) - 
							reinterpret_cast<uint8_t*>(current_segment_)
							);

				// Move to next logical offset
				kafka_logical_offset_.store(logical_offset + 1);

				// Check if next message is already written
				if (written_messages_range_.contains(logical_offset + 1)) {
					start = logical_offset + 1;
					logical_offset = written_messages_range_[start];
					written_messages_range_.erase(start);
					has_next_messages_written = true;
				}
			} while (has_next_messages_written);
		}
	};
}

std::function<void(void*, size_t)> Topic::CorfuGetCXLBuffer(
		BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE],
		void* &log,
		void* &segment_header,
		size_t &logical_offset,
		BatchHeader* &batch_header_location) {

	// Set batch header location to nullptr (not used by Corfu sequencer)
	batch_header_location = nullptr;
	
	// Calculate addresses
	const unsigned long long int segment_metadata = 
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;
	BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);

	// Get log address with batch offset
	log = reinterpret_cast<void*>(log_addr_.load()
			+ batch_header.log_idx);

	// Check for segment boundary issues
	CheckSegmentBoundary(log, msg_size, segment_metadata);

	batch_header_log[batch_header.batch_seq].batch_seq = batch_header.batch_seq;
	batch_header_log[batch_header.batch_seq].total_size = batch_header.total_size;
	batch_header_log[batch_header.batch_seq].broker_id = broker_id_;
	batch_header_log[batch_header.batch_seq].ordered = 0;
	batch_header_log[batch_header.batch_seq].batch_off_to_export = 0;
	batch_header_log[batch_header.batch_seq].log_idx = static_cast<size_t>(
			reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_)
			);

	// Return replication callback
	return [this, batch_header, log](void* log_ptr, size_t /*placeholder*/) {
		BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);
		// Handle replication if needed
		if (replication_factor_ > 0 && corfu_replication_client_) {
			MessageHeader *header = (MessageHeader*)log;
			// Wait until the message is combined
			while(header->next_msg_diff == 0){
				std::this_thread::yield();
			}

			corfu_replication_client_->ReplicateData(
					batch_header.log_idx,
					batch_header.total_size,
					log
					);

			// Marking replication done
			size_t last_offset = header->logical_offset + batch_header.num_msg - 1;
			for (int i = 0; i < replication_factor_; i++) {
				int num_brokers = get_num_brokers_callback_();
				int b = (broker_id_ + num_brokers - i) % num_brokers;
				if (tinode_->replicate_tinode) {
					replica_tinode_->offsets[b].replication_done[broker_id_] = last_offset;
				}
				tinode_->offsets[b].replication_done[broker_id_] = last_offset;
			}
		}
		// This ensures in Corfu tinode.ordered collects the number of messages replicated
		{
			absl::MutexLock lock(&mutex_);
			tinode_->offsets[broker_id_].ordered += batch_header.num_msg;
		}
		batch_header_log[batch_header.batch_seq].ordered = 1;
	};
}

std::function<void(void*, size_t)> Topic::Order3GetCXLBuffer(
		BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE],
		void* &log,
		void* &segment_header,
		size_t &logical_offset,
		BatchHeader* &batch_header_location) {

	// Set batch header location to nullptr (not used by Order3 sequencer)
	batch_header_location = nullptr;
	
	absl::MutexLock lock(&mutex_);

	static size_t num_brokers = get_num_brokers_callback_();
	// Check if this batch was previously skipped
	if (skipped_batch_.contains(batch_header.client_id)) {
		auto& client_batches = skipped_batch_[batch_header.client_id];
		auto it = client_batches.find(batch_header.batch_seq);

		if (it != client_batches.end()) {
			log = it->second;
			client_batches.erase(it);
			return nullptr;
		}
	}

	// Initialize client tracking if needed
	if (!order3_client_batch_.contains(batch_header.client_id)) {
		order3_client_batch_.emplace(batch_header.client_id, broker_id_);
	}

	// Handle all skipped batches
	auto& client_seq = order3_client_batch_[batch_header.client_id];
	while (client_seq < batch_header.batch_seq) {
		// Allocate space for skipped batch
		void* skipped_addr = reinterpret_cast<void*>(log_addr_.load());

		// Store for later retrieval
		skipped_batch_[batch_header.client_id].emplace(client_seq, skipped_addr);

		// Move log address forward (assuming same batch size)
		log_addr_ += batch_header.total_size;

		// Update client sequence
		client_seq += num_brokers;
	}

	// Allocate space for this batch
	log = reinterpret_cast<void*>(log_addr_.load());
	log_addr_ += batch_header.total_size;
	client_seq += num_brokers;

	return nullptr;
}

std::function<void(void*, size_t)> Topic::Order4GetCXLBuffer(
		BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE],
		void* &log,
		void* &segment_header,
		size_t &logical_offset,
		BatchHeader* &batch_header_location) {

	// Calculate base addresses
	const unsigned long long int segment_metadata = 
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;
	void* batch_headers_log;

	{
		absl::MutexLock lock(&mutex_);

		// Allocate space in log
		log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));

		// Allocate space for batch header
		batch_headers_log = reinterpret_cast<void*>(batch_headers_);
		batch_headers_ += sizeof(BatchHeader);
		logical_offset = logical_offset_;
		logical_offset_ += batch_header.num_msg;
	}

	// Check for segment boundary
	CheckSegmentBoundary(log, msg_size, segment_metadata);

	// Update batch header fields
	batch_header.start_logical_offset = logical_offset;
	batch_header.broker_id = broker_id_;
	batch_header.ordered = 0;
	batch_header.total_order = 0;
	batch_header.log_idx = static_cast<size_t>(
			reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_)
			);

	// Store batch header and initialize completion flag
	memcpy(batch_headers_log, &batch_header, sizeof(BatchHeader));
	// Ensure batch_complete is initialized to 0 for Sequencer 5
	reinterpret_cast<BatchHeader*>(batch_headers_log)->batch_complete = 0;
	
	// Return the batch header location for completion signaling
	batch_header_location = reinterpret_cast<BatchHeader*>(batch_headers_log);

	return nullptr;
}

std::function<void(void*, size_t)> Topic::ScalogGetCXLBuffer(
        BatchHeader &batch_header,
        const char topic[TOPIC_NAME_SIZE],
        void* &log,
        void* &segment_header,
        size_t &logical_offset,
        BatchHeader* &batch_header_location) {
    
    // Set batch header location to nullptr (not used by Scalog sequencer)
    batch_header_location = nullptr;
    
    static std::atomic<size_t> batch_offset = 0;
    batch_header.log_idx = batch_offset.fetch_add(batch_header.total_size); 

	// Calculate addresses
	const unsigned long long int segment_metadata = 
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;

	// Allocate space in log
	log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));

	// Check for segment boundary
	CheckSegmentBoundary(log, msg_size, segment_metadata);

	// Return replication callback
	return [this, batch_header, log](void* log_ptr, size_t /*placeholder*/) {
		// Handle replication if needed
		if (replication_factor_ > 0 && scalog_replication_client_) {
				scalog_replication_client_->ReplicateData(
						batch_header.log_idx,
						batch_header.total_size,
						batch_header.num_msg,
						log
				);
		}
	};
}

std::function<void(void*, size_t)> Topic::EmbarcaderoGetCXLBuffer(
		BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE],
		void* &log,
		void* &segment_header,
		size_t &logical_offset,
		BatchHeader* &batch_header_location) {

	// Calculate base addresses
	const unsigned long long int segment_metadata = 
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;
	void* batch_headers_log;

	{
		absl::MutexLock lock(&mutex_);

		// Allocate space in log
		log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));

		// Allocate space for batch header
		batch_headers_log = reinterpret_cast<void*>(batch_headers_);
		batch_headers_ += sizeof(BatchHeader);
		logical_offset = logical_offset_;
		logical_offset_ += batch_header.num_msg;
	}

	// Check for segment boundary
	CheckSegmentBoundary(log, msg_size, segment_metadata);

	// Update batch header fields
	batch_header.start_logical_offset = logical_offset;
	batch_header.broker_id = broker_id_;
	batch_header.ordered = 0;
	batch_header.total_order = 0;
	batch_header.log_idx = static_cast<size_t>(
			reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_)
			);

	// Store batch header to the batch header ring and initialize completion flag
	memcpy(batch_headers_log, &batch_header, sizeof(BatchHeader));
	// Ensure batch_complete is initialized to 0 for Sequencer 5
	reinterpret_cast<BatchHeader*>(batch_headers_log)->batch_complete = 0;
	
	// Return the batch header location for completion signaling
	batch_header_location = reinterpret_cast<BatchHeader*>(batch_headers_log);

	return nullptr;
}

/*
 * Return one Ordered or Processed batch at a time
 * Current implementation expects ordered_batch is set accordingly (processed or ordered)
 * Should only call with Order 4 for now
 */
bool Topic::GetBatchToExport(
		size_t &expected_batch_offset,
		void* &batch_addr,
		size_t &batch_size) {
	static BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	BatchHeader* header = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * expected_batch_offset);
	if (header->ordered == 0){
		return false;
	}
	header = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header) + (int)(header->batch_off_to_export));
	batch_size = header->total_size;
	batch_addr = header->log_idx + reinterpret_cast<uint8_t*>(cxl_addr_);
	expected_batch_offset++;

	return true;
}

bool Topic::GetBatchToExportWithMetadata(
		size_t &expected_batch_offset,
		void* &batch_addr,
		size_t &batch_size,
		size_t &batch_total_order,
		uint32_t &num_messages) {
	static BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	
	// CRITICAL FIX for Sequencer 5: Search for next available ordered batch instead of expecting sequential order
	// Sequencer 5 processes batches in arrival order, not necessarily sequential batch offset order
	const size_t MAX_SEARCH_BATCHES = 1000;  // Reasonable upper bound to avoid infinite search
	size_t search_offset = expected_batch_offset;
	
	for (size_t i = 0; i < MAX_SEARCH_BATCHES; ++i) {
		BatchHeader* header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * search_offset);
		
		// Check if this batch is ordered and ready for export
		if (header->ordered == 1) {
			// Found an ordered batch! Use it for export
			header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(header) + (int)(header->batch_off_to_export));
			batch_size = header->total_size;
			batch_addr = header->log_idx + reinterpret_cast<uint8_t*>(cxl_addr_);
			
			// Extract batch metadata for Sequencer 5
			batch_total_order = header->total_order;
			num_messages = header->num_msg;
			
			// Update expected_batch_offset to continue from the next position
			expected_batch_offset = search_offset + 1;
			
			VLOG(4) << "GetBatchToExportWithMetadata: Found ordered batch at offset " << search_offset
			        << ", total_order=" << batch_total_order << ", num_messages=" << num_messages;
			return true;
		}
		
		// Try next batch position
		search_offset++;
	}
	
	// No ordered batches found in reasonable search range
	VLOG(4) << "GetBatchToExportWithMetadata: No ordered batches found starting from offset " << expected_batch_offset;
	return false;
}

/**
 * Get message address and size for topic subscribers
 *
 * Note: Current implementation depends on the subscriber knowing the physical
 * address of last fetched message. This is only true if messages were exported
 * from CXL. For disk cache optimization, we'd need to implement indexing.
 *
 * @return true if more messages are available
 */
bool Topic::GetMessageAddr(
		size_t &last_offset,
		void* &last_addr,
		void* &messages,
		size_t &messages_size) {

	// Determine current read position based on order
	size_t combined_offset;
	void* combined_addr;

	if (order_ > 0) {
		combined_offset = tinode_->offsets[broker_id_].ordered;
		combined_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + 
			tinode_->offsets[broker_id_].ordered_offset;
		if(ack_level_ == 2){
			//TODO(Jae) make replication also write written amount in the replication_done
			size_t r[replication_factor_];
			size_t min = (size_t)-1;
			for (int i = 0; i < replication_factor_; i++) {
				int b = (broker_id_ + NUM_MAX_BROKERS - i) % NUM_MAX_BROKERS;
				r[i] = tinode_->offsets[b].replication_done[broker_id_];
				if (min > r[i]) {
					min = r[i];
				}
			}
			if(min == (size_t)-1){
				return false;
			}
			if(combined_offset != min){
				combined_addr = reinterpret_cast<uint8_t*>(combined_addr) -
		(reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize * (combined_offset-min));
				combined_offset = min;
			}
		}
	} else {
		combined_offset = written_logical_offset_;
		combined_addr = written_physical_addr_;
	}

	// Check if we have new messages
	if (combined_offset == static_cast<size_t>(-1) ||
			(last_addr != nullptr && combined_offset <= last_offset)) {
		return false;
	}

	// Find start message location
	MessageHeader* start_msg_header;

	if (last_addr != nullptr) {
		start_msg_header = static_cast<MessageHeader*>(last_addr);

		// Wait for message to be combined if necessary
		while (start_msg_header->next_msg_diff == 0) {
			std::this_thread::yield();
		}

		// Move to next message
		start_msg_header = reinterpret_cast<MessageHeader*>(
				reinterpret_cast<uint8_t*>(start_msg_header) + start_msg_header->next_msg_diff
				);
	} else {
		// Start from first message
		if (combined_addr <= last_addr) {
			LOG(ERROR) << "GetMessageAddr: Invalid address relationship";
			return false;
		}
		start_msg_header = static_cast<MessageHeader*>(first_message_addr_);
	}

	// Verify message is valid
	if (start_msg_header->paddedSize == 0) {
		return false;
	}

	// Set output message pointer
	messages = static_cast<void*>(start_msg_header);

#ifdef MULTISEGMENT
	// Multi-segment logic for determining message size and last offset
	unsigned long long int* segment_offset_ptr = 
		static_cast<unsigned long long int*>(start_msg_header->segment_header);

	MessageHeader* last_msg_of_segment = reinterpret_cast<MessageHeader*>(
			reinterpret_cast<uint8_t*>(segment_offset_ptr) + *segment_offset_ptr
			);

	if (combined_addr < last_msg_of_segment) {
		// Last message is not fully ordered yet
		messages_size = reinterpret_cast<uint8_t*>(combined_addr) -
			reinterpret_cast<uint8_t*>(start_msg_header) +
			reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize;
		last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
		last_addr = combined_addr;
	} else {
		// Return entire segment of messages
		messages_size = reinterpret_cast<uint8_t*>(last_msg_of_segment) -
			reinterpret_cast<uint8_t*>(start_msg_header) +
			last_msg_of_segment->paddedSize;
		last_offset = last_msg_of_segment->logical_offset;
		last_addr = static_cast<void*>(last_msg_of_segment);
	}
#else
	// Single-segment logic for determining message size and last offset
	messages_size = reinterpret_cast<uint8_t*>(combined_addr) -
		reinterpret_cast<uint8_t*>(start_msg_header) +
		reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize;

	last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
	last_addr = combined_addr;
#endif

	return true;
}
// Sequencer 5: Batch-level sequencer implementation for Topic class
void Topic::Sequencer5() {
	LOG(INFO) << "Starting Sequencer5 for topic: " << topic_name_;
	absl::btree_set<int> registered_brokers;
	GetRegisteredBrokerSet(registered_brokers);

	global_seq_ = 0;

	std::vector<std::thread> sequencer5_threads;
	for (int broker_id : registered_brokers) {
		sequencer5_threads.emplace_back(
			&Topic::BrokerScannerWorker5,
			this,
			broker_id
		);
	}

	// Join worker threads
	for(auto &t : sequencer5_threads){
		while(!t.joinable()){
			std::this_thread::yield();
		}
		t.join();
	}
}

void Topic::BrokerScannerWorker5(int broker_id) {
	LOG(INFO) << "BrokerScannerWorker5 starting for broker " << broker_id;
	// Wait until tinode of the broker is initialized
	while(tinode_->offsets[broker_id].log_offset == 0){
		std::this_thread::yield();
	}
	LOG(INFO) << "BrokerScannerWorker5 broker " << broker_id << " initialized, starting scan loop";

	BatchHeader* ring_start_default = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
	BatchHeader* current_batch_header = ring_start_default;
	
	BatchHeader* header_for_sub = ring_start_default;

	absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>> skipped_batches;

	size_t loop_count = 0;
	size_t total_batches_processed = 0;
	auto last_log_time = std::chrono::steady_clock::now();
	
	while (!stop_threads_) {
		volatile size_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;

		// Debug: Log what we're seeing every 10M iterations
		if (loop_count % 10000000 == 0 && loop_count > 0) {
			VLOG(2) << "BrokerScannerWorker5 [B" << broker_id << "]: Sample values - num_msg=" 
					<< num_msg_check << ", log_idx=" << current_batch_header->log_idx;
		}
		
		// Validate batch header - check for reasonable message count (max 100000 messages per batch)
		if (num_msg_check == 0 || current_batch_header->log_idx == 0 || num_msg_check > 100000) {
			if (!ProcessSkipped5(skipped_batches, header_for_sub)) {
				std::this_thread::yield();
			}
			
			// Debug logging every 10000000 iterations
			if (++loop_count % 10000000 == 0) {
				VLOG(2) << "BrokerScannerWorker5 [B" << broker_id << "]: Still scanning, no batches found after " << loop_count << " iterations";
			}
			continue;  // DON'T advance pointer - wait for batch to become ready at current position
		}

		// OPTIMIZED: Batch completion check with fallback validation
		// Only check completion for batches that look valid (reduces unnecessary atomic reads)
		if (__builtin_expect(__atomic_load_n(&current_batch_header->batch_complete, __ATOMIC_ACQUIRE) == 0, 0)) {
			// FALLBACK: Check if all messages in the batch have paddedSize set (alternative completion check)
			// This handles edge cases where batch_complete flag is not set but messages are actually ready
			bool all_messages_complete = true;
			MessageHeader* msg_header = reinterpret_cast<MessageHeader*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + current_batch_header->log_idx);
			
			for (size_t i = 0; i < num_msg_check && i < 10; ++i) {  // Check up to 10 messages for efficiency
				if (msg_header->paddedSize == 0) {
					all_messages_complete = false;
					break;
				}
				if (i < num_msg_check - 1) {  // Don't advance past last message
					msg_header = reinterpret_cast<MessageHeader*>(
						reinterpret_cast<uint8_t*>(msg_header) + msg_header->paddedSize);
				}
			}
			
			if (!all_messages_complete) {
				// Log waiting for completion every 1M iterations to debug hanging
				static thread_local size_t wait_count = 0;
				if (++wait_count % 1000000 == 0) {
					LOG(WARNING) << "BrokerScannerWorker5 [B" << broker_id << "]: Waiting for batch completion, client_id=" 
								<< current_batch_header->client_id << ", batch_seq=" << current_batch_header->batch_seq 
								<< ", num_msg=" << num_msg_check << ", wait_iterations=" << wait_count;
				}
				std::this_thread::yield();
				continue;  // DON'T advance pointer - wait for batch completion
			} else {
				// Messages are complete but batch_complete flag not set - log and proceed
				LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: Processing batch without batch_complete flag (messages ready), client_id=" 
						<< current_batch_header->client_id << ", batch_seq=" << current_batch_header->batch_seq;
			}
		}

		// Valid batch found
		VLOG(3) << "BrokerScannerWorker5 [B" << broker_id << "]: Found valid batch with " << num_msg_check << " messages, batch_seq=" << current_batch_header->batch_seq << ", client_id=" << current_batch_header->client_id;

		BatchHeader* header_to_process = current_batch_header;
		size_t client_id = current_batch_header->client_id;
		size_t batch_seq = current_batch_header->batch_seq;
		bool ready_to_order = false;
		size_t start_total_order = 0;
		bool skip_batch = false;

		// SIMPLIFIED: Process all batches as they arrive (like order level 0)
		// No strict sequencing - just assign total_order and process immediately
		{
			absl::MutexLock lock(&global_seq_batch_seq_mu_);
			start_total_order = global_seq_;
			global_seq_ += header_to_process->num_msg;
			ready_to_order = true;
			skip_batch = false;
			
			VLOG(4) << "Scanner5 [B" << broker_id << "]: Processing batch from client " << client_id 
					<< ", batch_seq=" << batch_seq << ", total_order=[" << start_total_order 
					<< ", " << (start_total_order + header_to_process->num_msg) << ")";
		}

		// Since we're not skipping batches anymore, always process
		if (ready_to_order) {
			AssignOrder5(header_to_process, start_total_order, header_for_sub);
			total_batches_processed++;
		}

		// Periodic status logging
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
			LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: Processed " << total_batches_processed 
			          << " batches, current tinode.ordered=" << tinode_->offsets[broker_id].ordered;
			last_log_time = now;
		}
		
		// CRITICAL FIX: Handle accumulated skipped batches with timeout
		static thread_local auto last_batch_time = std::chrono::steady_clock::now();
		static thread_local size_t last_batch_count = 0;
		static thread_local auto last_skip_cleanup = std::chrono::steady_clock::now();
		
		if (ready_to_order) {
			last_batch_time = now;
			last_batch_count = total_batches_processed;
		}
		
		// No gap processing needed since we process all batches immediately
		
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_batch_time).count() >= 10) {
			// No new batches for 10 seconds - check if we're missing final batches
			if (tinode_->offsets[broker_id].ordered >= 655000 && tinode_->offsets[broker_id].ordered < 655360) {
				LOG(WARNING) << "DIAGNOSTIC: BrokerScannerWorker5 [B" << broker_id << "] may be missing final batches. "
				             << "Current ordered=" << tinode_->offsets[broker_id].ordered << ", expected=655360. "
				             << "Last batch processed 10+ seconds ago. Current batch info: num_msg=" << num_msg_check 
				             << ", batch_seq=" << current_batch_header->batch_seq;
				
				// RECOVERY ATTEMPT: Try to find and process any remaining valid batches
				// Look ahead in the ring buffer for batches that might have been missed
				BatchHeader* search_header = current_batch_header;
				for (int search_offset = 0; search_offset < 100; ++search_offset) {
					volatile size_t search_num_msg = search_header->num_msg;
					size_t num_msg_to_check = search_num_msg; // Copy to non-volatile for std::min
					if (search_num_msg > 0 && search_num_msg < 1000 && search_header->log_idx > 0) {
						// Found a potentially valid batch
						LOG(INFO) << "RECOVERY: Found potential batch at offset " << search_offset 
						          << ", num_msg=" << search_num_msg << ", batch_seq=" << search_header->batch_seq;
						
						// Try to process this batch using the fallback mechanism
						MessageHeader* msg_header = reinterpret_cast<MessageHeader*>(
							reinterpret_cast<uint8_t*>(cxl_addr_) + search_header->log_idx);
						
						bool batch_ready = true;
						for (size_t i = 0; i < std::min(num_msg_to_check, (size_t)5); ++i) {
							if (msg_header->paddedSize == 0) {
								batch_ready = false;
								break;
							}
							if (i < num_msg_to_check - 1) {
								msg_header = reinterpret_cast<MessageHeader*>(
									reinterpret_cast<uint8_t*>(msg_header) + msg_header->paddedSize);
							}
						}
						
						if (batch_ready) {
							LOG(INFO) << "RECOVERY: Processing recovered batch with " << search_num_msg << " messages";
							// Set current header to this batch and let the main loop process it
							current_batch_header = search_header;
							break;
						}
					}
					
					search_header = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(search_header) + sizeof(BatchHeader)
					);
				}
				
				last_batch_time = now; // Reset timer to avoid spam
			}
		}

		current_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader)
		);
	}
}

bool Topic::ProcessSkipped5(
	absl::flat_hash_map<size_t, absl::btree_map<size_t, BatchHeader*>>& skipped_batches,
	BatchHeader*& header_for_sub) {
	
	bool processed_any = false;
	auto client_skipped_it = skipped_batches.begin();
	while (client_skipped_it != skipped_batches.end()) {
		size_t client_id = client_skipped_it->first;
		auto& client_skipped_map = client_skipped_it->second;

		size_t start_total_order;
		bool batch_processed;
		do {
			batch_processed = false;
			size_t expected_seq;
			BatchHeader* batch_header = nullptr;
			auto batch_it = client_skipped_map.end();
			{
				absl::MutexLock l(&global_seq_batch_seq_mu_);
				auto it = next_expected_batch_seq_.find(client_id);
				expected_seq = (it != next_expected_batch_seq_.end()) ? it->second : 0;
				batch_it = client_skipped_map.find(expected_seq);
				if (batch_it != client_skipped_map.end()) {
					batch_header = batch_it->second;
					start_total_order = global_seq_;
					global_seq_ += batch_header->num_msg;
					next_expected_batch_seq_[client_id] = expected_seq + 1;
					batch_processed = true;
					processed_any = true;
					VLOG(4) << "ProcessSkipped5 [B?]: Client " << client_id 
							<< ", processing skipped batch " << expected_seq 
							<< ", reserving seq [" << start_total_order << ", " << (start_total_order + batch_header->num_msg) << ")";
				}
			}
			if (batch_processed && batch_header) {
				client_skipped_map.erase(batch_it);
				AssignOrder5(batch_header, start_total_order, header_for_sub);
			}
		} while (batch_processed && !client_skipped_map.empty());

		if (client_skipped_map.empty()) {
			skipped_batches.erase(client_skipped_it++);
		} else {
			++client_skipped_it;
		}
	}
	return processed_any;
}

void Topic::AssignOrder5(BatchHeader* batch_to_order, size_t start_total_order, BatchHeader*& header_for_sub) {
	int broker = batch_to_order->broker_id;

	size_t num_messages = batch_to_order->num_msg;
	if (num_messages == 0) {
		LOG(WARNING) << "!!!! Orderer5: Dequeued batch with zero messages. Skipping !!!";
		return;
	}

	// Pure batch-level ordering - set only batch total_order, no message-level processing
	batch_to_order->total_order = start_total_order;

	// Update ordered and written counts by the number of messages in the batch
	// This maintains compatibility with existing read path
	tinode_->offsets[broker].ordered = tinode_->offsets[broker].ordered + num_messages;
	tinode_->offsets[broker].written = tinode_->offsets[broker].written + num_messages;

	// Set up export chain (GOI equivalent)
	header_for_sub->batch_off_to_export = (reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(header_for_sub));
	header_for_sub->ordered = 1;

	header_for_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header_for_sub) + sizeof(BatchHeader));

	VLOG(3) << "Orderer5: Assigned batch-level order " << start_total_order 
			<< " to batch with " << num_messages << " messages from broker " << broker;
}

} // End of namespace Embarcadero
