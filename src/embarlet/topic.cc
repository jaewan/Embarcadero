#include "topic.h"
#include "../cxl_manager/scalog_local_sequencer.h"
#include "common/performance_utils.h"
#include "../common/wire_formats.h"

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
		
	// Start delegation thread if needed (Stage 2: Local Ordering)
	// Delegation Thread assigns local per-broker sequence numbers
	if (seq_type == CORFU || (seq_type != KAFKA && order_ != 4)) {
		delegationThreads_.emplace_back(&Topic::DelegationThread, this);
	}

		// Head node runs sequencer
		if(broker_id == 0){
			LOG(INFO) << "Topic constructor: broker_id=" << broker_id << ", order=" << order << ", seq_type=" << seq_type;
			switch(seq_type){
				case KAFKA: // Kafka is just a way to not run DelegationThread, not actual sequencer
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
	// LEGACY: Update TInode (backward compatibility)
	if (tinode_->replicate_tinode && replica_tinode_) {
		replica_tinode_->offsets[broker_id_].written = written;
		replica_tinode_->offsets[broker_id_].written_addr = written_addr;
	}

	// Update primary tinode
	tinode_->offsets[broker_id_].written = written;
	tinode_->offsets[broker_id_].written_addr = written_addr;
}

/**
 * DelegationThread: Stage 2 (Local Ordering)
 * 
 * Purpose: Assign local per-broker sequence numbers to messages after receiver writes them
 * 
 * Processing Pipeline:
 * 1. Poll received flag (set by Receiver in Stage 1)
 * 2. Assign local counter (per-broker sequence number)
 * 3. Update TInode.offset_entry.written_addr (monotonic pointer advance)
 * 4. Flush cache line containing delegation fields (bytes 16-31 only)
 * 
 * Threading: Single delegation thread per broker (no locks needed)
 * Ownership: Writes to BlogMessageHeader bytes 16-31 (delegation region)
 */
void Topic::DelegationThread() {
	// Validate first_message_addr_ before using it
	if (!first_message_addr_ || first_message_addr_ == cxl_addr_) {
		LOG(FATAL) << "Invalid first_message_addr_ in DelegationThread for topic: " << topic_name_
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
	
	// DelegationThread started for topic

	// NEW APPROACH: Use batch-based processing instead of message-by-message
	// Initialize batch header pointer to match EmbarcaderoGetCXLBuffer allocation
	BatchHeader* current_batch = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	
	// [[CRITICAL FIX: Removed Dead Code]] - first_batch variable was unused
	size_t processed_batches = 0;
	
	// [[PERFORMANCE FIX]]: Batch flush optimization (DEV-002)
	// Flush every 8 batches or every 64KB of data, whichever comes first
	// This reduces flush overhead from ~10M flushes/sec to ~1.25M flushes/sec
	constexpr size_t BATCH_FLUSH_INTERVAL = 8;  // Flush every 8 batches
	constexpr size_t BYTE_FLUSH_INTERVAL = 64 * 1024;  // Flush every 64KB
	size_t batches_since_flush = 0;
	size_t bytes_since_flush = 0;
	
		// DelegationThread: Starting batch processing

	while (!stop_threads_) {
		// NEW: Try batch-based processing first
		if (current_batch && __atomic_load_n(&current_batch->batch_complete, __ATOMIC_ACQUIRE)) {
			// DelegationThread: Found completed batch
			// Process this completed batch
			if (current_batch->num_msg > 0) {
				// [[BLOG_HEADER: Gate ORDER=5 per-message writes when BlogHeader is enabled]]
				bool skip_per_message_writes = (order_ == 5 && HeaderUtils::ShouldUseBlogHeader());

				// For BlogMessageHeader, receiver already set the required fields; delegation doesn't need to write
				// For MessageHeader, delegation needs to set logical_offset/segment_header/next_msg_diff

				if (!skip_per_message_writes) {
					// MessageHeader path: set required fields for each message
					MessageHeader* batch_first_msg = reinterpret_cast<MessageHeader*>(
						reinterpret_cast<uint8_t*>(cxl_addr_) + current_batch->log_idx);
					
					// Process all messages in this batch efficiently
					MessageHeader* msg_ptr = batch_first_msg;
					for (size_t i = 0; i < current_batch->num_msg; ++i) {
						// Set required fields for each message
						msg_ptr->logical_offset = logical_offset_;
						msg_ptr->segment_header = reinterpret_cast<uint8_t*>(msg_ptr) - CACHELINE_SIZE;
						
						// Read paddedSize first (needed for next message calculation)
						size_t current_padded_size = msg_ptr->paddedSize;
						
						// [[CRITICAL FIX: paddedSize Validation]] - Add bounds check to prevent out-of-bounds access
						// Matches validation in legacy message-by-message path (line 378-389)
						const size_t min_msg_size = sizeof(MessageHeader);
						const size_t max_msg_size = 1024 * 1024; // 1MB max message size
						if (current_padded_size < min_msg_size || current_padded_size > max_msg_size) {
							static thread_local size_t error_count = 0;
							if (++error_count % 1000 == 1) {
								LOG(ERROR) << "DelegationThread: Invalid paddedSize=" << current_padded_size 
								           << " for topic " << topic_name_ << ", broker " << broker_id_
								           << " (error #" << error_count << ")";
							}
							CXL::cpu_pause();
							break; // Exit message loop on corrupted data
						}
						
						msg_ptr->next_msg_diff = current_padded_size;

						// Update segment header
						*reinterpret_cast<unsigned long long int*>(msg_ptr->segment_header) =
							static_cast<unsigned long long int>(
								reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(msg_ptr->segment_header));

						// Move to next message in batch
						if (i < current_batch->num_msg - 1) {
							msg_ptr = reinterpret_cast<MessageHeader*>(
								reinterpret_cast<uint8_t*>(msg_ptr) + current_padded_size);
						}
						logical_offset_++;
					}

					// Update TInode and tracking with the last message in the batch
					UpdateTInodeWritten(
						logical_offset_ - 1, 
						static_cast<unsigned long long int>(
							reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_)));
				} else {
			// BlogMessageHeader path: receiver already set fields, just track offsets
				// For ORDER=5, we don't need per-message header writes; export uses BatchHeader.total_size
				// Compute end position for tracking
				BlogMessageHeader* batch_first_msg = reinterpret_cast<BlogMessageHeader*>(
					reinterpret_cast<uint8_t*>(cxl_addr_) + current_batch->log_idx);
				BlogMessageHeader* msg_ptr = batch_first_msg;
				
				for (size_t i = 0; i < current_batch->num_msg; ++i) {
					// For BlogMessageHeader, size is in payload bytes only
					// [[PAPER_SPEC: BlogMessageHeader]] - Validate size is within bounds
					if (msg_ptr->size > wire::MAX_MESSAGE_PAYLOAD_SIZE) {
						LOG(ERROR) << "DelegationThread: Message " << i << " in batch has excessive payload size=" << msg_ptr->size
							<< " (max=" << wire::MAX_MESSAGE_PAYLOAD_SIZE << "), aborting batch";
						break;
					}
					
					size_t header_size = sizeof(BlogMessageHeader);
					size_t padded_size = wire::ComputeMessageStride(header_size, msg_ptr->size);
					
					// Validate stride is reasonable
					if (padded_size < 64 || padded_size > wire::MAX_MESSAGE_PAYLOAD_SIZE + header_size + 64) {
						LOG(ERROR) << "DelegationThread: Message " << i << " computed invalid stride=" << padded_size
							<< " from payload_size=" << msg_ptr->size << ", aborting batch";
						break;
					}
					
					// Validate we don't walk past the batch end (sanity check)
					if (i > 0) {
						size_t batch_end_estimate = current_batch->log_idx + current_batch->total_size;
						size_t msg_end = static_cast<size_t>(reinterpret_cast<uint8_t*>(msg_ptr) + padded_size - reinterpret_cast<uint8_t*>(cxl_addr_));
						if (msg_end > batch_end_estimate) {
							static thread_local size_t stride_error_count = 0;
							if (++stride_error_count % 100 == 1) {
								LOG(WARNING) << "DelegationThread: Message " << i << " would walk past batch end"
									<< " (msg_end=" << msg_end << ", batch_end=" << batch_end_estimate << ")"
									<< ", error count=" << stride_error_count;
							}
						}
					}
					
					logical_offset_++;
					
					// Move to next message
					if (i < current_batch->num_msg - 1) {
						msg_ptr = reinterpret_cast<BlogMessageHeader*>(
							reinterpret_cast<uint8_t*>(msg_ptr) + padded_size);
					}
				}

					// Update TInode tracking
					UpdateTInodeWritten(
						logical_offset_ - 1, 
						static_cast<unsigned long long int>(
							reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_)));
				}

			// [[PERFORMANCE FIX]]: Batch flush optimization (DEV-002)
			// Flush every N batches or every 64KB, whichever comes first
			// This reduces flush overhead while maintaining CXL visibility
			batches_since_flush++;
			bytes_since_flush += current_batch->total_size;
			
			if (batches_since_flush >= BATCH_FLUSH_INTERVAL || bytes_since_flush >= BYTE_FLUSH_INTERVAL) {
				// Flush cache line after TInode update for CXL visibility
				// Flush & Poll principle: Writers must flush after writes to non-coherent CXL
				CXL::flush_cacheline(const_cast<const void*>(static_cast<volatile void*>(&tinode_->offsets[broker_id_])));
				CXL::store_fence();
				batches_since_flush = 0;
				bytes_since_flush = 0;
			}

			processed_batches++;
			VLOG(3) << "DelegationThread: Processed batch " << processed_batches 
			        << " with " << current_batch->num_msg << " messages";

			// [[CRITICAL FIX: Removed Prefetching]] - Batch headers are written by NetworkManager
			// Prefetching remote-writer data can cause stale cache reads in non-coherent CXL
			// See docs/INVESTIGATION_2026_01_26_CRITICAL_ISSUES.md Issue #1

			// Move to next batch
			BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
			current_batch = next_batch;
			continue; // Skip the old message-by-message processing
		}
		} else if (current_batch && current_batch->num_msg > 0) {
		// DelegationThread: Waiting for batch completion (reduced logging)
		}

		// FALLBACK: Old message-by-message processing for compatibility
		// Safe memory access with bounds checking
		try {
			// Validate header pointer before accessing
			if (reinterpret_cast<uintptr_t>(header) < reinterpret_cast<uintptr_t>(cxl_addr_) ||
			    reinterpret_cast<uintptr_t>(header) >= reinterpret_cast<uintptr_t>(cxl_addr_) + (1ULL << 36)) {
				LOG(ERROR) << "DelegationThread: Invalid header pointer " << header 
				           << " for topic " << topic_name_ << ", broker " << broker_id_;
				break;
			}
		} catch (...) {
			LOG(ERROR) << "DelegationThread: Exception accessing memory at " << header 
			           << " for topic " << topic_name_ << ", broker " << broker_id_;
			break;
		}

		
		// [[DEVIATION_004]] - Using TInode.offset_entry instead of Bmeta. Should rename TInode to Bmeta
		// Legacy path: Poll MessageHeader.paddedSize (current implementation)
		// TInode.offset_entry.log_offset tracks the start of the broker's log
		// TInode.offset_entry.written_addr tracks the last processed message
		
		// Wait for message to be complete before processing
		volatile size_t padded_size;
		do {
			if (stop_threads_) return;
		// Use memory barrier to ensure fresh read from memory
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		padded_size = header->paddedSize;
		if (padded_size == 0) {
			CXL::cpu_pause();
		}
		} while (padded_size == 0);
		
		// Additional validation: ensure paddedSize is reasonable
		const size_t min_msg_size = sizeof(MessageHeader);
		const size_t max_msg_size = 1024 * 1024; // 1MB max message size
		if (padded_size < min_msg_size || padded_size > max_msg_size) {
			static thread_local size_t error_count = 0;
			if (++error_count % 1000 == 1) {
				LOG(ERROR) << "DelegationThread: Invalid paddedSize=" << padded_size 
			           << " for topic " << topic_name_ << ", broker " << broker_id_
			           << " (error #" << error_count << ")";
		}
		CXL::cpu_pause();
		continue;
		}

		// Update message metadata
		header->segment_header = segment_header;
		header->logical_offset = logical_offset_;
		header->next_msg_diff = padded_size;

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
				LOG(WARNING) << "DelegationThread: Invalid next header pointer " << next_header 
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

	global_seq_.store(0, std::memory_order_relaxed);

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

	// [[DEVIATION_004]] - Stage 3 (Global Ordering)
	// Using TInode.offset_entry.written_addr instead of Bmeta.local.processed_ptr
	// This aligns with the processing pipeline in Paper §3.3
	// TInode.offset_entry.written_addr tracks the last processed message address
	uint64_t last_processed_addr = 0;

	while (!stop_threads_) {
		// 1. Poll TInode.offset_entry.written_addr for new messages
		// [[DEVIATION_004]] - Using offset_entry.written_addr instead of bmeta_.local.processed_ptr
		uint64_t current_processed_addr = __atomic_load_n(
			reinterpret_cast<volatile uint64_t*>(&tinode_->offsets[broker_id].written_addr), 
			__ATOMIC_ACQUIRE);

		if (current_processed_addr == last_processed_addr) {
			if (!ProcessSkipped(skipped_batches, header_for_sub)) {
				CXL::cpu_pause();
			}
			continue;
		}

		// 2. We have new messages from last_processed_addr to current_processed_addr
		// For now, we still use the BatchHeader-based logic for FIFO validation
		// but we detect new batches by looking at the BatchHeader ring
		
		// 1. Check for new Batch Header (Use memory_order_acquire for visibility)
		volatile size_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;

		// No new batch written in the BatchHeader ring yet, even if processed_ptr moved
		// This can happen if processed_ptr is updated before BatchHeader is fully visible
		if (num_msg_check == 0 || current_batch_header->log_idx == 0) {
			if(!ProcessSkipped(skipped_batches, header_for_sub)){
				CXL::cpu_pause();
			}
			continue;
		}

		// Update last_processed_addr after confirming BatchHeader is visible
		last_processed_addr = current_processed_addr;

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
					start_total_order = global_seq_.fetch_add(
						header_to_process->num_msg,
						std::memory_order_relaxed);
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
					start_total_order = global_seq_.fetch_add(
						header_to_process->num_msg,
						std::memory_order_relaxed);
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
					start_total_order = global_seq_.fetch_add(
						batch_header->num_msg,
						std::memory_order_relaxed);
					map_it->second = expected_seq + 1;
					batch_processed = true; // Mark to proceed outside lock
					processed_any = true; // Mark that we did *some* work
					VLOG(4) << "ProcessSkipped [B?]: Client " << client_id << ", processing skipped batch " << expected_seq << ", reserving seq [" << start_total_order << ", " << (start_total_order + batch_header->num_msg) << ")";
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

		// Note: DEV-002 (batched flushes) planned - could batch if multiple fields in same cache line
		CXL::flush_cacheline(msg_header);
		CXL::store_fence();

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

	// [[DEVIATION_004]] - Update TInode.offset_entry.ordered_offset (Stage 3: Global Ordering)
	// Paper §3.3 - Sequencer updates ordered_offset after assigning total_order
	// This signals to Stage 4 (Replication) that the batch is ordered
	// Using TInode.offset_entry.ordered_offset instead of Bmeta.seq.ordered_ptr
	size_t ordered_offset = static_cast<size_t>(
		reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(cxl_addr_));
	tinode_->offsets[broker].ordered_offset = ordered_offset;
	
	// [[DEV-005: Optimize Flush Frequency]]
	// CRITICAL FIX: Flush the SEQUENCER region cachelines (bytes 256-511)
	// offset_entry is alignas(256) with two 256B sub-structs:
	// - First 256B: broker region (written_addr, etc.)
	// - Second 256B: sequencer region (ordered, ordered_offset) <- WE NEED TO FLUSH THIS
	// Address of sequencer region is at broker region + 256 bytes
	// 
	// OPTIMIZATION: Combine flush before fence (removes per-batch overhead vs. paper design)
	const void* seq_region = const_cast<const void*>(static_cast<const volatile void*>(&tinode_->offsets[broker].ordered));
	CXL::flush_cacheline(seq_region);
	CXL::store_fence();
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

	global_seq_.store(0, std::memory_order_relaxed);

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
	
	// [[CRITICAL FIX: Ring Buffer Boundary]] - Calculate ring end to prevent out-of-bounds access
	// Each broker gets BATCHHEADERS_SIZE bytes per topic for batch headers
	BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(ring_start_default) + BATCHHEADERS_SIZE);
	
	BatchHeader* header_for_sub = ring_start_default;

	size_t total_batches_processed = 0;
	auto last_log_time = std::chrono::steady_clock::now();

	// [[CRITICAL FIX: Simplified Polling Logic]]
	// Match message_ordering.cc pattern: Simply check num_msg and advance if not ready
	// Don't use written_addr as polling signal - it causes complexity and bugs
	// The working implementation in message_ordering.cc doesn't use written_addr at all
	// See docs/CRITICAL_BUG_FOUND_2026_01_26.md
	
	while (!stop_threads_) {
		// Check current batch header (matches message_ordering.cc:600-617 pattern)
		// num_msg is uint32_t in BatchHeader, so read as volatile uint32_t for type safety
		// For non-coherent CXL: volatile prevents compiler caching; ACQUIRE doesn't help cache coherence
		volatile uint32_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
		// Read log_idx as volatile for consistency (written by remote broker via NetworkManager)
		volatile size_t log_idx_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->log_idx;
		
		// Max reasonable: 2MB batch / 64B min message = ~32k messages, use 100k as safety limit
		constexpr uint32_t MAX_REASONABLE_NUM_MSG = 100000;
		if (num_msg_check == 0 || log_idx_check == 0 || num_msg_check > MAX_REASONABLE_NUM_MSG) {
			// Current batch not ready or invalid - advance to next (matches message_ordering.cc pattern)
			// This is the key: always advance when not ready, don't wait for written_addr
			BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
			if (next_batch_header >= ring_end) {
				next_batch_header = ring_start_default;
			}
			current_batch_header = next_batch_header;
			CXL::cpu_pause();
			continue;
		}

		// Batch is ready - process it

		// Valid batch found - use the volatile values we already read
		VLOG(3) << "BrokerScannerWorker5 [B" << broker_id << "]: Found valid batch with " << num_msg_check << " messages, batch_seq=" << current_batch_header->batch_seq << ", client_id=" << current_batch_header->client_id;

		BatchHeader* header_to_process = current_batch_header;
		size_t client_id = current_batch_header->client_id;
		size_t batch_seq = current_batch_header->batch_seq;

		// SIMPLIFIED: Process all batches as they arrive (like order level 0)
		// No strict sequencing - just assign total_order and process immediately
		// Use lock-free atomic fetch_add for global_seq_
		size_t start_total_order = global_seq_.fetch_add(
			static_cast<size_t>(num_msg_check),
			std::memory_order_relaxed);

		VLOG(4) << "Scanner5 [B" << broker_id << "]: Processing batch from client " << client_id 
				<< ", batch_seq=" << batch_seq << ", total_order=[" << start_total_order 
				<< ", " << (start_total_order + static_cast<size_t>(num_msg_check)) << ")";

		AssignOrder5(header_to_process, start_total_order, header_for_sub);
		total_batches_processed++;

		// [[CRITICAL FIX: Removed Prefetching]] - Prefetching remote-writer data violates non-coherent CXL semantics
		// Batch headers are written by NetworkManager (remote broker) and read by Sequencer (head broker)
		// Prefetching can cache stale values, causing infinite polling loops
		// See docs/INVESTIGATION_2026_01_26_CRITICAL_ISSUES.md Issue #1

		// Periodic status logging
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
			LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: Processed " << total_batches_processed 
			          << " batches, current tinode.ordered=" << tinode_->offsets[broker_id].ordered;
			last_log_time = now;
		}

		// Advance to next batch header
		BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
		
		// [[CRITICAL FIX: Ring Buffer Boundary Check]] - Wrap around when reaching ring end
		// Prevents out-of-bounds access and reading invalid memory
		// See docs/INVESTIGATION_2026_01_26_CRITICAL_ISSUES.md Issue #2
		if (next_batch_header >= ring_end) {
			next_batch_header = ring_start_default;
		}
		
		current_batch_header = next_batch_header;
	}
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

	// [[BLOG_HEADER: Batch-level ordering only for ORDER=5]]
	// With BlogMessageHeader emission at publisher, messages already have proper header format.
	// Subscriber reconstructs per-message total_order logically using BatchMetadata.
	// NO per-message CXL writes needed - this eliminates the performance bottleneck.
	// 
	// RATIONALE:
	// - Publisher emits BlogMessageHeader with proper format directly
	// - ORDER=5 means all messages in batch get same range [start_total_order, start_total_order + num_msg)
	// - Subscriber uses wire::BatchMetadata to assign per-message total_order logically
	// - Eliminates per-message flush_blog_sequencer_region() that was causing ~50% slowdown
	//
	// DISABLED CODE (was causing performance regression):
	// if (HeaderUtils::ShouldUseBlogHeader() && batch_to_order->num_msg > 0) {
	//     for (size_t i = 0; i < num_messages; ++i) {
	//         msg_hdr->total_order = current_order;
	//         CXL::flush_blog_sequencer_region(msg_hdr);
	//     }
	// }

	// Update ordered count by the number of messages in the batch
	tinode_->offsets[broker].ordered = tinode_->offsets[broker].ordered + num_messages;

	// [[DEVIATION_004]] - Update TInode.offset_entry.ordered_offset (Stage 3: Global Ordering)
	// Paper §3.3 - Sequencer updates ordered_offset after assigning total_order
	// This signals to Stage 4 (Replication) that the batch is ordered
	// Using TInode.offset_entry.ordered_offset instead of Bmeta.seq.ordered_ptr
	size_t ordered_offset = static_cast<size_t>(
		reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(cxl_addr_));
	tinode_->offsets[broker].ordered_offset = ordered_offset;

	// [[DEV-005: Optimize Flush Frequency]]
	// CRITICAL FIX: Flush the SEQUENCER region cachelines (bytes 256-511)
	// offset_entry is alignas(256) with two 256B sub-structs:
	// - First 256B: broker region (written_addr, etc.)
	// - Second 256B: sequencer region (ordered, ordered_offset) <- WE NEED TO FLUSH THIS
	// Address of sequencer region is at broker region + 256 bytes
	// 
	// OPTIMIZATION: Combine two flushes before single fence
	// Both sequencer-region and BatchHeader flushes can precede the same fence
	// This reduces serialization overhead vs. flush-fence-flush-fence pattern
	const void* seq_region = const_cast<const void*>(static_cast<const volatile void*>(&tinode_->offsets[broker].ordered));
	CXL::flush_cacheline(seq_region);
	
	// BatchHeader flush for total_order visibility (same cacheline as batch_to_order->total_order)
	CXL::flush_cacheline(batch_to_order);
	
	// Single fence for both flushes - reduces fence overhead
	CXL::store_fence();

	// Set up export chain (GOI equivalent)
	header_for_sub->batch_off_to_export = (reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(header_for_sub));
	header_for_sub->ordered = 1;

	header_for_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header_for_sub) + sizeof(BatchHeader));

	VLOG(3) << "Orderer5: Assigned batch-level order " << start_total_order 
			<< " to batch with " << num_messages << " messages from broker " << broker;
}

} // End of namespace Embarcadero
