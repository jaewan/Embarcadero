#include "topic.h"
#include "../cxl_manager/scalog_local_sequencer.h"
#include "common/performance_utils.h"
#include "../common/wire_formats.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#ifdef __x86_64__
#include <immintrin.h>  // For _mm_lfence
#endif

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
	num_slots_(BATCHHEADERS_SIZE / sizeof(BatchHeader)),
	current_segment_(segment_metadata),
	// [[SEQUENCER_ONLY_HEAD_NODE]] Detect sequencer-only mode from nullptr segment
	is_sequencer_only_(segment_metadata == nullptr && broker_id == 0) {

		// Validate tinode pointer first
		if (!tinode_) {
			LOG(FATAL) << "TInode is null for topic: " << topic_name;
		}

		// [[SEQUENCER_ONLY_HEAD_NODE]] Skip offset validation for sequencer-only head node
		// Sequencer-only node has no segment/batch header ring allocated, offsets[0] is unused
		if (is_sequencer_only_) {
			LOG(INFO) << "[SEQUENCER_ONLY] Topic " << topic_name << " created in sequencer-only mode"
			          << " (broker_id=" << broker_id << ", no local data path)";
			// Initialize addresses to nullptr/0 since we have no local ring
			log_addr_.store(0);
			batch_headers_ = 0;
			first_message_addr_ = nullptr;
			first_batch_headers_addr_ = nullptr;
		} else {
			// Normal path: validate offsets before using them
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
			// [[CODE_REVIEW_FIX]] Initial cache = "all free" sentinel so watermark logic is correct before first refresh
			cached_pbr_consumed_through_.store(BATCHHEADERS_SIZE, std::memory_order_release);
			// [[LOCKFREE_PBR]] Consumed seq = 0 (all slots free; sequencer sentinel is BATCHHEADERS_SIZE bytes)
			cached_consumed_seq_.store(0, std::memory_order_release);
#if defined(__x86_64__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
			use_lock_free_pbr_ = true;  // [[CODE_REVIEW Issue #6]] Trust CMPXCHG16B on x86-64; some libs report false for 16B atomics
#else
			use_lock_free_pbr_ = pbr_state_.is_lock_free();
#endif
			if (num_slots_ == 0) {
				LOG(ERROR) << "PBR num_slots_ is 0 for topic " << topic_name_
					<< " (BATCHHEADERS_SIZE=" << BATCHHEADERS_SIZE
					<< ", sizeof(BatchHeader)=" << sizeof(BatchHeader) << "); PBR reservation will fail.";
			}
		}

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
	// [[ORDER_0_ACK_OPTIMIZATION]] When order=0 and recv_direct_to_cxl, receive thread updates
	// 'written' so ACKs work without DelegationThread; skip starting it.
	bool skip_delegation_for_order0 = (order_ == 0 && GetConfig().config().network.recv_direct_to_cxl.get());
	if (seq_type == CORFU || (seq_type != KAFKA && order_ != 4 && !skip_delegation_for_order0)) {
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
					else if (order == 2) {
						LOG(INFO) << "Creating Sequencer2 thread for order level 2 (total order)";
						if (replication_factor_ > 0) {
							goi_recovery_thread_ = std::thread(&Topic::GOIRecoveryThread, this);
							LOG(INFO) << "Started GOIRecoveryThread for topic " << topic_name_;
						}
						sequencerThread_ = std::thread(&Topic::Sequencer2, this);
					}
					else if (order == 3)
						LOG(ERROR) << "Sequencer 3 is not ported yet";
						//sequencerThread_ = std::thread(&Topic::Sequencer3, this);
					else if (order == 4){
						sequencerThread_ = std::thread(&Topic::Sequencer4, this);
					}
					else if (order == 5){
						LOG(INFO) << "Creating Sequencer5 thread for order level 5";
					// [[PHASE_3]] Start GOI recovery thread for monitoring chain replication
					if (replication_factor_ > 0) {
						goi_recovery_thread_ = std::thread(&Topic::GOIRecoveryThread, this);
						LOG(INFO) << "Started GOIRecoveryThread for topic " << topic_name_;
					}
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
	// [[SEQUENCER_ONLY_HEAD_NODE]] No local log on sequencer-only node - nothing to delegate
	if (is_sequencer_only_) {
		VLOG(1) << "DelegationThread: sequencer-only topic " << topic_name_ << ", no local delegation";
		return;
	}
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
		BatchHeader* &batch_header_location,
		bool /*epoch_already_checked*/) {

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
		BatchHeader* &batch_header_location,
		bool /*epoch_already_checked*/) {

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
		BatchHeader* &batch_header_location,
		bool /*epoch_already_checked*/) {

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

std::pair<uint64_t, bool> Topic::RefreshBrokerEpochFromCXL(bool force_full_read) {
	if (!force_full_read) {
		return {broker_epoch_.load(std::memory_order_relaxed), false};
	}
	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::flush_cacheline(control_block);
	CXL::load_fence();
	uint64_t current_epoch = control_block->epoch.load(std::memory_order_acquire);
	uint64_t prev_epoch = broker_epoch_.load(std::memory_order_acquire);
	bool was_stale = (current_epoch > prev_epoch);
	if (was_stale) {
		broker_epoch_.store(current_epoch, std::memory_order_release);
	}
	return {current_epoch, was_stale};
}

std::function<void(void*, size_t)> Topic::Order4GetCXLBuffer(
		BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE],
		void* &log,
		void* &segment_header,
		size_t &logical_offset,
		BatchHeader* &batch_header_location,
		bool /*epoch_already_checked*/) {

	// [[PHASE_1A_EPOCH_FENCING]] Periodic epoch check (§4.2.1 "e.g. every 100 batches"); refuse one batch when stale
	uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
	bool force_full_read = (n % kEpochCheckInterval == 0);
	auto [current_epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
	if (was_stale) {
		log = nullptr;
		batch_header_location = nullptr;
		return nullptr;
	}

	// Calculate base addresses
	const unsigned long long int segment_metadata = 
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;
	void* batch_headers_log;

	{
		absl::MutexLock lock(&mutex_);

		// Allocate space in log
		log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));

		// Allocate space for batch header (wrap within ring)
		batch_headers_log = reinterpret_cast<void*>(batch_headers_);
		batch_headers_ += sizeof(BatchHeader);
		const unsigned long long int batch_headers_start =
			reinterpret_cast<unsigned long long int>(first_batch_headers_addr_);
		const unsigned long long int batch_headers_end = batch_headers_start + BATCHHEADERS_SIZE;
		if (batch_headers_ >= batch_headers_end) {
			batch_headers_ = batch_headers_start;
		}
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
	batch_header.epoch_created = static_cast<uint16_t>(std::min(current_epoch, static_cast<uint64_t>(0xFFFF)));

	// [[PHASE_2]] Generate globally unique batch_id for GOI tracking
	// Format: (broker_id << 48) | (timestamp << 16) | counter
	static thread_local uint16_t batch_counter = 0;
	uint64_t timestamp = CXL::rdtsc() >> 16;  // Coarse timestamp (32 bits after shift)
	batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) |
	                        ((timestamp & 0xFFFFFFFF) << 16) |
	                        (batch_counter++);

	// [[PHASE_2_FIX]] Generate absolute PBR index (monotonic, never wraps) for CV tracking
	batch_header.pbr_absolute_index = broker_pbr_counters_[broker_id_].fetch_add(1, std::memory_order_relaxed);

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
        BatchHeader* &batch_header_location,
        bool /*epoch_already_checked*/) {
    
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
		BatchHeader* &batch_header_location,
		bool epoch_already_checked) {

	// [[SEQUENCER_ONLY_HEAD_NODE]] Defense in depth: fail fast if called on sequencer-only Topic
	// Sequencer-only node has no segment/batch header ring, offsets[0] is uninitialized.
	// NetworkManager is not started so this shouldn't be called, but guard against future misuse.
	if (is_sequencer_only_) {
		LOG(ERROR) << "[SEQUENCER_ONLY] EmbarcaderoGetCXLBuffer called on sequencer-only Topic "
		           << topic_name_ << " - this should not happen";
		log = nullptr;
		batch_header_location = nullptr;
		return nullptr;
	}

	// [[Issue #3]] When caller did CheckEpochOnce at batch start, skip duplicate epoch check.
	if (!epoch_already_checked) {
		uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
		bool force_full_read = (n % kEpochCheckInterval == 0);
		auto [current_epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
		if (was_stale) {
			log = nullptr;
			batch_header_location = nullptr;
			return nullptr;
		}
	}

	// Calculate base addresses
	const unsigned long long int segment_metadata =
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;

	// [[PERF FIX]] Ring gating check moved OUTSIDE mutex for ORDER=0
	// ORDER=0 doesn't use sequencer, so ring gating isn't strictly needed.
	// For other orders, we still check but do the expensive CXL read outside the lock.
	bool skip_ring_gating = (order_ == 0);
	bool slot_free = true;

	if (!skip_ring_gating) {
		// Read batch_headers_consumed_through OUTSIDE mutex (stale read is safe - worst case is
		// thinking ring is fuller than it is, which just causes retry)
		const void* consumed_through_addr = const_cast<const void*>(
			reinterpret_cast<const volatile void*>(&tinode_->offsets[broker_id_].batch_headers_consumed_through));
		CXL::flush_cacheline(consumed_through_addr);
		CXL::load_fence();
		size_t consumed_through = tinode_->offsets[broker_id_].batch_headers_consumed_through;

		// Approximate check - next_slot_offset read without lock is approximate but safe
		size_t approx_next_slot = cached_next_slot_offset_.load(std::memory_order_acquire);

		if (consumed_through != BATCHHEADERS_SIZE) {
			size_t in_flight;
			if (approx_next_slot >= consumed_through) {
				in_flight = approx_next_slot - consumed_through;
			} else {
				in_flight = (BATCHHEADERS_SIZE - consumed_through) + approx_next_slot;
			}
			// Add extra margin (2 slots) for approximation safety
			slot_free = (in_flight + sizeof(BatchHeader) * 2 < BATCHHEADERS_SIZE);
		}

		if (!slot_free) {
			log = nullptr;
			batch_header_location = nullptr;
			uint64_t count = ring_full_count_.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count <= 10 || count % 10000 == 0) {
				LOG(WARNING) << "EmbarcaderoGetCXLBuffer: Ring full (approx check) broker=" << broker_id_
				             << " topic=" << topic_name_ << " count=" << count;
			}
			return nullptr;
		}
	}

	{
		// Minimal critical section: just allocation
		absl::MutexLock lock(&mutex_);

		// Allocate space in log
		log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
	}

	// Check for segment boundary
	CheckSegmentBoundary(log, msg_size, segment_metadata);

	// [[DESIGN: PBR reserve after receive]] Do NOT generate metadata or write the BatchHeader here.
	// NetworkManager reserves a PBR slot after recv(payload) and generates metadata once.
	// For non-EMBARCADERO sequencers: ReservePBRSlotAndWriteEntry generates metadata.
	// Leave batch_header unchanged; caller will fill it.
	batch_header_location = nullptr;

	return nullptr;
}

// [[Issue #3]] Single epoch check per batch; call once at batch start, pass epoch_already_checked to ReserveBLogSpace/ReservePBRSlotAndWriteEntry.
bool Topic::CheckEpochOnce() {
	uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
	bool force_full_read = (n % kEpochCheckInterval == 0);
	auto [current_epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
	last_checked_epoch_.store(current_epoch, std::memory_order_release);
	return was_stale;
}

// [[RECV_DIRECT_TO_CXL]] Lock-free BLog allocation (~10ns). Returns nullptr if sequencer-only.
// [[PHASE_1A_EPOCH_FENCING]] Design §4.2.1: refuse writes if broker epoch is stale (zombie broker).
// [[Issue #3]] When epoch_already_checked true, skip epoch check (caller did CheckEpochOnce at batch start).
void* Topic::ReserveBLogSpace(size_t size, bool epoch_already_checked) {
	if (is_sequencer_only_) return nullptr;

	if (!epoch_already_checked) {
		uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
		bool force_full_read = (n % kEpochCheckInterval == 0);
		auto [current_epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
		if (was_stale) return nullptr;
	}

	void* log = reinterpret_cast<void*>(log_addr_.fetch_add(size));
	const unsigned long long int segment_metadata = reinterpret_cast<unsigned long long int>(current_segment_);
	CheckSegmentBoundary(log, size, segment_metadata);
	return log;
}

void Topic::RefreshPBRConsumedThroughCache() {
	const void* consumed_through_addr = const_cast<const void*>(
		reinterpret_cast<const volatile void*>(&tinode_->offsets[broker_id_].batch_headers_consumed_through));
	CXL::flush_cacheline(consumed_through_addr);
	CXL::load_fence();
	size_t consumed = tinode_->offsets[broker_id_].batch_headers_consumed_through;
	cached_pbr_consumed_through_.store(consumed, std::memory_order_release);
	// [[LOCKFREE_PBR]] Drive cached_consumed_seq_ for lock-free path (sentinel BATCHHEADERS_SIZE → seq 0)
	if (consumed == BATCHHEADERS_SIZE) {
		cached_consumed_seq_.store(0, std::memory_order_release);
	} else {
		cached_consumed_seq_.store(static_cast<uint64_t>(consumed / sizeof(BatchHeader)), std::memory_order_release);
	}
}

int Topic::GetPBRUtilizationPct() {
	if (is_sequencer_only_) return -1;
	if (!first_batch_headers_addr_) return -1;
	uint64_t n = pbr_cache_refresh_counter_.fetch_add(1, std::memory_order_relaxed);
	if (n % kPBRCacheRefreshInterval == 0) {
		RefreshPBRConsumedThroughCache();
	}
	// [[LOCKFREE_PBR]] When lock-free path is active, next_slot is in pbr_state_; cached_next_slot_offset_ is not updated.
	if (use_lock_free_pbr_) {
		uint64_t consumed_seq = cached_consumed_seq_.load(std::memory_order_acquire);
		uint64_t next_slot_seq = pbr_state_.load(std::memory_order_acquire).next_slot_seq;
		uint64_t in_flight_slots = (next_slot_seq >= consumed_seq)
			? (next_slot_seq - consumed_seq) : 0;
		if (num_slots_ == 0) return 0;
		return static_cast<int>((in_flight_slots * 100) / num_slots_);
	}
	size_t consumed = cached_pbr_consumed_through_.load(std::memory_order_acquire);
	size_t next_slot_offset = cached_next_slot_offset_.load(std::memory_order_acquire);
	size_t in_flight;
	// [[FIX PBR_WATERMARK_ORDER0]] Sentinel BATCHHEADERS_SIZE = "all slots free" (same as EmbarcaderoGetCXLBuffer).
	// For ORDER=0 no sequencer runs, so consumed_through stays at sentinel; must not treat next_slot_offset as in_flight.
	if (consumed == BATCHHEADERS_SIZE) {
		in_flight = 0;  // Sentinel = all free, no backpressure
	} else {
		if (next_slot_offset >= consumed) {
			in_flight = next_slot_offset - consumed;
		} else {
			in_flight = (BATCHHEADERS_SIZE - consumed) + next_slot_offset;
		}
	}
	return static_cast<int>((in_flight * 100) / BATCHHEADERS_SIZE);
}

bool Topic::IsPBRAboveHighWatermark(int high_pct) {
	if (high_pct <= 0 || high_pct > 100) return false;
	int util = GetPBRUtilizationPct();
	return (util >= 0 && util >= high_pct);
}

bool Topic::IsPBRBelowLowWatermark(int low_pct) {
	if (low_pct < 0 || low_pct > 100) return true;
	int util = GetPBRUtilizationPct();
	return (util < 0 || util <= low_pct);
}

bool Topic::ReservePBRSlotLockFree(uint32_t num_msg, size_t& out_byte_offset, size_t& out_logical_offset) {
	if (num_slots_ == 0) return false;  // Config/size mismatch; avoid % 0 below
	uint64_t consumed_seq = cached_consumed_seq_.load(std::memory_order_acquire);
	PBRProducerState current = pbr_state_.load(std::memory_order_acquire);
	PBRProducerState next;
	do {
		uint64_t in_flight = (current.next_slot_seq >= consumed_seq)
			? (current.next_slot_seq - consumed_seq) : 0;
		if (in_flight >= num_slots_ - 1)
			return false;
		next.next_slot_seq = current.next_slot_seq + 1;
		next.logical_offset = current.logical_offset + num_msg;
	} while (!pbr_state_.compare_exchange_weak(
		current, next,
		std::memory_order_acq_rel,
		std::memory_order_acquire));
	size_t slot_index = static_cast<size_t>(current.next_slot_seq % num_slots_);
	out_byte_offset = slot_index * sizeof(BatchHeader);
	out_logical_offset = static_cast<size_t>(current.logical_offset);
	return true;
}

bool Topic::ReservePBRSlotAfterRecv(BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	if (is_sequencer_only_) return false;
	if (!first_batch_headers_addr_) return false;

	uint64_t current_epoch;
	if (!epoch_already_checked) {
		uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
		bool force_full_read = (n % kEpochCheckInterval == 0);
		auto [epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
		if (was_stale) return false;
		current_epoch = epoch;
	} else {
		current_epoch = last_checked_epoch_.load(std::memory_order_acquire);
	}

	const unsigned long long int segment_metadata = reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;
	void* batch_headers_log;

	if (use_lock_free_pbr_) {
		// [[LOCKFREE_PBR]] 128-bit CAS path; no CXL read here (cached_consumed_seq_ refreshed by GetPBRUtilizationPct).
		size_t byte_offset;
		if (!ReservePBRSlotLockFree(batch_header.num_msg, byte_offset, logical_offset)) {
			batch_header_location = nullptr;
			return false;
		}
		batch_headers_log = reinterpret_cast<void*>(
			reinterpret_cast<uintptr_t>(first_batch_headers_addr_) + byte_offset);
	} else {
		// [[CODE_REVIEW Issue #3]] Mutex fallback: refresh consumed_through only every kPBRCacheRefreshInterval batches (not every batch).
		uint64_t n = pbr_cache_refresh_counter_.fetch_add(1, std::memory_order_relaxed);
		if (n % kPBRCacheRefreshInterval == 0) {
			RefreshPBRConsumedThroughCache();
		}
		const unsigned long long int batch_headers_start =
			reinterpret_cast<unsigned long long int>(first_batch_headers_addr_);
		const unsigned long long int batch_headers_end = batch_headers_start + BATCHHEADERS_SIZE;
		{
			absl::MutexLock lock(&mutex_);
			size_t next_slot_offset = static_cast<size_t>(batch_headers_ - batch_headers_start);
			size_t consumed_through = cached_pbr_consumed_through_.load(std::memory_order_acquire);

			bool slot_free;
			if (consumed_through == BATCHHEADERS_SIZE) {
				slot_free = true;
			} else {
				size_t in_flight;
				if (next_slot_offset >= consumed_through) {
					in_flight = next_slot_offset - consumed_through;
				} else {
					in_flight = (BATCHHEADERS_SIZE - consumed_through) + next_slot_offset;  // Wrap
				}
				slot_free = (in_flight + sizeof(BatchHeader) < BATCHHEADERS_SIZE);
			}
			if (!slot_free) {
				batch_header_location = nullptr;
				return false;
			}

			batch_headers_log = reinterpret_cast<void*>(batch_headers_);
			batch_headers_ += sizeof(BatchHeader);
			if (batch_headers_ >= batch_headers_end) {
				batch_headers_ = batch_headers_start;
			}
			cached_next_slot_offset_.store(static_cast<size_t>(batch_headers_ - batch_headers_start), std::memory_order_release);
			logical_offset = logical_offset_;
			logical_offset_ += batch_header.num_msg;
		}
	}

	CheckSegmentBoundary(log, msg_size, segment_metadata);
	segment_header = current_segment_;

	batch_header.start_logical_offset = logical_offset;
	batch_header.broker_id = broker_id_;
	batch_header.ordered = 0;
	batch_header.total_order = 0;
	static thread_local uint16_t batch_counter_tls = 0;  // [[CODE_REVIEW Issue #8]] Used in both lock-free and mutex paths
	uint64_t timestamp = CXL::rdtsc() >> 16;
	batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) |
		((timestamp & 0xFFFFFFFF) << 16) | (batch_counter_tls++);
	batch_header.pbr_absolute_index = broker_pbr_counters_[broker_id_].fetch_add(1, std::memory_order_relaxed);
	batch_header.epoch_created = static_cast<uint16_t>(std::min(current_epoch, static_cast<uint64_t>(0xFFFF)));
	batch_header.log_idx = static_cast<size_t>(
		reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_));
	batch_header.batch_complete = 0;  // Readiness is num_msg>0; batch_complete reserved for async paths.

	// Prevent sequencer from observing stale data before we publish the full header.
	// [[CRITICAL]] Must flush the zeroed slot so sequencer doesn't see old batch metadata.
	memset(batch_headers_log, 0, sizeof(BatchHeader));
	CXL::flush_cacheline(batch_headers_log);
	const void* batch_header_next_line = reinterpret_cast<const void*>(
		reinterpret_cast<const uint8_t*>(batch_headers_log) + 64);
	CXL::flush_cacheline(batch_header_next_line);
	CXL::store_fence();
	batch_header_location = reinterpret_cast<BatchHeader*>(batch_headers_log);
	return true;
}

bool Topic::ReservePBRSlotAndWriteEntry(BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	if (is_sequencer_only_) return false;
	if (!first_batch_headers_addr_) return false;

	uint64_t current_epoch;
	if (!epoch_already_checked) {
		uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
		bool force_full_read = (n % kEpochCheckInterval == 0);
		auto [epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
		if (was_stale) return false;
		current_epoch = epoch;
	} else {
		current_epoch = last_checked_epoch_.load(std::memory_order_acquire);
	}

	const unsigned long long int segment_metadata = reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;
	void* batch_headers_log;

	if (use_lock_free_pbr_) {
		// [[LOCKFREE_PBR]] 128-bit CAS path; no CXL read here (cached_consumed_seq_ refreshed by GetPBRUtilizationPct).
		// [[CODE_REVIEW Issue #4]] Lock-free path uses monotonic slot sequences (no wrap); in_flight = next_slot_seq - consumed_seq.
		size_t byte_offset;
		if (!ReservePBRSlotLockFree(batch_header.num_msg, byte_offset, logical_offset)) {
			batch_header_location = nullptr;
			return false;
		}
		batch_headers_log = reinterpret_cast<void*>(
			reinterpret_cast<uintptr_t>(first_batch_headers_addr_) + byte_offset);
	} else {
		// [[CODE_REVIEW Issue #3]] Mutex fallback: refresh consumed_through only every kPBRCacheRefreshInterval batches (not every batch).
		uint64_t n = pbr_cache_refresh_counter_.fetch_add(1, std::memory_order_relaxed);
		if (n % kPBRCacheRefreshInterval == 0) {
			RefreshPBRConsumedThroughCache();
		}
		const unsigned long long int batch_headers_start =
			reinterpret_cast<unsigned long long int>(first_batch_headers_addr_);
		const unsigned long long int batch_headers_end = batch_headers_start + BATCHHEADERS_SIZE;
		{
			absl::MutexLock lock(&mutex_);
			size_t next_slot_offset = static_cast<size_t>(batch_headers_ - batch_headers_start);
			size_t consumed_through = cached_pbr_consumed_through_.load(std::memory_order_acquire);

			// [[CODE_REVIEW Issue #4]] Mutex path uses byte offsets that wrap; in_flight = bytes between consumed and next write.
			bool slot_free;
			if (consumed_through == BATCHHEADERS_SIZE) {
				slot_free = true;
			} else {
				size_t in_flight;
				if (next_slot_offset >= consumed_through) {
					in_flight = next_slot_offset - consumed_through;
				} else {
					in_flight = (BATCHHEADERS_SIZE - consumed_through) + next_slot_offset;  // Wrap
				}
				slot_free = (in_flight + sizeof(BatchHeader) < BATCHHEADERS_SIZE);
			}
			if (!slot_free) {
				batch_header_location = nullptr;
				return false;
			}

			batch_headers_log = reinterpret_cast<void*>(batch_headers_);
			batch_headers_ += sizeof(BatchHeader);
			if (batch_headers_ >= batch_headers_end) {
				batch_headers_ = batch_headers_start;
			}
			cached_next_slot_offset_.store(static_cast<size_t>(batch_headers_ - batch_headers_start), std::memory_order_release);
			logical_offset = logical_offset_;
			logical_offset_ += batch_header.num_msg;
		}
	}

	CheckSegmentBoundary(log, msg_size, segment_metadata);
	segment_header = current_segment_;

	// [[FIX: Metadata generation unified]] Generate metadata here for legacy paths (non-EMBARCADERO).
	// For EMBARCADERO path: this ReservePBRSlotAndWriteEntry is not used (only ReservePBRSlotAfterRecv is used).
	// Keep metadata generation here for Order 4 and other sequencers (KAFKA, CORFU, SCALOG).
	batch_header.start_logical_offset = logical_offset;
	batch_header.broker_id = broker_id_;
	batch_header.ordered = 0;
	batch_header.total_order = 0;
	static thread_local uint16_t batch_counter_tls = 0;
	uint64_t timestamp = CXL::rdtsc() >> 16;
	batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) |
		((timestamp & 0xFFFFFFFF) << 16) | (batch_counter_tls++);
	batch_header.pbr_absolute_index = broker_pbr_counters_[broker_id_].fetch_add(1, std::memory_order_relaxed);
	batch_header.epoch_created = static_cast<uint16_t>(std::min(current_epoch, static_cast<uint64_t>(0xFFFF)));
	batch_header.log_idx = static_cast<size_t>(
		reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_));

	// [[Issue #4]] Write batch header but do NOT flush here; sequencer must not see it until batch_complete=1.
	// CompleteBatchInCXL flushes once when setting batch_complete (avoids 4 flushes + 2 fences per batch).
	memcpy(batch_headers_log, &batch_header, sizeof(BatchHeader));
	reinterpret_cast<BatchHeader*>(batch_headers_log)->batch_complete = 0;
	batch_header_location = reinterpret_cast<BatchHeader*>(batch_headers_log);
	return true;
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
	BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
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
	BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
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
		if(ack_level_ == 2 && replication_factor_ > 0){
			// [[PHASE_2]] ACK Level 2: Wait for full replication before exposing messages to subscribers
			// Use CompletionVector (8 bytes) instead of replication_done array (256 bytes)

			if (seq_type_ == heartbeat_system::EMBARCADERO) {
				// [[PHASE_2_CV_PATH]] Use shared helper to read CV and get replicated position
				size_t replicated_last_offset;
				if (!GetReplicatedLastOffsetFromCV(cxl_addr_, tinode_, broker_id_, replicated_last_offset)) {
					return false;  // No replication progress yet
				}

				// Adjust combined_offset to not exceed replicated position
				if (combined_offset > replicated_last_offset) {
					// Back up to replicated position
					combined_addr = reinterpret_cast<uint8_t*>(combined_addr) -
						(reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize * (combined_offset - replicated_last_offset));
					combined_offset = replicated_last_offset;
				}
			} else {
				// [[PHASE_1]] Legacy path: Use replication_done array for non-EMBARCADERO sequencers
				// [[FIX]] Use GetReplicationSetBroker to align with MessageExport
				int num_brokers = get_num_brokers_callback_();
				size_t r[replication_factor_];
				size_t min = (size_t)-1;
				for (int i = 0; i < replication_factor_; i++) {
					int b = Embarcadero::GetReplicationSetBroker(broker_id_, replication_factor_, num_brokers, i);
					volatile uint64_t* rep_done_ptr = &tinode_->offsets[b].replication_done[broker_id_];
					CXL::flush_cacheline(const_cast<const void*>(
						reinterpret_cast<const volatile void*>(rep_done_ptr)));
					r[i] = *rep_done_ptr;
					if (min > r[i]) {
						min = r[i];
					}
				}
				CXL::load_fence();

				if(min == (size_t)-1){
					return false;
				}
				if(combined_offset != min){
					combined_addr = reinterpret_cast<uint8_t*>(combined_addr) -
						(reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize * (combined_offset-min));
					combined_offset = min;
				}
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
	size_t full_size = reinterpret_cast<uint8_t*>(combined_addr) -
		reinterpret_cast<uint8_t*>(start_msg_header) +
		reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize;

	// Order 0: cap export at 2MB per call to reduce mutex/send overhead (batch message export)
	constexpr size_t kMaxExportBatchBytes = 2UL << 20;
	if (order_ == 0 && full_size > kMaxExportBatchBytes) {
		MessageHeader* cur = start_msg_header;
		size_t accumulated = 0;
		MessageHeader* stop_at = nullptr;
		while (cur != combined_addr) {
			size_t step = cur->paddedSize;
			if (step == 0) break;
			if (accumulated + step > kMaxExportBatchBytes) break;
			accumulated += step;
			stop_at = cur;
			cur = reinterpret_cast<MessageHeader*>(reinterpret_cast<uint8_t*>(cur) + step);
		}
		if (stop_at != nullptr) {
			last_offset = stop_at->logical_offset;
			last_addr = stop_at;
			messages_size = reinterpret_cast<uint8_t*>(stop_at) -
				reinterpret_cast<uint8_t*>(start_msg_header) + stop_at->paddedSize;
		} else {
			messages_size = full_size;
			last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
			last_addr = combined_addr;
		}
	} else {
		messages_size = full_size;
		last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
		last_addr = combined_addr;
	}
#endif

	return true;
}
// Sequencer 5: Batch-level sequencer (Phase 1b: epoch pipeline + Level 5 hold buffer)
void Topic::Sequencer5() {
	LOG(INFO) << "Starting Sequencer5 (Phase 1b epoch pipeline) for topic: " << topic_name_;
	absl::btree_set<int> registered_brokers;
	GetRegisteredBrokerSet(registered_brokers);

	// [[FIX: B3=0 ACKs]] Log which brokers are in the scanner set at startup
	// This is CRITICAL for diagnosing missing broker issues - scanners are only
	// created for brokers registered at this moment. Late-registering brokers
	// will NOT be scanned, causing 0 ACKs for those brokers!
	{
		std::string broker_list;
		for (int b : registered_brokers) {
			if (!broker_list.empty()) broker_list += ", ";
			broker_list += "B" + std::to_string(b);
		}
		LOG(INFO) << "Sequencer5: registered_brokers at startup = [" << broker_list << "] "
		         << "(count=" << registered_brokers.size() << "). "
		         << "WARNING: Brokers not in this list will NOT be scanned!";
	}

	// [[SEQUENCER_ONLY_HEAD_NODE]] Filter out broker 0 when sequencer-only
	// Sequencer-only head node has no B0 ring allocated, so don't spawn scanner for B0
	if (is_sequencer_only_) {
		registered_brokers.erase(0);
		LOG(INFO) << "[SEQUENCER_ONLY] Sequencer5: filtered out B0 from scanner set, scanning brokers: "
		          << [&]() {
		              std::string s;
		              for (int b : registered_brokers) s += std::to_string(b) + " ";
		              return s;
		          }();
	}

	// [[PHASE_1A_EPOCH_FENCING]] Epoch increment on sequencer start (§4.2)
	// New sequencer writes epoch+1 to ControlBlock so zombies see new epoch and replicas reject stale entries
	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::flush_cacheline(control_block);
	CXL::load_fence();
	uint64_t prev_epoch = control_block->epoch.load(std::memory_order_acquire);
	uint64_t new_epoch = prev_epoch + 1;
	control_block->epoch.store(new_epoch, std::memory_order_release);
	CXL::flush_cacheline(control_block);
	CXL::store_fence();
	LOG(INFO) << "Sequencer5: ControlBlock.epoch advanced " << prev_epoch << " -> " << new_epoch << " (zombie fencing)";

	global_seq_.store(0, std::memory_order_relaxed);
	epoch_index_.store(0, std::memory_order_relaxed);
	last_sequenced_epoch_.store(0, std::memory_order_relaxed);
	current_epoch_for_hold_ = 0;
	{
		absl::MutexLock lock(&per_scanner_buffers_mu_);
		for (int i = 0; i < 3; i++) {
			for (auto& kv : per_scanner_buffers_[i]) {
				kv.second.clear();
			}
			per_scanner_buffers_[i].clear();
		}
	}

	// [[PHASE_1B]] Init per-broker header_for_sub for export chain
	{
		absl::MutexLock lock(&phase1b_header_for_sub_mu_);
		phase1b_header_for_sub_.clear();
		for (int broker_id : registered_brokers) {
			BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
			phase1b_header_for_sub_[broker_id] = ring_start;
		}
	}

	epoch_driver_thread_ = std::thread(&Topic::EpochDriverThread, this);
	std::thread epoch_sequencer_thread(&Topic::EpochSequencerThread, this);

	// [[FIX: B3=0 ACKs]] Use dynamic scanner management instead of fixed threads
	// This allows late-registering brokers to be scanned
	{
		absl::MutexLock lock(&scanner_management_mu_);
		for (int broker_id : registered_brokers) {
			brokers_with_scanners_.insert(broker_id);
			scanner_threads_.emplace_back(&Topic::BrokerScannerWorker5, this, broker_id);
			LOG(INFO) << "Sequencer5: Started initial BrokerScannerWorker5 for broker " << broker_id;
		}
	}

	epoch_sequencer_thread.join();

	// Join all scanner threads (including any dynamically added ones)
	{
		absl::MutexLock lock(&scanner_management_mu_);
		for (std::thread& t : scanner_threads_) {
			if (t.joinable()) t.join();
		}
	}
	if (epoch_driver_thread_.joinable()) {
		epoch_driver_thread_.join();
	}
}

// Order 2: Total order (no per-client ordering). Same epoch pipeline as Sequencer5, but no Level 5
// partition or hold buffer; all batches go directly to ready in arrival order. Design §2.3 Order 2.
void Topic::Sequencer2() {
	LOG(INFO) << "Starting Sequencer2 (Order 2 total order, no per-client state) for topic: " << topic_name_;
	absl::btree_set<int> registered_brokers;
	GetRegisteredBrokerSet(registered_brokers);

	{
		std::string broker_list;
		for (int b : registered_brokers) {
			if (!broker_list.empty()) broker_list += ", ";
			broker_list += "B" + std::to_string(b);
		}
		LOG(INFO) << "Sequencer2: registered_brokers at startup = [" << broker_list << "] (count=" << registered_brokers.size() << ")";
	}

	if (is_sequencer_only_) {
		registered_brokers.erase(0);
		LOG(INFO) << "[SEQUENCER_ONLY] Sequencer2: filtered out B0 from scanner set";
	}

	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::flush_cacheline(control_block);
	CXL::load_fence();
	uint64_t prev_epoch = control_block->epoch.load(std::memory_order_acquire);
	uint64_t new_epoch = prev_epoch + 1;
	control_block->epoch.store(new_epoch, std::memory_order_release);
	CXL::flush_cacheline(control_block);
	CXL::store_fence();
	LOG(INFO) << "Sequencer2: ControlBlock.epoch advanced " << prev_epoch << " -> " << new_epoch << " (zombie fencing)";

	global_seq_.store(0, std::memory_order_relaxed);
	global_batch_seq_.store(0, std::memory_order_relaxed);
	epoch_index_.store(0, std::memory_order_relaxed);
	last_sequenced_epoch_.store(0, std::memory_order_relaxed);
	{
		absl::MutexLock lock(&per_scanner_buffers_mu_);
		for (int i = 0; i < 3; i++) {
			for (auto& kv : per_scanner_buffers_[i]) {
				kv.second.clear();
			}
			per_scanner_buffers_[i].clear();
		}
	}

	{
		absl::MutexLock lock(&phase1b_header_for_sub_mu_);
		phase1b_header_for_sub_.clear();
		for (int broker_id : registered_brokers) {
			BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
			phase1b_header_for_sub_[broker_id] = ring_start;
		}
	}

	epoch_driver_thread_ = std::thread(&Topic::EpochDriverThread, this);
	std::thread epoch_sequencer_thread(&Topic::EpochSequencerThread2, this);

	{
		absl::MutexLock lock(&scanner_management_mu_);
		for (int broker_id : registered_brokers) {
			brokers_with_scanners_.insert(broker_id);
			scanner_threads_.emplace_back(&Topic::BrokerScannerWorker5, this, broker_id);
			LOG(INFO) << "Sequencer2: Started BrokerScannerWorker5 for broker " << broker_id;
		}
	}

	epoch_sequencer_thread.join();

	{
		absl::MutexLock lock(&scanner_management_mu_);
		for (std::thread& t : scanner_threads_) {
			if (t.joinable()) t.join();
		}
	}
	if (epoch_driver_thread_.joinable()) {
		epoch_driver_thread_.join();
	}
}

void Topic::EpochSequencerThread2() {
	LOG(INFO) << "EpochSequencerThread2 started for topic: " << topic_name_;
	static thread_local auto last_diag_time = std::chrono::steady_clock::now();
	while (!stop_threads_) {
		uint64_t last = last_sequenced_epoch_.load(std::memory_order_acquire);
		uint64_t current = epoch_index_.load(std::memory_order_acquire);
		if (last >= current) {
			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::seconds>(now - last_diag_time).count() >= 5) {
				size_t epoch_total = 0;
				{
					absl::MutexLock lock(&per_scanner_buffers_mu_);
					for (int i = 0; i < 3; i++) {
						for (const auto& kv : per_scanner_buffers_[i]) {
							epoch_total += kv.second.size();
						}
					}
				}
				LOG(INFO) << "[EpochSequencer2 Diag] topic=" << topic_name_
				          << " pending_batches=" << epoch_total
				          << " last=" << last << " current=" << current;
				last_diag_time = now;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(10));
			continue;
		}
		size_t buffer_idx = last % 3;
		std::vector<PendingBatch5> batch_list;
		{
			// [[PERF: Merge per-scanner buffers]] Lock only when merging (once per epoch), not on hot path.
			absl::MutexLock lock(&per_scanner_buffers_mu_);
			for (auto& kv : per_scanner_buffers_[buffer_idx]) {
				batch_list.insert(batch_list.end(), 
					std::make_move_iterator(kv.second.begin()),
					std::make_move_iterator(kv.second.end()));
				kv.second.clear();
			}
		}
		last_sequenced_epoch_.store(last + 1, std::memory_order_release);

		if (batch_list.empty()) {
			continue;
		}

		// [[PHASE_1A_EPOCH_FENCING]] Sequencer-side epoch validation (§4.2)
		ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
		CXL::flush_cacheline(control_block);
		CXL::load_fence();
		uint64_t current_epoch = control_block->epoch.load(std::memory_order_acquire);
		constexpr uint16_t kMaxEpochAge = 3;
		batch_list.erase(
			std::remove_if(batch_list.begin(), batch_list.end(),
				[current_epoch, kMaxEpochAge](const PendingBatch5& p) {
					uint16_t cur = static_cast<uint16_t>(current_epoch & 0xFFFF);
					uint16_t created = p.epoch_created;
					uint16_t age = (cur - created) & 0xFFFF;
					return age > kMaxEpochAge;
				}),
			batch_list.end());

		if (batch_list.empty()) {
			continue;
		}

		// Order 2: No Level 5 partition. All batches go directly to ready (arrival order).
		std::vector<PendingBatch5> ready;
		for (PendingBatch5& p : batch_list) {
			ready.push_back(std::move(p));
		}

		// One atomic per epoch (§3.2)
		size_t total_msg = 0;
		for (const PendingBatch5& p : ready) total_msg += p.num_msg;
		size_t base_order = global_seq_.fetch_add(total_msg, std::memory_order_relaxed);

		// Sort by (broker_id, slot_offset) for correct consumed_through and export chain
		std::sort(ready.begin(), ready.end(), [](const PendingBatch5& a, const PendingBatch5& b) {
			if (a.broker_id != b.broker_id) return a.broker_id < b.broker_id;
			return a.slot_offset < b.slot_offset;
		});

		size_t next_order = base_order;
		absl::flat_hash_map<int, size_t> contiguous_consumed_per_broker;
		for (const PendingBatch5& p : ready) {
			int b = p.broker_id;
			if (contiguous_consumed_per_broker.contains(b)) continue;
			const volatile void* addr = reinterpret_cast<const volatile void*>(&tinode_->offsets[b].batch_headers_consumed_through);
			CXL::flush_cacheline(const_cast<const void*>(addr));
			CXL::load_fence();
			size_t initial = tinode_->offsets[b].batch_headers_consumed_through;
			if (initial == BATCHHEADERS_SIZE) {
				initial = 0;
			}
			contiguous_consumed_per_broker[b] = initial;
		}
		absl::MutexLock header_lock(&phase1b_header_for_sub_mu_);

		GOIEntry* goi = reinterpret_cast<GOIEntry*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kGOIOffset);

		for (PendingBatch5& p : ready) {
			if (p.skipped) {
				int b = p.broker_id;
				size_t& next_expected = contiguous_consumed_per_broker[b];
				if (p.slot_offset >= next_expected) {
					next_expected = p.slot_offset + sizeof(BatchHeader);
				}
				continue;
			}

			p.hdr->total_order = next_order;

			uint64_t batch_index = global_batch_seq_.fetch_add(1, std::memory_order_relaxed);

			GOIEntry* entry = &goi[batch_index];
			entry->global_seq = batch_index;
			entry->total_order = next_order;
			entry->batch_id = p.hdr->batch_id;
			entry->broker_id = static_cast<uint16_t>(p.broker_id);
			entry->epoch_sequenced = p.epoch_created;
			entry->blog_offset = p.hdr->log_idx;
			entry->payload_size = static_cast<uint32_t>(p.hdr->total_size);
			entry->message_count = static_cast<uint32_t>(p.num_msg);
			entry->num_replicated.store(0, std::memory_order_release);
			entry->client_id = p.client_id;
			entry->client_seq = p.hdr->batch_seq;
			entry->pbr_index = p.hdr->pbr_absolute_index;

			// Flush GOI entry so replicas/recovery see it on non-coherent CXL
			CXL::flush_cacheline(entry);

			if (replication_factor_ > 0) {
				uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				size_t ring_pos = goi_timestamp_write_pos_.fetch_add(1, std::memory_order_relaxed) % kGOITimestampRingSize;
				goi_timestamps_[ring_pos].goi_index.store(batch_index, std::memory_order_relaxed);
				goi_timestamps_[ring_pos].timestamp_ns.store(now_ns, std::memory_order_release);
			}

			next_order += p.num_msg;

			int b = p.broker_id;
			tinode_->offsets[b].ordered += p.num_msg;
			tinode_->offsets[b].ordered_offset = static_cast<size_t>(
				reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(cxl_addr_));

			auto it = phase1b_header_for_sub_.find(b);
			if (it != phase1b_header_for_sub_.end()) {
				BatchHeader* sub = it->second;
				sub->batch_off_to_export = reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(sub);
				sub->ordered = 1;
				BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[b].batch_headers_offset);
				BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(ring_start) + BATCHHEADERS_SIZE);
				BatchHeader* next_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(sub) + sizeof(BatchHeader));
				if (next_sub >= ring_end) next_sub = ring_start;
				it->second = next_sub;
			}

			p.hdr->batch_complete = 0;
			CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(&tinode_->offsets[b].ordered)));
			CXL::flush_cacheline(p.hdr);
			size_t& next_expected = contiguous_consumed_per_broker[b];
			if (p.slot_offset == next_expected) {
				next_expected = p.slot_offset + sizeof(BatchHeader);
			}
		}
		CXL::store_fence();

		for (const auto& kv : contiguous_consumed_per_broker) {
			int b = kv.first;
			tinode_->offsets[b].batch_headers_consumed_through = kv.second;
			CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].batch_headers_consumed_through));
		}
		CXL::store_fence();
	}
}

void Topic::EpochDriverThread() {
	LOG(INFO) << "EpochDriverThread started (epoch_us=" << kEpochUs << ")";
	constexpr uint64_t kNewBrokerCheckInterval = 2000;  // Check every 2000 epochs (~1 second)
	uint64_t epoch_count = 0;
	while (!stop_threads_) {
		std::this_thread::sleep_for(std::chrono::microseconds(kEpochUs));
		if (stop_threads_) break;
		epoch_index_.fetch_add(1, std::memory_order_release);

		// [[FIX: B3=0 ACKs]] Periodically check for newly registered brokers
		if (++epoch_count % kNewBrokerCheckInterval == 0) {
			CheckAndSpawnNewScanners();
		}
	}
}

void Topic::CheckAndSpawnNewScanners() {
	// Get current registered brokers
	absl::btree_set<int> current_brokers;
	GetRegisteredBrokerSet(current_brokers);

	// [[SEQUENCER_ONLY_HEAD_NODE]] Filter out B0 if sequencer-only
	if (is_sequencer_only_) {
		current_brokers.erase(0);
	}

	// Check for new brokers and spawn scanners
	absl::MutexLock lock(&scanner_management_mu_);
	for (int broker_id : current_brokers) {
		if (brokers_with_scanners_.find(broker_id) == brokers_with_scanners_.end()) {
			// New broker found - spawn a scanner!
			LOG(INFO) << "[DYNAMIC_SCANNER] Detected newly registered broker " << broker_id
			         << ", spawning BrokerScannerWorker5";

			// Initialize per-broker header_for_sub for the new broker
			{
				absl::MutexLock sub_lock(&phase1b_header_for_sub_mu_);
				if (phase1b_header_for_sub_.find(broker_id) == phase1b_header_for_sub_.end()) {
					BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
					phase1b_header_for_sub_[broker_id] = ring_start;
				}
			}

			brokers_with_scanners_.insert(broker_id);
			scanner_threads_.emplace_back(&Topic::BrokerScannerWorker5, this, broker_id);

			LOG(INFO) << "[DYNAMIC_SCANNER] Started BrokerScannerWorker5 for broker " << broker_id
			         << ", total scanners now: " << brokers_with_scanners_.size();
		}
	}
}

void Topic::EpochSequencerThread() {
	LOG(INFO) << "EpochSequencerThread started for topic: " << topic_name_;
	static thread_local auto last_diag_time = std::chrono::steady_clock::now();
	while (!stop_threads_) {
		uint64_t last = last_sequenced_epoch_.load(std::memory_order_acquire);
		uint64_t current = epoch_index_.load(std::memory_order_acquire);
		if (last >= current) {
			// [[DIAGNOSTIC]] Periodic snapshot when idle (every 5s) for ACK stall diagnosis
			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::seconds>(now - last_diag_time).count() >= 5) {
				size_t hold_sz = 0;
				size_t epoch_total = 0;
				{
					absl::MutexLock lock(&per_scanner_buffers_mu_);
					for (int i = 0; i < 3; i++) {
						for (const auto& kv : per_scanner_buffers_[i]) {
							epoch_total += kv.second.size();
						}
					}
				}
				{
					absl::MutexLock lock(&level5_state_mu_);
					hold_sz = hold_buffer_size_;
				}
				LOG(INFO) << "[EpochSequencer Diag] topic=" << topic_name_
				          << " hold_buffer_size=" << hold_sz
				          << " pending_batches=" << epoch_total
				          << " last=" << last << " current=" << current;
				last_diag_time = now;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(10));
			continue;
		}
		size_t buffer_idx = last % 3;
		std::vector<PendingBatch5> batch_list;
		{
			// [[PERF: Merge per-scanner buffers]] Lock only during merge, not on hot path.
			absl::MutexLock lock(&per_scanner_buffers_mu_);
			for (auto& kv : per_scanner_buffers_[buffer_idx]) {
				batch_list.insert(batch_list.end(), 
					std::make_move_iterator(kv.second.begin()),
					std::make_move_iterator(kv.second.end()));
				kv.second.clear();
			}
		}
		last_sequenced_epoch_.store(last + 1, std::memory_order_release);
		current_epoch_for_hold_ = last + 1;

		if (batch_list.empty()) {
			continue;
		}

		// [[PHASE_1A_EPOCH_FENCING]] Sequencer-side epoch validation (§4.2)
		// Reject batches with stale epoch_created so replicas never see zombie-sequencer data
		ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
		CXL::flush_cacheline(control_block);
		CXL::load_fence();
		uint64_t current_epoch = control_block->epoch.load(std::memory_order_acquire);
		constexpr uint16_t kMaxEpochAge = 3;  // Design §4.2: reject if entry.epoch < current_epoch - MAX_EPOCH_AGE
		batch_list.erase(
			std::remove_if(batch_list.begin(), batch_list.end(),
				[current_epoch, kMaxEpochAge](const PendingBatch5& p) {
					uint16_t cur = static_cast<uint16_t>(current_epoch & 0xFFFF);
					uint16_t created = p.epoch_created;
					uint16_t age = (cur - created) & 0xFFFF;
					return age > kMaxEpochAge;
				}),
			batch_list.end());

		if (batch_list.empty()) {
			continue;
		}

		// Partition Level 0 (client_id==0) vs Level 5 (client_id!=0)
		// Skipped markers now carry real client_id/batch_seq and go through level5 so hold buffer can advance (§3.2)
		std::vector<PendingBatch5> level0, level5;
		for (PendingBatch5& p : batch_list) {
			if (p.client_id == 0) {
				level0.push_back(std::move(p));
			} else {
				level5.push_back(std::move(p));
			}
		}

		// Process Level 5: hold buffer + gap timeout (§3.2)
		std::vector<PendingBatch5> ready_level5;
		ProcessLevel5Batches(level5, ready_level5);

		// Merge Level 0 + Level 5 ready; preserve order for stable commit (we sort by broker+slot later)
		std::vector<PendingBatch5> ready;
		for (PendingBatch5& p : level0) ready.push_back(std::move(p));
		for (PendingBatch5& p : ready_level5) ready.push_back(std::move(p));

		if (ready.empty()) continue;

		// One atomic per epoch (§3.2)
		size_t total_msg = 0;
		for (const PendingBatch5& p : ready) total_msg += p.num_msg;
		size_t base_order = global_seq_.fetch_add(total_msg, std::memory_order_relaxed);

		// Sort by (broker_id, slot_offset) for correct consumed_through and export chain
		std::sort(ready.begin(), ready.end(), [](const PendingBatch5& a, const PendingBatch5& b) {
			if (a.broker_id != b.broker_id) return a.broker_id < b.broker_id;
			return a.slot_offset < b.slot_offset;
		});

		// Assign total_order and commit
		size_t next_order = base_order;
		// [[CONSUMED_THROUGH_FIX]] Min-contiguous per broker (see docs/memory-bank/LOCKFREE_BATCH_HEADER_RING_DESIGN.md).
		// Scanner can skip slots (batch_complete=0), so epoch buffer may have e.g. 700,702 but not 701.
		// If we used max(slot_offset+64), we'd set consumed_through past 702 and allow producer to
		// overwrite 701 (never processed) → batch loss and ACK stall. We only advance
		// consumed_through when the slot we process is exactly the next expected (contiguous).
		absl::flat_hash_map<int, size_t> contiguous_consumed_per_broker;
		for (const PendingBatch5& p : ready) {
			int b = p.broker_id;
			if (contiguous_consumed_per_broker.contains(b)) continue;
			const volatile void* addr = reinterpret_cast<const volatile void*>(&tinode_->offsets[b].batch_headers_consumed_through);
			CXL::flush_cacheline(const_cast<const void*>(addr));
			CXL::load_fence();
			size_t initial = tinode_->offsets[b].batch_headers_consumed_through;
			// [[SENTINEL]] BATCHHEADERS_SIZE means "all slots free" (topic_manager.cc); for min-contiguous
			// we need "next expected = slot 0", so convert sentinel to 0. Otherwise slot_offset (0,64,...)
			// never equals BATCHHEADERS_SIZE and consumed_through would never advance → no backpressure.
			if (initial == BATCHHEADERS_SIZE) {
				initial = 0;
			}
			contiguous_consumed_per_broker[b] = initial;
		}
		absl::MutexLock header_lock(&phase1b_header_for_sub_mu_);

		// [[PHASE_2]] Get GOI pointer for writing sequenced batches
		GOIEntry* goi = reinterpret_cast<GOIEntry*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kGOIOffset);

		for (PendingBatch5& p : ready) {
			// [[CONSUMED_THROUGH_SKIP]] Skipped slot: only advance consumed_through so ring can drain
			// [[CONSUMED_THROUGH_SKIP]] Skipped slot: advance consumed_through so ring can drain.
			// [[SKIP_THE_GAP]] Accept slot_offset >= next_expected (skip the gap). Empty slots before
			// this one have no data; we're already discarding this batch, so advancing loses nothing.
			if (p.skipped) {
				int b = p.broker_id;
				size_t& next_expected = contiguous_consumed_per_broker[b];
				if (p.slot_offset >= next_expected) {
					next_expected = p.slot_offset + sizeof(BatchHeader);
				}
				continue;
			}

			p.hdr->total_order = next_order;

			// [[PHASE_2_FIX]] Use sequential batch counter for GOI indexing (NOT message count)
			// This ensures replicas can poll GOI[0], GOI[1], GOI[2]... without gaps
			uint64_t batch_index = global_batch_seq_.fetch_add(1, std::memory_order_relaxed);

			// [[PHASE_2]] Write batch to GOI after assigning total_order
			// Sequencer is single-writer, no coordination needed
			GOIEntry* entry = &goi[batch_index];
			entry->global_seq = batch_index;           // Sequential batch index (0, 1, 2, 3...)
			entry->total_order = next_order;           // Starting message sequence number
			entry->batch_id = p.hdr->batch_id;
			entry->broker_id = static_cast<uint16_t>(p.broker_id);
			entry->epoch_sequenced = p.epoch_created;  // Use batch's creation epoch
			entry->blog_offset = p.hdr->log_idx;
			entry->payload_size = static_cast<uint32_t>(p.hdr->total_size);
			entry->message_count = static_cast<uint32_t>(p.num_msg);
			entry->num_replicated.store(0, std::memory_order_release);  // Replicas will increment
			entry->client_id = p.client_id;
			entry->client_seq = p.hdr->batch_seq;

			// [[PHASE_2_FIX]] Use absolute PBR index (from BatchHeader, generated at ingestion)
			// This is monotonic and never wraps, ensuring CV contiguity
			entry->pbr_index = p.hdr->pbr_absolute_index;

			// Flush GOI entry to CXL (non-coherent memory requires explicit flush)
			CXL::flush_cacheline(entry);

			// [[PHASE_3]] Record GOI write timestamp for recovery monitoring (§4.2.2)
			// Lock-free write - no mutex on hot path!
			// Recovery thread scans this ring periodically to detect stalled chains
			if (replication_factor_ > 0) {
				uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				size_t ring_pos = goi_timestamp_write_pos_.fetch_add(1, std::memory_order_relaxed) % kGOITimestampRingSize;
				goi_timestamps_[ring_pos].goi_index.store(batch_index, std::memory_order_relaxed);
				goi_timestamps_[ring_pos].timestamp_ns.store(now_ns, std::memory_order_release);
			}

			next_order += p.num_msg;

			int b = p.broker_id;
			tinode_->offsets[b].ordered += p.num_msg;
			tinode_->offsets[b].ordered_offset = static_cast<size_t>(
				reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(cxl_addr_));

			auto it = phase1b_header_for_sub_.find(b);
			if (it != phase1b_header_for_sub_.end()) {
				BatchHeader* sub = it->second;
				sub->batch_off_to_export = reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(sub);
				sub->ordered = 1;
				BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[b].batch_headers_offset);
				BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(ring_start) + BATCHHEADERS_SIZE);
				BatchHeader* next_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(sub) + sizeof(BatchHeader));
				if (next_sub >= ring_end) next_sub = ring_start;
				it->second = next_sub;
			}

			p.hdr->batch_complete = 0;
			CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(&tinode_->offsets[b].ordered)));
			CXL::flush_cacheline(p.hdr);
			// Advance contiguous consumed only when this slot is exactly the next expected (no gap).
			size_t& next_expected = contiguous_consumed_per_broker[b];
			if (p.slot_offset == next_expected) {
				next_expected = p.slot_offset + sizeof(BatchHeader);
			}
			// Else: gap (slot not in epoch buffer); do not advance so producer cannot overwrite it.
		}
		CXL::store_fence();

		for (const auto& kv : contiguous_consumed_per_broker) {
			int b = kv.first;
			tinode_->offsets[b].batch_headers_consumed_through = kv.second;
			CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].batch_headers_consumed_through));
		}
		CXL::store_fence();

		// [[ACK_TIMEOUT_ORDER5]] Periodic epoch summary every 5s for stall diagnosis (H3/H4)
		{
			static thread_local auto last_epoch_summary_time = std::chrono::steady_clock::now();
			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::seconds>(now - last_epoch_summary_time).count() >= 5) {
				size_t hb = 0;
				{ absl::MutexLock lock(&level5_state_mu_); hb = hold_buffer_size_; }
				LOG(INFO) << "[EpochSeq] epoch=" << last << " processed=" << ready.size()
					<< " consumed_through=[B0:" << tinode_->offsets[0].batch_headers_consumed_through
					<< " B1:" << tinode_->offsets[1].batch_headers_consumed_through
					<< " B2:" << tinode_->offsets[2].batch_headers_consumed_through
					<< " B3:" << tinode_->offsets[3].batch_headers_consumed_through << "]"
					<< " hold_buffer=" << hb;
				last_epoch_summary_time = now;
			}
		}
	}
}

void Topic::ProcessLevel5Batches(std::vector<PendingBatch5>& level5, std::vector<PendingBatch5>& ready) {
	absl::MutexLock lock(&level5_state_mu_);
	// Sort by client_id then batch_seq (client_seq)
	std::sort(level5.begin(), level5.end(), [](const PendingBatch5& a, const PendingBatch5& b) {
		if (a.client_id != b.client_id) return a.client_id < b.client_id;
		return a.batch_seq < b.batch_seq;
	});

	for (auto it = level5.begin(); it != level5.end(); ) {
		size_t client_id = it->client_id;
		auto group_end = it;
		while (group_end != level5.end() && group_end->client_id == client_id) ++group_end;

		ClientState5& state = level5_client_state_[client_id];
		for (auto jt = it; jt != group_end; ++jt) {
			// [[HOLD_BUFFER_SKIP]] Skipped (in-flight) batch: advance next_expected so held batches for this client can be released
			if (jt->skipped) {
				if (jt->batch_seq == state.next_expected) {
					state.next_expected = jt->batch_seq + 1;
					state.highest_sequenced = jt->batch_seq;
					state.recent_window.set(jt->batch_seq % 128);
				}
				ready.push_back(*jt);  // EpochSequencerThread still advances consumed_through for this marker
				continue;
			}
			size_t seq = jt->batch_seq;
			bool dup = (seq <= state.highest_sequenced && state.recent_window.test(seq % 128));
			if (dup) continue;
			if (seq == state.next_expected) {
				ready.push_back(*jt);
				state.next_expected = seq + 1;
				state.highest_sequenced = seq;
				state.recent_window.set(seq % 128);
			} else if (seq > state.next_expected) {
				if (hold_buffer_size_ < kHoldBufferMaxEntries) {
					hold_buffer_[client_id][seq] = HoldEntry5{*jt, 0};
					hold_buffer_size_++;
				} else {
					// Timeout: skip gap; mark in recent_window so duplicates are deduped (§3.2)
					state.next_expected = seq + 1;
					state.highest_sequenced = seq;
					state.recent_window.set(seq % 128);
					ready.push_back(*jt);
				}
			}
		}
		it = group_end;
	}

	// Drain hold buffer: entries that are now in-sequence or timed out
	std::vector<std::pair<size_t, size_t>> to_remove;
	for (auto& client_it : hold_buffer_) {
		size_t cid = client_it.first;
		ClientState5& state = level5_client_state_[cid];
		for (auto& seq_it : client_it.second) {
			HoldEntry5& he = seq_it.second;
			he.wait_epochs++;
			if (he.batch.batch_seq == state.next_expected) {
				ready.push_back(he.batch);
				state.next_expected = he.batch.batch_seq + 1;
				state.highest_sequenced = he.batch.batch_seq;
				state.recent_window.set(he.batch.batch_seq % 128);
				to_remove.push_back({cid, seq_it.first});
				hold_buffer_size_--;
			} else if (he.wait_epochs >= kMaxWaitEpochs) {
				// Timeout: skip gap; mark in recent_window so duplicates are deduped (§3.2)
				state.next_expected = he.batch.batch_seq + 1;
				state.highest_sequenced = he.batch.batch_seq;
				state.recent_window.set(he.batch.batch_seq % 128);
				ready.push_back(he.batch);
				to_remove.push_back({cid, seq_it.first});
				hold_buffer_size_--;
			}
		}
	}
	for (const auto& kv : to_remove) {
		hold_buffer_[kv.first].erase(kv.second);
		if (hold_buffer_[kv.first].empty()) hold_buffer_.erase(kv.first);
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

	size_t total_batches_processed = 0;
	auto last_log_time = std::chrono::steady_clock::now();
	auto scanner_heartbeat_time = last_log_time;  // [[ACK_TIMEOUT_ORDER5]] Per-scanner 5s heartbeat for stall diagnosis
	size_t idle_cycles = 0;

	// [[PEER_REVIEW #10]] Named constants for scanner wait/sleep (document 100µs in-flight wait, 50µs idle advance)
	constexpr int kScannerSleepStuckSlotUs = 100;  // Sleep when retrying same in-flight slot (idle_cycles >= 1024)
	constexpr int kScannerSleepIdleAdvanceUs = 50; // Sleep when advancing past empty slot

	// [[CRITICAL FIX: Simplified Polling Logic]]
	// Match message_ordering.cc pattern: Simply check num_msg and advance if not ready
	// Don't use written_addr as polling signal - it causes complexity and bugs
	// The working implementation in message_ordering.cc doesn't use written_addr at all
	// See docs/CRITICAL_BUG_FOUND_2026_01_26.md
	
	while (!stop_threads_) {
		static thread_local size_t scan_loops = 0;
		static thread_local size_t ready_batches_seen = 0;

		// [[CRITICAL FIX: Always invalidate cache before reading batch_complete on non-coherent CXL]]
		// CORRECTNESS: Writer (NetworkManager) writes batch_complete=1 + flush. Reader MUST invalidate
		// before EVERY read to see remote writes. The "1000x overhead reduction" was a bug - it caused
		// the sequencer to read stale cached batch_complete=0 for up to 999 iterations, missing batches.
		// This is the root cause of ACK stalls: sequencer stops processing → no ordered updates → no ACKs.
		// Trade-off: Correctness > CPU efficiency. Non-coherent CXL requires this overhead.
		CXL::flush_cacheline(current_batch_header);
		CXL::load_fence();

		++scan_loops;

		// Check current batch header (matches message_ordering.cc:600-617 pattern)
		// num_msg is uint32_t in BatchHeader, so read as volatile uint32_t for type safety
		// For non-coherent CXL: volatile prevents compiler caching; ACQUIRE doesn't help cache coherence
		volatile uint32_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
		// Read log_idx as volatile for consistency (written by remote broker via NetworkManager)
		volatile size_t log_idx_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->log_idx;

		// Max reasonable: 2MB batch / 64B min message = ~32k messages, use 100k as safety limit
		constexpr uint32_t MAX_REASONABLE_NUM_MSG = 100000;
		// [[DESIGN: Write PBR only after receive]] Broker writes BatchHeader to PBR once after payload
		// is in the blog. Readiness = num_msg>0 (slot is zeroed until then). No batch_complete needed.
		bool batch_ready = (num_msg_check > 0 && num_msg_check <= MAX_REASONABLE_NUM_MSG);
		// Per-scanner heartbeat every 5s for stall diagnosis
		{
			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::seconds>(now - scanner_heartbeat_time).count() >= 5) {
				const char* state_str = batch_ready ? "READY" : "EMPTY";
				LOG(INFO) << "[Scanner B" << broker_id << "] slot=" << std::hex << current_batch_header << std::dec
					<< " num_msg=" << num_msg_check
					<< " state=" << state_str << " processed=" << total_batches_processed;
				scanner_heartbeat_time = now;
			}
		}
		if (!batch_ready) {
			++idle_cycles;
			// Slot is empty (num_msg=0) or invalid - advance to next slot
			// This is safe because there's no batch data to lose
			BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
			if (next_batch_header >= ring_end) {
				next_batch_header = ring_start_default;
			}
			current_batch_header = next_batch_header;
			if (idle_cycles >= 1024) {
				std::this_thread::sleep_for(std::chrono::microseconds(kScannerSleepIdleAdvanceUs));
				idle_cycles = 0;
			} else {
				CXL::cpu_pause();
			}
			continue;
		}
		// [[PERF: Reduce hot-path logging]]
		// At high throughput (10k+ batches/sec), LOG(INFO) every 100-200 batches adds ~10% overhead
		// Use VLOG for high-frequency logs; LOG(INFO) only for first few and periodic summary
		size_t ready_seen = ++ready_batches_seen;
		if (ready_seen <= 5) {
			// Log first 5 batches at INFO level for startup diagnostics
			LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: batch_ready_seen=" << ready_seen
			          << " batch_seq=" << current_batch_header->batch_seq
			          << " client_id=" << current_batch_header->client_id
			          << " num_msg=" << num_msg_check;
		} else {
			// Use VLOG for high-frequency logs
			VLOG(2) << "BrokerScannerWorker5 [B" << broker_id << "]: batch_ready_seen=" << ready_seen
			        << " batch_seq=" << current_batch_header->batch_seq
			        << " num_msg=" << num_msg_check;
		}

		// [[PHASE_1B]] Batch ready: push to epoch buffer; EpochSequencerThread will assign order and commit
		VLOG(3) << "BrokerScannerWorker5 [B" << broker_id << "]: Collecting batch with "
		        << num_msg_check << " messages, batch_seq=" << current_batch_header->batch_seq
		        << ", client_id=" << current_batch_header->client_id;

		size_t slot_offset = reinterpret_cast<uint8_t*>(current_batch_header) -
			reinterpret_cast<uint8_t*>(ring_start_default);
		PendingBatch5 pending;
		pending.hdr = current_batch_header;
		pending.broker_id = broker_id;
		pending.num_msg = num_msg_check;
		pending.client_id = current_batch_header->client_id;
		pending.batch_seq = current_batch_header->batch_seq;
		pending.slot_offset = slot_offset;
		pending.epoch_created = static_cast<uint16_t>(current_batch_header->epoch_created);
		{
			// [[CORRECTNESS]] Serialize with EpochSequencerThread merge/clear to avoid data race.
			// Load epoch inside lock so we never push to a buffer the sequencer has already merged/drained.
			absl::MutexLock lock(&per_scanner_buffers_mu_);
			uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
			per_scanner_buffers_[epoch % 3][broker_id].push_back(pending);
		}
		total_batches_processed++;
		idle_cycles = 0;

		// Periodic status logging (VLOG to avoid hot-path overhead during throughput tests)
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
			VLOG(2) << "BrokerScannerWorker5 [B" << broker_id << "]: Processed " << total_batches_processed 
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
	// Flush the SEQUENCER region cachelines (bytes 512-767 within 768B offset_entry)
	// offset_entry layout (768 bytes total):
	// - bytes 0-255: broker_region (log_offset, batch_headers_offset, written, written_addr, padding)
	// - bytes 256-511: replication_done[NUM_MAX_BROKERS] (replication progress)
	// - bytes 512-767: sequencer_region (ordered, ordered_offset, padding) <- WE NEED TO FLUSH THIS
	//
	// With flat layout (no nested structs), 'ordered' is at offset 512 relative to offset_entry base.
	// CXL::flush_cacheline will flush the full 64B cache line containing 'ordered'.
	// This ensures both 'ordered' and 'ordered_offset' (@ offset +8 from ordered) are visible.
	//
	// OPTIMIZATION: Combine two flushes before single fence
	// Both sequencer-fields and BatchHeader flushes can precede the same fence
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

/**
 * Start/Stop GOIRecoveryThread (added in Topic constructor/destructor)
 * This is a placeholder - actual thread start/stop will be added to constructor/destructor
 */
