#include "topic.h"
#include "../cxl_manager/scalog_local_sequencer.h"
#include "common/performance_utils.h"
#include "../common/wire_formats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>


namespace Embarcadero {

// [[ORDER5_ACK_STALL]] Use BatchHeader flags to mark CLAIMED/VALID; scanner never skips CLAIMED slots.
static inline uint64_t SteadyNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

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
	current_segment_(segment_metadata) {

		// Validate tinode pointer first
		if (!tinode_) {
			LOG(FATAL) << "TInode is null for topic: " << topic_name;
		}

		// Validate offsets before using them
		if (tinode_->offsets[broker_id_].log_offset == 0) {
			throw std::runtime_error("Tinode not initialized for broker " + std::to_string(broker_id_) +
			                        " in topic: " + topic_name_);
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

		ack_level_ = tinode_->ack_level;
		replication_factor_ = tinode_->replication_factor;
		ordered_offset_addr_ = nullptr;
		ordered_offset_ = 0;

		// Set appropriate get buffer function based on sequencer type
		if (seq_type == KAFKA) {
			GetCXLBufferFunc = &Topic::KafkaGetCXLBuffer;
		} else if (seq_type == CORFU) {
			// Initialize Corfu replication client
		// Read replica addresses from environment variable (comma-separated)
		const char* env_addrs = std::getenv("CORFU_REPLICA_ADDRS");
		std::string replica_addrs = env_addrs ? env_addrs : ("127.0.0.1:" + std::to_string(CORFU_REP_PORT));
			corfu_replication_client_ = std::make_unique<Corfu::CorfuReplicationClient>(
					topic_name, 
					replication_factor_, 
					replica_addrs
					);

			if (!corfu_replication_client_->Connect()) {
				LOG(ERROR) << "Corfu replication client failed to connect to replica";
			}

			// Route to appropriate CORFU implementation based on order level
		if (order_ == 3) {
			GetCXLBufferFunc = &Topic::CorfuOrder3GetCXLBuffer;
		} else {
			GetCXLBufferFunc = &Topic::CorfuGetCXLBuffer;  // Order 0 or 4
		}
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


		// Constructor completes initialization without starting threads
		// Call Start() method separately to begin thread execution
}

void Topic::Start() {
	// Ensure all initialization is complete before starting threads
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	// Start delegation thread if needed (Stage 2: Local Ordering)
	// [PHASE-1] Skip DelegationThread for Order 5 and Order 2 with EMBARCADERO sequencer.
	// Reason 1: Order 5/2 subscribers use GetBatchToExportWithMetadata (GOI/CV-based),
	//           not GetMessageAddr (which needs DelegationThread's written/written_addr).
	// Reason 2: DelegationThread and BrokerScannerWorker5 both read the same PBR ring,
	//           causing CXL cache line invalidation ping-pong (~30% scanner latency hit).
	// Reason 3: DelegationThread has a ring-wrap bug (Phase 9) that doesn't affect Order 5/2
	//           if DelegationThread is disabled.
	bool skip_delegation = false;
	if (order_ == 5 && seq_type_ == EMBARCADERO) {
		skip_delegation = true;
		LOG(INFO) << "Topic " << topic_name_ << ": DelegationThread disabled for Order 5 "
		          << "(subscribers use GetBatchToExportWithMetadata; eliminates scanner contention)";
	}
	if (order_ == 2 && seq_type_ == EMBARCADERO) {
		skip_delegation = true;
		LOG(INFO) << "Topic " << topic_name_ << ": DelegationThread disabled for Order 2 "
		          << "(subscribers use GetBatchToExportWithMetadata)";
	}
	// [CORFU_ORDER3] Skip DelegationThread for CORFU Order 3
	// CORFU assigns order before publish (external sequencer), so no local ordering needed
	if (order_ == 3 && seq_type_ == CORFU) {
		skip_delegation = true;
		LOG(INFO) << "Topic " << topic_name_ << ": DelegationThread disabled for CORFU Order 3 "
		          << "(external sequencer assigns order before publish)";
	}
	if (!skip_delegation &&
	    (seq_type_ == CORFU || (seq_type_ != KAFKA && order_ != 4))) {
		delegationThreads_.emplace_back(&Topic::DelegationThread, this);
	}

	// Head node runs sequencer
	if(broker_id_ == 0){
		LOG(INFO) << "Topic Start: broker_id=" << broker_id_ << ", order=" << order_ << ", seq_type=" << seq_type_;
		switch(seq_type_){
			case KAFKA: // Kafka is just a way to not run DelegationThread, not actual sequencer
			case EMBARCADERO:
				if (order_ == 1)
					LOG(ERROR) << "Sequencer 1 is not ported yet from cxl_manager";
					//sequencerThread_ = std::thread(&Topic::Sequencer1, this);
				else if (order_ == 2) {
					LOG(INFO) << "Creating Sequencer2 thread for order level 2 (total order)";
					if (replication_factor_ > 0) {
						goi_recovery_thread_ = std::thread(&Topic::GOIRecoveryThread, this);
						LOG(INFO) << "Started GOIRecoveryThread for topic " << topic_name_;
					}
					sequencerThread_ = std::thread(&Topic::Sequencer2, this);
				}
				else if (order_ == 3)
					LOG(ERROR) << "Sequencer 3 is not ported yet";
					//sequencerThread_ = std::thread(&Topic::Sequencer3, this);
				else if (order_ == 4){
					sequencerThread_ = std::thread(&Topic::Sequencer4, this);
				}
				else if (order_ == 5){
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
				if (order_ == 1){
					sequencerThread_ = std::thread(&Topic::StartScalogLocalSequencer, this);
					// Already started when creating topic instance from topic manager
				}else if (order_ == 2)
					LOG(ERROR) << "Order is set 2 at scalog";
				break;
			case CORFU:
				if (order_ == 0 || order_ == 3 || order_ == 4)
					VLOG(3) << "Order " << order_ <<
						" for Corfu is right as messages are written ordered. Combiner combining is enough";
				else
					LOG(ERROR) << "Wrong Order is set for corfu " << order_;
				break;
			default:
				LOG(ERROR) << "Unknown sequencer:" << seq_type_;
				break;
		}
	}
}

void Topic::StartScalogLocalSequencer() {
	// int unique_port = SCALOG_SEQ_PORT + scalog_local_sequencer_port_offset_.fetch_add(1);
	BatchHeader* batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	Scalog::ScalogLocalSequencer scalog_local_sequencer(tinode_, broker_id_, cxl_addr_, topic_name_, batch_header);
	scalog_local_sequencer.SendLocalCut(topic_name_, stop_threads_volatile_);
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

	// [PHASE-9] Ring boundaries for wrap check (prevents OOB read after ring wrap)
	BatchHeader* delegation_ring_start = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	BatchHeader* delegation_ring_end = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(delegation_ring_start) + BATCHHEADERS_SIZE);
	
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
			
			BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
			if (next_batch >= delegation_ring_end) {
				next_batch = delegation_ring_start;
			}
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

std::function<void(void*, size_t)> Topic::CorfuOrder3GetCXLBuffer(
		BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE],
		void* &log,
		void* &segment_header,
		size_t &logical_offset,
		BatchHeader* &batch_header_location,
		bool /*epoch_already_checked*/) {

	// CORFU Order 3: No DelegationThread, no sequencer
	// Publisher already assigned total_order via corfu_client_->GetTotalOrder()

	// CRITICAL FIX: Allocate PBR slot for ACK pipeline
	// Even though Order 3 doesn't use DelegationThread, NetworkManager needs
	// batch_header_location to set batch_complete=1 for ACKs
	BatchHeader* batch_header_ring = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);

	batch_header_location = &batch_header_ring[batch_header.batch_seq % (BATCHHEADERS_SIZE / sizeof(BatchHeader))];

	// [[CRITICAL FIX]] Clear batch_complete before reusing PBR slot
	// When slots are reused (batch_seq wraps around), stale batch_complete=1 from
	// previous batch causes callback to immediately proceed instead of waiting
	__atomic_store_n(&batch_header_location->batch_complete, 0, __ATOMIC_RELEASE);

	// Calculate addresses
	const unsigned long long int segment_metadata =
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;
	BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);

	// Allocate log space at offset assigned by CORFU sequencer
	log = reinterpret_cast<void*>(log_addr_.load() + batch_header.log_idx);

	// Check for segment boundary issues
	CheckSegmentBoundary(log, msg_size, segment_metadata);

	// Store batch metadata in CXL batch header ring
	// *** CRITICAL FIX: Preserve total_order from publisher! ***
	batch_header_log[batch_header.batch_seq].batch_seq = batch_header.batch_seq;
	batch_header_log[batch_header.batch_seq].total_size = batch_header.total_size;
	batch_header_log[batch_header.batch_seq].num_msg = batch_header.num_msg;
	batch_header_log[batch_header.batch_seq].broker_id = broker_id_;
	batch_header_log[batch_header.batch_seq].ordered = 0;
	batch_header_log[batch_header.batch_seq].batch_off_to_export = 0;
	batch_header_log[batch_header.batch_seq].log_idx = static_cast<size_t>(
		reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_));
	batch_header_log[batch_header.batch_seq].total_order = batch_header.total_order;  // CRITICAL!

	// [TEST4.1] Verify total_order is saved
	VLOG(1) << "[TEST4.1] SAVED total_order=" << batch_header.total_order
	        << " for batch_seq=" << batch_header.batch_seq
	        << " (broker_id=" << broker_id_ << ")";

	// Flush batch header to CXL
	CXL::flush_cacheline(&batch_header_log[batch_header.batch_seq]);
	CXL::store_fence();

	// Return replication callback
	return [this, batch_header, log, batch_header_location](void* log_ptr, size_t /*placeholder*/) {
		BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);

		// Keep header pointer for later use (replication tracking needs logical_offset)
		MessageHeader *header = reinterpret_cast<MessageHeader*>(log);

		// [[CORFU_ORDER3_FIX]] Wait for NetworkManager to set batch_complete flag
		// This is more reliable than polling next_msg_diff because NetworkManager
		// explicitly flushes batch_complete to CXL after writing all messages
		const int MAX_SPINS = 10000000;  // ~10ms timeout at 1GHz
		int spin_count = 0;

		while (spin_count < MAX_SPINS) {
			// Flush cacheline to see NetworkManager's batch_complete write
			CXL::flush_cacheline(batch_header_location);
			CXL::load_fence();

			if (__atomic_load_n(&batch_header_location->batch_complete, __ATOMIC_ACQUIRE) == 1) {
				break;  // Batch complete
			}

			CXL::cpu_pause();
			spin_count++;
		}

		if (__atomic_load_n(&batch_header_location->batch_complete, __ATOMIC_ACQUIRE) != 1) {
			LOG(ERROR) << "CORFU Order 3: Batch completion timeout after " << spin_count
			           << " spins (batch_seq=" << batch_header.batch_seq << ")";
			return;
		}

		// Handle replication if needed
		if (replication_factor_ > 0 && corfu_replication_client_) {
			corfu_replication_client_->ReplicateData(
				batch_header.log_idx,
				batch_header.total_size,
				log
			);

			// Mark replication done for all replicas
			size_t last_offset = header->logical_offset + batch_header.num_msg - 1;
			for (int i = 0; i < replication_factor_; i++) {
				int num_brokers = get_num_brokers_callback_();
				int b = (broker_id_ + num_brokers - i) % num_brokers;

				if (tinode_->replicate_tinode) {
					replica_tinode_->offsets[b].replication_done[broker_id_] = last_offset;
				}
				tinode_->offsets[b].replication_done[broker_id_] = last_offset;
			}

			// Flush replication_done updates to CXL
			CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(&tinode_->offsets[broker_id_].replication_done)));
			CXL::store_fence();
		}

		// Update tinode.ordered to track highest total_order replicated
		// FIXED: Use actual total_order range, not just count
		{
			absl::MutexLock lock(&mutex_);
			size_t batch_max_order = batch_header.total_order + batch_header.num_msg - 1;
			size_t current_ordered = __atomic_load_n(&tinode_->offsets[broker_id_].ordered, __ATOMIC_ACQUIRE);

			if (batch_max_order > current_ordered) {
				__atomic_store_n(&tinode_->offsets[broker_id_].ordered, batch_max_order, __ATOMIC_RELEASE);
			}
		}

		// Mark batch as ordered
		batch_header_log[batch_header.batch_seq].ordered = 1;
		CXL::flush_cacheline(&batch_header_log[batch_header.batch_seq]);
		CXL::store_fence();
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

	cached_num_brokers_ = get_num_brokers_callback_();
	size_t num_brokers = cached_num_brokers_;
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

	// [[P2.1]] Same batch_id scheme as ReservePBRSlotCore: (broker_id << 48) | pbr_absolute_index (no rdtsc).
	batch_header.pbr_absolute_index = broker_pbr_counters_[broker_id_].fetch_add(1, std::memory_order_relaxed);
	batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) | batch_header.pbr_absolute_index;

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
    
    batch_header.log_idx = scalog_batch_offset_.fetch_add(batch_header.total_size); 

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

		// [[FIX WRAP BUG]] BATCHHEADERS_SIZE = "consumer at logical 0"; treat as 0 for in_flight.
		size_t effective_consumed = (consumed_through == BATCHHEADERS_SIZE) ? 0 : consumed_through;
		size_t in_flight;
		if (approx_next_slot >= effective_consumed) {
			in_flight = approx_next_slot - effective_consumed;
		} else {
			in_flight = (BATCHHEADERS_SIZE - effective_consumed) + approx_next_slot;
		}
		// Add extra margin (2 slots) for approximation safety
		slot_free = (in_flight + sizeof(BatchHeader) * 2 < BATCHHEADERS_SIZE);

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

	// [[PERF Phase 1.1]] Lock-free BLog allocation: fetch_add is atomic; no mutex needed.
	// PBR allocation is handled separately in ReservePBRSlotAfterRecv(); only log_addr_ is updated here.
	// Design §3.1: wait-free broker; multi-threaded ingestion without mutex on hot path.
	// [[P1.2]] When replication=0 no O_DIRECT pwrite; use cache-line alignment to reduce padding.
	constexpr size_t kODirectAlign = 4096;
	constexpr size_t kCacheLineAlign = 64;
	size_t align = (replication_factor_ > 0) ? kODirectAlign : kCacheLineAlign;
	size_t alloc_size = (msg_size + align - 1) & ~(align - 1);
	log = reinterpret_cast<void*>(log_addr_.fetch_add(alloc_size));

	// Check for segment boundary (advisory LOG(ERROR); does not need atomicity with allocation)
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

// [[RECV_DIRECT_TO_CXL]] Lock-free BLog allocation (~10ns).
// [[PHASE_1A_EPOCH_FENCING]] Design §4.2.1: refuse writes if broker epoch is stale (zombie broker).
// [[Issue #3]] When epoch_already_checked true, skip epoch check (caller did CheckEpochOnce at batch start).
void* Topic::ReserveBLogSpace(size_t size, bool epoch_already_checked) {
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

size_t Topic::GetAndAdvanceOrder0LogicalOffset(uint32_t num_msg) {
	// Returns value before add = start logical offset for this batch; UpdateWrittenForOrder0 uses logical_offset + num_msg.
	return order0_next_logical_offset_.fetch_add(num_msg, std::memory_order_acq_rel);
}

void Topic::SetOrder0Written(size_t cumulative_logical_offset, size_t blog_offset, uint32_t num_msg) {
	if (num_msg == 0) return;
	static std::atomic<uint64_t> set_entry_count{0};
	uint64_t entry_n = set_entry_count.fetch_add(1, std::memory_order_relaxed) + 1;
	MessageHeader* first_msg = reinterpret_cast<MessageHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + blog_offset);
	// [[CXL_VISIBILITY]] Invalidate so we see NetworkManager's next_msg_diff/paddedSize writes
	CXL::flush_cacheline(CXL::ToFlushable(first_msg));
	CXL::load_fence();
	if (entry_n <= 5 || entry_n % 1000 == 0) {
		LOG(INFO) << "SetOrder0Written entry topic=" << topic_name_ << " broker=" << broker_id_
		          << " blog_offset=" << blog_offset << " num_msg=" << num_msg
		          << " first_msg paddedSize=" << first_msg->paddedSize << " next_msg_diff=" << first_msg->next_msg_diff << " call#=" << entry_n;
	}
	MessageHeader* last_msg = first_msg;
	for (uint32_t i = 0; i + 1 < num_msg; ++i) {
		size_t diff = last_msg->next_msg_diff;
		if (diff == 0) {
			diff = last_msg->paddedSize;
			if (diff == 0) {
				VLOG(2) << "SetOrder0Written early return topic=" << topic_name_ << " broker=" << broker_id_
				        << " at i=" << i << " paddedSize=0 next_msg_diff=0 blog_offset=" << blog_offset;
				return;
			}
		}
		last_msg = reinterpret_cast<MessageHeader*>(
			reinterpret_cast<uint8_t*>(last_msg) + diff);
	}
	absl::MutexLock lock(&written_mutex_);

	// [[FIX: ORDER0_FIRST_BATCH]] CRITICAL: Set chain head BEFORE early return check!
	// cumulative_logical_offset == num_msg means this is the batch starting at logical offset 0.
	// Must record its location regardless of completion order so GetMessageAddr can start the chain.
	if (cumulative_logical_offset == num_msg && order0_first_physical_addr_ == nullptr) {
		order0_first_physical_addr_ = reinterpret_cast<uint8_t*>(cxl_addr_) + blog_offset;
		VLOG(2) << "SetOrder0Written: Set order0_first_physical_addr_=" << (void*)order0_first_physical_addr_
		        << " topic=" << topic_name_ << " broker=" << broker_id_;
	}

	// Only advance written_*; batches can complete out of order (different connections/threads).
	// written_logical_offset_ == (size_t)-1 means "not yet set" - allow first update.
	const bool already_set = (written_logical_offset_ != static_cast<size_t>(-1));
	if (already_set && cumulative_logical_offset <= written_logical_offset_) {
		VLOG(2) << "SetOrder0Written skip (out-of-order) topic=" << topic_name_ << " broker=" << broker_id_
		        << " cumulative=" << cumulative_logical_offset << " written=" << written_logical_offset_;
		return;
	}
	written_logical_offset_ = cumulative_logical_offset;
	written_physical_addr_ = last_msg;
	VLOG(2) << "SetOrder0Written topic=" << topic_name_ << " broker=" << broker_id_
	        << " cumulative_offset=" << cumulative_logical_offset << " num_msg=" << num_msg;
}

void Topic::FinalizeOrder0WrittenIfNeeded() {
	size_t target = order0_next_logical_offset_.load(std::memory_order_acquire);
	absl::MutexLock lock(&written_mutex_);
	if (written_logical_offset_ == static_cast<size_t>(-1) || target <= written_logical_offset_ ||
	    written_physical_addr_ == nullptr) {
		VLOG(1) << "FinalizeOrder0WrittenIfNeeded topic=" << topic_name_ << " broker=" << broker_id_
		        << " skip (written=" << written_logical_offset_ << " target=" << target << ")";
		return;
	}
	size_t steps = target - written_logical_offset_;
	MessageHeader* cur = static_cast<MessageHeader*>(written_physical_addr_);
	CXL::flush_cacheline(CXL::ToFlushable(cur));
	CXL::load_fence();
	size_t s = 0;
	for (; s < steps && cur != nullptr; ++s) {
		size_t diff = cur->next_msg_diff;
		if (diff == 0) diff = cur->paddedSize;
		if (diff == 0) break;
		cur = reinterpret_cast<MessageHeader*>(reinterpret_cast<uint8_t*>(cur) + diff);
	}
	if (cur != nullptr && s == steps) {
		written_logical_offset_ = target;
		written_physical_addr_ = cur;
		LOG(INFO) << "FinalizeOrder0WrittenIfNeeded topic=" << topic_name_ << " broker=" << broker_id_
		          << " advanced written to " << target << " (steps=" << steps << ")";
	} else {
		LOG(WARNING) << "FinalizeOrder0WrittenIfNeeded topic=" << topic_name_ << " broker=" << broker_id_
		             << " walk stopped at step " << s << "/" << steps << " (cur=" << (void*)cur << ")";
	}
}

// [[ORDER_0_ACK_RACE_FIX]] Update last_batch_complete_offset when batch_complete=1 is set
void Topic::TrackBatchComplete(size_t logical_offset_end) {
	// Use atomic max to handle concurrent updates from multiple receive threads
	size_t current = last_batch_complete_offset_.load(std::memory_order_relaxed);
	while (logical_offset_end > current) {
		if (last_batch_complete_offset_.compare_exchange_weak(current, logical_offset_end,
		                                                       std::memory_order_release,
		                                                       std::memory_order_relaxed)) {
			break;
		}
	}
}

// [[ORDER_0_ACK_RACE_FIX]] Directly update written to last_batch_complete_offset without walking message chain
void Topic::UpdateWrittenToLastComplete() {
	size_t target = last_batch_complete_offset_.load(std::memory_order_acquire);
	if (target == 0) {
		VLOG(1) << "UpdateWrittenToLastComplete topic=" << topic_name_ << " broker=" << broker_id_
		        << " skip (no batches completed)";
		return;
	}

	// Update TInode written offset directly (no message chain walk needed!)
	TInode* tinode = tinode_;
	if (!tinode) {
		LOG(WARNING) << "UpdateWrittenToLastComplete topic=" << topic_name_ << " broker=" << broker_id_
		             << " skip (tinode is null)";
		return;
	}

	// Update written atomically (DelegationThread may also update concurrently)
	size_t current_written = tinode->offsets[broker_id_].written;
	if (target > current_written) {
		tinode->offsets[broker_id_].written = target;
		CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(&tinode->offsets[broker_id_].written)));
		CXL::store_fence();
		LOG(INFO) << "UpdateWrittenToLastComplete topic=" << topic_name_ << " broker=" << broker_id_
		          << " advanced written from " << current_written << " to " << target
		          << " (+" << (target - current_written) << " messages)";

		// [[B0_TAIL_ACK_FIX]] For Order 4/5, also advance CompletionVector so AckThread can send tail ACKs.
		// Normally Sequencer does this, but if it missed some tail batches (e.g. connection closed early),
		// we must advance it here to avoid client ACK timeout.
		if (ack_level_ == 1 && (order_ == 4 || order_ == 5) && seq_type_ == heartbeat_system::SequencerType::EMBARCADERO) {
			CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
			CompletionVectorEntry* entry = &cv[broker_id_];
			
			// Use atomic max to advance CV
			uint64_t cur = entry->completed_logical_offset.load(std::memory_order_acquire);
			while (target > cur) {
				if (entry->completed_logical_offset.compare_exchange_weak(cur, target, std::memory_order_release)) {
					CXL::flush_cacheline(entry);
					CXL::store_fence();
					LOG(INFO) << "UpdateWrittenToLastComplete topic=" << topic_name_ << " broker=" << broker_id_
					          << " also advanced CV from " << cur << " to " << target;
					break;
				}
				// cur is updated by compare_exchange_weak on failure
			}
		}
	} else {
		VLOG(1) << "UpdateWrittenToLastComplete topic=" << topic_name_ << " broker=" << broker_id_
		        << " skip (written=" << current_written << " already >= target=" << target << ")";
	}
}

int Topic::GetPBRUtilizationPct() {
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
	// [[FIX WRAP BUG]] Treat BATCHHEADERS_SIZE as 0 for in_flight (consistent with allocation paths).
	size_t effective_consumed = (consumed == BATCHHEADERS_SIZE) ? 0 : consumed;
	size_t in_flight;
	if (next_slot_offset >= effective_consumed) {
		in_flight = next_slot_offset - effective_consumed;
	} else {
		in_flight = (BATCHHEADERS_SIZE - effective_consumed) + next_slot_offset;
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

	// [[PERF Phase 1.2]] One CXL read before CAS loop; avoid 200–500ns flush+fence on every retry.
	// CAS failure = contention (another thread took a slot), not ring-full; no need to re-read CXL.
	RefreshPBRConsumedThroughCache();

	PBRProducerState current = pbr_state_.load(std::memory_order_acquire);
	PBRProducerState next;
	do {
		uint64_t consumed_seq = cached_consumed_seq_.load(std::memory_order_acquire);
		uint64_t in_flight = (current.next_slot_seq >= consumed_seq)
			? (current.next_slot_seq - consumed_seq) : 0;

		if (in_flight >= num_slots_ - 1) {
			// Ring appears full — refresh from CXL to check if consumer advanced
			RefreshPBRConsumedThroughCache();
			consumed_seq = cached_consumed_seq_.load(std::memory_order_acquire);
			in_flight = (current.next_slot_seq >= consumed_seq)
				? (current.next_slot_seq - consumed_seq) : 0;
			if (in_flight >= num_slots_ - 1)
				return false;  // Genuinely full
		}

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

bool Topic::ReservePBRSlotCore(BatchHeader& batch_header, void* log, bool epoch_already_checked,
		void*& batch_headers_log, size_t& logical_offset, void*& segment_header) {
	if (!first_batch_headers_addr_) return false;

	uint64_t current_epoch;
	if (!epoch_already_checked) {
		uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
		bool force_full_read = (n % kEpochCheckInterval == 0);
		auto [epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
		if (was_stale) return false;
		current_epoch = epoch;
		cached_epoch_.store(epoch, std::memory_order_release);
	} else {
		current_epoch = last_checked_epoch_.load(std::memory_order_acquire);
	}

	const unsigned long long int segment_metadata = reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;

	if (use_lock_free_pbr_) {
		size_t byte_offset;
		if (!ReservePBRSlotLockFree(batch_header.num_msg, byte_offset, logical_offset))
			return false;
		batch_headers_log = reinterpret_cast<void*>(
			reinterpret_cast<uintptr_t>(first_batch_headers_addr_) + byte_offset);
	} else {
		uint64_t n = pbr_cache_refresh_counter_.fetch_add(1, std::memory_order_relaxed);
		if (n % kPBRCacheRefreshInterval == 0)
			RefreshPBRConsumedThroughCache();
		const unsigned long long int batch_headers_start =
			reinterpret_cast<unsigned long long int>(first_batch_headers_addr_);
		const unsigned long long int batch_headers_end = batch_headers_start + BATCHHEADERS_SIZE;
		{
			absl::MutexLock lock(&mutex_);
			size_t next_slot_offset = static_cast<size_t>(batch_headers_ - batch_headers_start);
			size_t consumed_through = cached_pbr_consumed_through_.load(std::memory_order_acquire);
			size_t effective_consumed = (consumed_through == BATCHHEADERS_SIZE) ? 0 : consumed_through;
			size_t in_flight;
			if (next_slot_offset >= effective_consumed)
				in_flight = next_slot_offset - effective_consumed;
			else
				in_flight = (BATCHHEADERS_SIZE - effective_consumed) + next_slot_offset;
			if (in_flight + sizeof(BatchHeader) >= BATCHHEADERS_SIZE)
				return false;
			batch_headers_log = reinterpret_cast<void*>(batch_headers_);
			batch_headers_ += sizeof(BatchHeader);
			if (batch_headers_ >= batch_headers_end)
				batch_headers_ = batch_headers_start;
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
	batch_header.pbr_absolute_index = broker_pbr_counters_[broker_id_].fetch_add(1, std::memory_order_relaxed);
	batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) | batch_header.pbr_absolute_index;
	batch_header.epoch_created = static_cast<uint16_t>(std::min(current_epoch, static_cast<uint64_t>(0xFFFF)));
	batch_header.log_idx = static_cast<size_t>(
		reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_));
	return true;
}

bool Topic::ReservePBRSlotAfterRecv(BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	void* batch_headers_log;
	if (!ReservePBRSlotCore(batch_header, log, epoch_already_checked, batch_headers_log, logical_offset, segment_header)) {
		batch_header_location = nullptr;
		return false;
	}
	batch_header.batch_complete = 0;
	batch_header.flags = kBatchHeaderFlagValid;

	// [[P0.1]] Minimal slot init: scanner gates on VALID && num_msg>0. Set CLAIMED only; no full memset.
	BatchHeader* slot = reinterpret_cast<BatchHeader*>(batch_headers_log);
	__atomic_store_n(&slot->flags, kBatchHeaderFlagClaimed, __ATOMIC_RELEASE);
	__atomic_store_n(&slot->batch_complete, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&slot->num_msg, 0, __ATOMIC_RELEASE);
	CXL::flush_cacheline(batch_headers_log);
	CXL::store_fence();
	if (broker_id_ == 0 && kEnableDiagnostics) {
		static std::atomic<uint64_t> b0_pbr_claimed_log{0};
		uint64_t n = b0_pbr_claimed_log.fetch_add(1, std::memory_order_relaxed);
		size_t slot_off = reinterpret_cast<uint8_t*>(batch_headers_log) - reinterpret_cast<uint8_t*>(first_batch_headers_addr_);
		if (n < 10 || (n % 5000 == 0 && n > 0))
			LOG(INFO) << "[B0_PBR_WRITE] slot_offset=" << slot_off << " flags=CLAIMED written";
	}
	batch_header_location = reinterpret_cast<BatchHeader*>(batch_headers_log);
	return true;
}

bool Topic::PublishPBRSlotAfterRecv(const BatchHeader& batch_header, BatchHeader* batch_header_location) {
	if (!batch_header_location) return false;

	BatchHeader published = batch_header;
	// Keep CLAIMED set after publish so scanners never misclassify a published slot as empty tail
	// if num_msg visibility lags on non-coherent CXL.
	published.flags |= kBatchHeaderFlagClaimed;
	published.flags |= kBatchHeaderFlagValid;
	memcpy(batch_header_location, &published, sizeof(BatchHeader));
	__atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE);

	// BatchHeader is 128B; flush both cachelines for non-coherent CXL visibility.
	CXL::flush_cacheline(batch_header_location);
	const void* batch_header_next_line = reinterpret_cast<const void*>(
		reinterpret_cast<const uint8_t*>(batch_header_location) + 64);
	CXL::flush_cacheline(batch_header_next_line);
	CXL::store_fence();
	return true;
}

bool Topic::ReservePBRSlotAndWriteEntry(BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	void* batch_headers_log;
	if (!ReservePBRSlotCore(batch_header, log, epoch_already_checked, batch_headers_log, logical_offset, segment_header)) {
		batch_header_location = nullptr;
		return false;
	}
	batch_header.flags = kBatchHeaderFlagValid;
	// [[Issue #4]] Write batch header but do NOT flush here; sequencer must not see it until batch_complete=1.
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
	if (num_slots_ == 0) return false;
	BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(start_batch_header) + BATCHHEADERS_SIZE);
	// [[RING_WRAP]] expected_batch_offset can grow without bound; wrap to slot index
	size_t slot_index = expected_batch_offset % num_slots_;
	BatchHeader* header = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * slot_index);
	// [[CXL_NON_COHERENT]] Invalidate before reading ordered so export sees sequencer writes
	CXL::flush_cacheline(header);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(header) + 64);
	CXL::full_fence();  // MFENCE required: CLFLUSHOPT only ordered by MFENCE (Intel SDM §8.2.5)
	if (header->ordered == 0) {
		return false;
	}
	size_t off = header->batch_off_to_export;
	// [[BOUNDS]] Validate batch_off_to_export (matches disk_manager.cc pattern)
	if (off >= BATCHHEADERS_SIZE || off % sizeof(BatchHeader) != 0) {
		LOG(WARNING) << "[GetBatchToExport B" << broker_id_ << "] Invalid batch_off_to_export=" << off;
		expected_batch_offset++;
		return false;
	}
	BatchHeader* actual = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header) + off);
	if (actual < start_batch_header || actual >= ring_end) {
		LOG(WARNING) << "[GetBatchToExport B" << broker_id_ << "] Export chain outside ring (off=" << off << ")";
		expected_batch_offset++;
		return false;
	}
	// [[CXL_NON_COHERENT]] Invalidate actual batch so we see sequencer-written total_size/log_idx
	CXL::flush_cacheline(actual);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(actual) + 64);
	CXL::full_fence();  // MFENCE required for CLFLUSHOPT ordering
	header = actual;
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
	// [[EXPORT_BY_TOTAL_ORDER]] [PHASE-8] Unbounded queue peek (mutex protected)
	bool have_hold = false;
	size_t hold_total_order = 0;
	OrderedHoldExportEntry hold_front;
	{
		absl::MutexLock lock(&hold_export_queues_[broker_id_].mu);
		if (!hold_export_queues_[broker_id_].q.empty()) {
			hold_front = hold_export_queues_[broker_id_].q.front();
			have_hold = true;
			hold_total_order = hold_front.total_order;
		}
	}

	// [[PHASE_2_CV_EXPORT]] O(1) export: use CompletionVector instead of linear scan (design §3.4).
	// expected_batch_offset = next PBR absolute index to export (monotonic).
	bool have_ring = false;
	size_t ring_total_order = 0;
	void* ring_batch_addr = nullptr;
	size_t ring_batch_size = 0;
	uint32_t ring_num_messages = 0;
	size_t next_pbr = expected_batch_offset;

	// [[EXPORT_DIAG]] Log struct layout once per process to verify CV/ring addressing
	{
		static bool layout_logged = false;
		if (!layout_logged) {
			size_t bh_off = tinode_->offsets[broker_id_].batch_headers_offset;
			LOG(ERROR) << "[STRUCT_LAYOUT B" << broker_id_ << "]"
			           << " sizeof(CompletionVectorEntry)=" << sizeof(CompletionVectorEntry)
			           << " offsetof(completed_pbr_head)=" << offsetof(CompletionVectorEntry, completed_pbr_head)
			           << " offsetof(completed_logical_offset)=" << offsetof(CompletionVectorEntry, completed_logical_offset)
			           << " sizeof(BatchHeader)=" << sizeof(BatchHeader)
			           << " kCompletionVectorOffset=" << kCompletionVectorOffset
			           << " cxl_addr_=" << cxl_addr_
			           << " batch_headers_offset=" << bh_off;
			layout_logged = true;
		}
	}

	if (num_slots_ > 0 && cxl_addr_) {
		CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
		CompletionVectorEntry* my_cv = &cv[broker_id_];
		CXL::flush_cacheline(my_cv);
		CXL::full_fence();  // MFENCE orders CLFLUSHOPT before loads (per Intel SDM §11.12)
		uint64_t completed_pbr_head = my_cv->completed_pbr_head.load(std::memory_order_acquire);
		constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);

		if (completed_pbr_head != kNoProgress && next_pbr <= completed_pbr_head) {
			size_t slot = next_pbr % num_slots_;
			BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
			BatchHeader* header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * slot);
			CXL::flush_cacheline(header);
			CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(header) + 64);
			CXL::full_fence();  // MFENCE required: CLFLUSHOPT only ordered by MFENCE (Intel SDM §8.2.5)
			if (header->ordered == 1 && header->pbr_absolute_index == next_pbr) {
				// Resolve export chain (batch_off_to_export 0 = in-place)
				BatchHeader* actual = header;
				if (header->batch_off_to_export != 0) {
					size_t off = header->batch_off_to_export;
					if (off >= BATCHHEADERS_SIZE || off % sizeof(BatchHeader) != 0) {
						VLOG(1) << "[GetBatchToExportWithMetadata B" << broker_id_ << "] Invalid batch_off_to_export=" << off;
					} else {
						BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
							reinterpret_cast<uint8_t*>(start_batch_header) + BATCHHEADERS_SIZE);
						actual = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header) + off);
						if (actual >= start_batch_header && actual < ring_end) {
							CXL::flush_cacheline(actual);
							CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(actual) + 64);
							CXL::full_fence();  // MFENCE required for CLFLUSHOPT ordering
						} else {
							actual = header;
						}
					}
				}
				ring_total_order = actual->total_order;

				// [TEST4.1] Verify total_order is read correctly
				VLOG(1) << "[TEST4.1] READ total_order=" << ring_total_order
				        << " from batch_seq=" << actual->batch_seq;
				ring_batch_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + actual->log_idx;
				ring_batch_size = actual->total_size;
				ring_num_messages = actual->num_msg;
				have_ring = true;
			}
		}
	}

	// Return batch with smaller total_order (or only available source)
	if (have_hold && have_ring) {
		if (hold_total_order <= ring_total_order) {
			{
				absl::MutexLock lock(&hold_export_queues_[broker_id_].mu);
				if (!hold_export_queues_[broker_id_].q.empty()) {
					hold_export_queues_[broker_id_].q.pop_front();
				}
			}
			batch_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + hold_front.log_idx;
			batch_size = hold_front.batch_size;
			batch_total_order = hold_front.total_order;
			num_messages = hold_front.num_messages;
			return true;
		}
		batch_addr = ring_batch_addr;
		batch_size = ring_batch_size;
		batch_total_order = ring_total_order;
		num_messages = ring_num_messages;
		expected_batch_offset = next_pbr + 1;
		return true;
	}
	if (have_hold) {
		{
			absl::MutexLock lock(&hold_export_queues_[broker_id_].mu);
			if (!hold_export_queues_[broker_id_].q.empty()) {
				hold_export_queues_[broker_id_].q.pop_front();
			}
		}
		batch_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + hold_front.log_idx;
		batch_size = hold_front.batch_size;
		batch_total_order = hold_front.total_order;
		num_messages = hold_front.num_messages;
		return true;
	}
	if (have_ring) {
		batch_addr = ring_batch_addr;
		batch_size = ring_batch_size;
		batch_total_order = ring_total_order;
		num_messages = ring_num_messages;
		expected_batch_offset = next_pbr + 1;
		return true;
	}

	// [[EXPORT_DIAG]] Log why export failed every ~2s to identify CV/slot/tinode mismatch on non-head brokers
	{
		static thread_local uint64_t diag_fail_count = 0;
		static thread_local auto last_diag_time = std::chrono::steady_clock::now();
		diag_fail_count++;

		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_diag_time).count() >= 2) {
			CompletionVectorEntry* cv_diag = reinterpret_cast<CompletionVectorEntry*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
			CompletionVectorEntry* my_cv_diag = &cv_diag[broker_id_];
			CXL::flush_cacheline(my_cv_diag);
			CXL::full_fence();
			uint64_t diag_pbr_head = my_cv_diag->completed_pbr_head.load(std::memory_order_acquire);
			uint64_t diag_logical = my_cv_diag->completed_logical_offset.load(std::memory_order_acquire);

			uint64_t diag_ordered = 0;
			uint64_t diag_pbr_idx = 0;
			size_t diag_total_size = 0;
			uint32_t diag_flags = 0;
			uint32_t diag_num_msg = 0;
			size_t diag_off_to_export = 0;
			if (num_slots_ > 0 && cxl_addr_) {
				size_t slot = expected_batch_offset % num_slots_;
				BatchHeader* start_bh = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
				BatchHeader* hdr_diag = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(start_bh) + sizeof(BatchHeader) * slot);
				CXL::flush_cacheline(hdr_diag);
				CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(hdr_diag) + 64);
				CXL::full_fence();
				diag_ordered = hdr_diag->ordered;
				diag_pbr_idx = hdr_diag->pbr_absolute_index;
				diag_total_size = hdr_diag->total_size;
				diag_flags = hdr_diag->flags;
				diag_num_msg = hdr_diag->num_msg;
				diag_off_to_export = hdr_diag->batch_off_to_export;
			}

			CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(&tinode_->offsets[broker_id_])));
			CXL::full_fence();
			size_t diag_tinode_ordered = tinode_->offsets[broker_id_].ordered;

			LOG(ERROR) << "[EXPORT_DIAG B" << broker_id_ << "]"
			           << " fails=" << diag_fail_count
			           << " next_pbr=" << expected_batch_offset
			           << " | CV: pbr_head=" << diag_pbr_head
			           << " logical_offset=" << diag_logical
			           << " | SLOT: ordered=" << diag_ordered
			           << " pbr_abs_idx=" << diag_pbr_idx
			           << " flags=" << diag_flags
			           << " num_msg=" << diag_num_msg
			           << " total_size=" << diag_total_size
			           << " off_to_export=" << diag_off_to_export
			           << " | TINODE: ordered=" << diag_tinode_ordered
			           << " | num_slots=" << num_slots_
			           << " kNoProgress=" << static_cast<uint64_t>(-1);

			last_diag_time = now;
			diag_fail_count = 0;
		}
	}

	return false;
}

void Topic::AdvanceCVForSequencer(uint16_t broker_id, uint64_t pbr_index, uint64_t cumulative_msg_count) {
	// [[PHASE_2_CV_EXPORT]] ack_level=1: sequencer advances CV so export can proceed without waiting for replication.
	// For ack_level=2 the tail replica also advances the SAME CV entry after replicating (chain_replication.cc).
	// [[B0_ACK_FIX]] Advance on cumulative_msg_count (ACK offset), not pbr_index. Late-arriving batches
	// (e.g. seq=0 after seq=1,2 expired) have lower pbr_index but must still advance ACK so broker sees progress.
	// completed_pbr_head remains max-only for export ordering; completed_logical_offset is the ACK source of truth.
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	CompletionVectorEntry* entry = &cv[broker_id];
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
	// Advance logical offset (ACK) when this batch extends the cumulative count
	for (;;) {
		uint64_t cur_offset = entry->completed_logical_offset.load(std::memory_order_acquire);
		if (cumulative_msg_count <= cur_offset) break;
		if (entry->completed_logical_offset.compare_exchange_strong(cur_offset, cumulative_msg_count, std::memory_order_release)) {
			// Optionally advance pbr_head for export (monotonic; don't regress)
			uint64_t cur_pbr = entry->completed_pbr_head.load(std::memory_order_acquire);
			if (cur_pbr == kNoProgress || pbr_index > cur_pbr) {
				entry->completed_pbr_head.store(pbr_index, std::memory_order_release);
			}
			CXL::flush_cacheline(entry);
			CXL::store_fence();
			break;
		}
	}
}

void Topic::AccumulateCVUpdate(
		uint16_t broker_id,
		uint64_t pbr_index,
		uint64_t cumulative_msg_count,
		std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
		std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index) {
	if (broker_id >= NUM_MAX_BROKERS) return;
	if (cumulative_msg_count > max_cumulative[broker_id]) {
		max_cumulative[broker_id] = cumulative_msg_count;
	}
	if (pbr_index > max_pbr_index[broker_id]) {
		max_pbr_index[broker_id] = pbr_index;
	}
}

void Topic::FlushAccumulatedCV(
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index) {
	// [PHASE-3] O(brokers) CXL writes + 1 fence (was O(batches) fences)
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);

	for (int broker_id = 0; broker_id < NUM_MAX_BROKERS; ++broker_id) {
		uint64_t cumulative = max_cumulative[broker_id];
		if (cumulative == 0) continue;

		CompletionVectorEntry* entry = &cv[broker_id];

		// [PANEL C3/R3] On non-coherent CXL, relaxed load can read from local cache (stale).
		// Invalidate + load_fence before reading so we see the last flushed value (our own or tail's).
		CXL::flush_cacheline(entry);
		CXL::load_fence();

		if (ack_level_ <= 1) {
			// Sequencer is the ONLY CV writer for ack_level <= 1. Store if we advance.
			uint64_t cur = entry->completed_logical_offset.load(std::memory_order_acquire);
			if (cumulative > cur) {
				entry->completed_logical_offset.store(cumulative, std::memory_order_release);
			}
		} else {
			// ack_level == 2: tail replica also writes CV. Use CAS.
			for (;;) {
				uint64_t cur = entry->completed_logical_offset.load(std::memory_order_acquire);
				if (cumulative <= cur) break;
				if (entry->completed_logical_offset.compare_exchange_strong(
						cur, cumulative, std::memory_order_release)) break;
			}
		}

		uint64_t pbr_val = max_pbr_index[broker_id];
		uint64_t cur_pbr = entry->completed_pbr_head.load(std::memory_order_acquire);
		if (cur_pbr == kNoProgress || pbr_val > cur_pbr) {
			entry->completed_pbr_head.store(pbr_val, std::memory_order_release);
		}

		CXL::flush_cacheline(entry);
	}
	CXL::store_fence();
}

void Topic::ResetCompletedRangeQueue() {
	completed_ranges_head_.store(0, std::memory_order_release);
	completed_ranges_tail_.store(0, std::memory_order_release);
	completed_ranges_enqueue_retries_.store(0, std::memory_order_release);
	completed_ranges_enqueue_wait_ns_.store(0, std::memory_order_release);
	completed_ranges_max_depth_.store(0, std::memory_order_release);
	committed_updater_pending_peak_.store(0, std::memory_order_release);
	for (auto& slot : completed_ranges_ring_) {
		slot.ready.store(false, std::memory_order_relaxed);
	}
}

void Topic::EnqueueCompletedRange(uint64_t start, uint64_t end) {
	if (end <= start) return;
	auto wait_start = std::chrono::steady_clock::now();
	uint64_t spins = 0;
	while (!stop_threads_) {
		uint64_t tail = completed_ranges_tail_.load(std::memory_order_relaxed);
		uint64_t head = completed_ranges_head_.load(std::memory_order_acquire);
		uint64_t depth = tail - head;
		uint64_t prev_peak = completed_ranges_max_depth_.load(std::memory_order_relaxed);
		while (depth > prev_peak &&
		       !completed_ranges_max_depth_.compare_exchange_weak(prev_peak, depth, std::memory_order_relaxed)) {
		}
		if (depth < kCompletedRangesRingCap) {
			if (completed_ranges_tail_.compare_exchange_weak(
					tail, tail + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
				CompletedRangeSlot& slot = completed_ranges_ring_[tail & kCompletedRangesRingMask];
				slot.data.start = start;
				slot.data.end = end;
				slot.ready.store(true, std::memory_order_release);
				committed_seq_updater_cv_.notify_one();
				if (spins > 0) {
					auto waited_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - wait_start).count();
					completed_ranges_enqueue_wait_ns_.fetch_add(static_cast<uint64_t>(waited_ns), std::memory_order_relaxed);
				}
				return;
			}
		} else {
			completed_ranges_enqueue_retries_.fetch_add(1, std::memory_order_relaxed);
			++spins;
			if ((spins & 0x3F) == 0) {
				std::this_thread::yield();
			} else {
				std::this_thread::sleep_for(std::chrono::microseconds(1));
			}
		}
	}
}

void Topic::CommittedSeqUpdaterThread() {
	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::flush_cacheline(control_block);
	CXL::load_fence();
	uint64_t cur = control_block->committed_seq.load(std::memory_order_acquire);
	uint64_t next_expected_start = (cur == UINT64_MAX) ? 0 : (cur + 1);

	std::priority_queue<CompletedRange, std::vector<CompletedRange>, std::greater<CompletedRange>> pending;
	uint64_t idle_spins = 0;

	while (true) {
		bool drained_any = false;
		while (true) {
			uint64_t head = completed_ranges_head_.load(std::memory_order_relaxed);
			uint64_t tail = completed_ranges_tail_.load(std::memory_order_acquire);
			if (head == tail) break;
			CompletedRangeSlot& slot = completed_ranges_ring_[head & kCompletedRangesRingMask];
			if (!slot.ready.load(std::memory_order_acquire)) break;
			pending.push(slot.data);
			slot.ready.store(false, std::memory_order_release);
			completed_ranges_head_.store(head + 1, std::memory_order_release);
			drained_any = true;
		}
		if (drained_any) {
			uint64_t pending_sz = static_cast<uint64_t>(pending.size());
			uint64_t prev_peak = committed_updater_pending_peak_.load(std::memory_order_relaxed);
			while (pending_sz > prev_peak &&
			       !committed_updater_pending_peak_.compare_exchange_weak(prev_peak, pending_sz, std::memory_order_relaxed)) {
			}
		}

		bool advanced = false;
		while (!pending.empty()) {
			const CompletedRange r = pending.top();
			if (r.end <= next_expected_start) {
				// Fully stale/duplicate range.
				pending.pop();
				continue;
			}
			if (r.start <= next_expected_start) {
				// Overlap or exact continuation.
				next_expected_start = r.end;
				pending.pop();
				advanced = true;
				continue;
			}
			// True gap at the head: cannot advance further yet.
			break;
		}

		if (advanced && next_expected_start > 0) {
			uint64_t new_committed = next_expected_start - 1;
			control_block->committed_seq.store(new_committed, std::memory_order_release);
			CXL::flush_cacheline(control_block);
			CXL::store_fence();
			idle_spins = 0;
		}

		if (committed_seq_updater_stop_.load(std::memory_order_acquire)) {
			uint64_t head = completed_ranges_head_.load(std::memory_order_acquire);
			uint64_t tail = completed_ranges_tail_.load(std::memory_order_acquire);
			if (head == tail && pending.empty()) break;
		}

		if (!advanced && !drained_any) {
			if (++idle_spins < 64) {
				std::this_thread::yield();
			} else {
				std::unique_lock<std::mutex> lock(committed_seq_updater_mu_);
				committed_seq_updater_cv_.wait_for(lock, std::chrono::microseconds(200), [this] {
					return committed_seq_updater_stop_.load(std::memory_order_acquire) ||
					       (completed_ranges_head_.load(std::memory_order_acquire) !=
					        completed_ranges_tail_.load(std::memory_order_acquire));
				});
				idle_spins = 0;
			}
		}
	}
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
	} else {  // order_ == 0
		// [[FIX: Read from TInode (CXL shared memory), not stale class members]]
		// This matches GetOffsetToAck's approach (single source of truth)
		volatile size_t* written_ptr = &tinode_->offsets[broker_id_].written;
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(written_ptr)));
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(&tinode_->offsets[broker_id_].written_addr)));
		CXL::full_fence();  // Ensure flush completes before reads

		combined_offset = tinode_->offsets[broker_id_].written;
		combined_addr = reinterpret_cast<void*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + static_cast<size_t>(tinode_->offsets[broker_id_].written_addr));
	}

	// Check if we have new messages. For Order 0, last_offset=(size_t)-1 means unset (publisher sent sentinel); ignore for this check.
	if (combined_offset == static_cast<size_t>(-1) ||
			(last_addr != nullptr && last_offset != static_cast<size_t>(-1) && combined_offset <= last_offset)) {
		static std::atomic<uint64_t> order0_false_count{0};
		uint64_t n = order0_false_count.fetch_add(1, std::memory_order_relaxed) + 1;
		const bool is_broker0 = (broker_id_ == 0);
		if (combined_offset == static_cast<size_t>(-1)) {
			// Broker 0: log first 20 then every 100k to diagnose head broker "no data yet"
			if (n <= 10 || n % 10000 == 0 || (is_broker0 && (n <= 20 || n % 100000 == 0))) {
				LOG(INFO) << "GetMessageAddr order=0 topic=" << topic_name_ << " broker=" << broker_id_
				          << " return false: no data yet (written_logical_offset_=-1) call#=" << n;
			}
		} else {
			// Subscriber caught up: broker has sent all messages for this connection.
			// [[STALL_DIAG]] Per-broker caught_up count and expected_tail to detect race (batches in-flight).
			static std::array<std::atomic<uint64_t>, NUM_MAX_BROKERS> per_broker_caught_up{};
			uint64_t bn = (static_cast<size_t>(broker_id_) < per_broker_caught_up.size())
				? per_broker_caught_up[broker_id_].fetch_add(1, std::memory_order_relaxed) + 1 : 0;
			size_t expected_tail = order0_next_logical_offset_.load(std::memory_order_acquire);
			size_t gap = (expected_tail > combined_offset) ? (expected_tail - combined_offset) : 0;
			if (bn <= 20 || bn % 10000 == 0) {
				LOG(INFO) << "GetMessageAddr [B" << broker_id_ << "] caught_up: "
				          << "combined_offset=" << combined_offset << " last_offset=" << last_offset
				          << " expected_tail=" << expected_tail << " gap=" << gap << " (call#=" << bn << ")";
			}
			static std::atomic<uint32_t> caught_up_log_count{0};
			uint32_t cu = caught_up_log_count.fetch_add(1, std::memory_order_relaxed);
			uint32_t b0_cu = is_broker0 ? bn : 0;
			if (cu < 4 || (is_broker0 && (b0_cu <= 10 || b0_cu % 50000 == 0))) {
				LOG(INFO) << "Broker " << broker_id_ << " topic=" << topic_name_
				          << ": sent all messages to subscriber (caught up at written_offset=" << combined_offset << ")";
			}
		}
		return false;
	}

	// Find start message location
	MessageHeader* start_msg_header;

	if (last_addr != nullptr) {
		start_msg_header = static_cast<MessageHeader*>(last_addr);

		// [[CXL_VISIBILITY]] Invalidate cache before reading next_msg_diff (writer may be in same process, different core)
		CXL::flush_cacheline(CXL::ToFlushable(start_msg_header));
		CXL::load_fence();

		// Wait for message to be combined if necessary
		while (start_msg_header->next_msg_diff == 0) {
			CXL::flush_cacheline(CXL::ToFlushable(start_msg_header));
			CXL::load_fence();
			std::this_thread::yield();
		}

		// Move to next message
		start_msg_header = reinterpret_cast<MessageHeader*>(
				reinterpret_cast<uint8_t*>(start_msg_header) + start_msg_header->next_msg_diff
				);
	} else {
		// Start from first message.
		if (order_ == 0) {
			// [[FIX: BUG_B]] Use first_message_addr_ (set at Topic construction from TInode)
			// instead of order0_first_physical_addr_ (set by removed SetOrder0Written call).
			if (first_message_addr_ == nullptr) {
				VLOG(2) << "GetMessageAddr: Order 0 chain head not ready (first_message_addr_=nullptr) ";
				return false;
			}
			start_msg_header = static_cast<MessageHeader*>(first_message_addr_);
		} else {
			if (combined_addr <= last_addr) {
				LOG(ERROR) << "GetMessageAddr: Invalid address relationship";
				return false;
			}
			start_msg_header = static_cast<MessageHeader*>(first_message_addr_);
		}
	}

	// [[CXL_VISIBILITY_FIX]] CRITICAL: Invalidate cache before reading paddedSize.
	// Even on same-process (broker 0), different CPU cores may have stale cache for CXL memory.
	// Without this, paddedSize can read as 0 and GetMessageAddr returns false → broker sends nothing.
	CXL::flush_cacheline(CXL::ToFlushable(start_msg_header));
	CXL::load_fence();

	// Verify message is valid. If paddedSize is 0 we must skip and advance last_* so we don't
	// permanently stall (next call would hit the same message and return false again).
	// [[B0_PADDED_SIZE_ZERO]] Re-read after a second flush; writer may have just written (same process).
	if (start_msg_header->paddedSize == 0) {
		CXL::flush_cacheline(CXL::ToFlushable(start_msg_header));
		CXL::load_fence();
		if (start_msg_header->paddedSize != 0) {
			// Re-read saw valid size; continue with normal path
			messages = static_cast<void*>(start_msg_header);
		} else {
			VLOG(2) << "GetMessageAddr: paddedSize=0 at start_msg_header=" << (void*)start_msg_header
			        << " broker=" << broker_id_ << " topic=" << topic_name_ << " (skipping after re-read)";
			size_t skip = start_msg_header->next_msg_diff;
			if (skip == 0) skip = sizeof(MessageHeader) + 64;  // minimal stride fallback
			MessageHeader* next_msg = reinterpret_cast<MessageHeader*>(
				reinterpret_cast<uint8_t*>(start_msg_header) + skip);
			if (next_msg <= combined_addr) {
				last_addr = next_msg;
				last_offset = (combined_offset > 0) ? (combined_offset - 1) : 0;
			} else {
				// Skip would go past tail; mark as caught up.
				last_addr = combined_addr;
				last_offset = combined_offset;
			}
			return false;
		}
	} else {
		// Set output message pointer
		messages = static_cast<void*>(start_msg_header);
	}

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
			// [[CXL_VISIBILITY]] Ensure we see writer's logical_offset (completion runs on receive path).
			CXL::flush_cacheline(CXL::ToFlushable(stop_at));
			CXL::load_fence();
			last_offset = stop_at->logical_offset;
			// [[FIX: FOLLOWER_CAUGHT_UP]] Never set last_offset=combined_offset in cap path when
			// logical_offset is -1 (stale cache or unset). That falsely marks "caught up" and
			// stops sending after ~12 MB. Use conservative value so next call returns more.
			if (last_offset == static_cast<size_t>(-1))
				last_offset = (combined_offset > 0) ? (combined_offset - 1) : 0;
			last_addr = stop_at;
			messages_size = reinterpret_cast<uint8_t*>(stop_at) -
				reinterpret_cast<uint8_t*>(start_msg_header) + stop_at->paddedSize;
		} else {
			CXL::flush_cacheline(CXL::ToFlushable(combined_addr));
			CXL::load_fence();
			last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
			if (last_offset == static_cast<size_t>(-1))
				last_offset = (combined_offset > 0) ? (combined_offset - 1) : 0;
			last_addr = combined_addr;
			messages_size = full_size;
		}
	} else {
		CXL::flush_cacheline(CXL::ToFlushable(combined_addr));
		CXL::load_fence();
		last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
		if (order_ == 0 && last_offset == static_cast<size_t>(-1))
			last_offset = (combined_offset > 0) ? (combined_offset - 1) : 0;
		last_addr = combined_addr;
		messages_size = full_size;
	}
#endif


	return true;
}
// Sequencer 5: Batch-level sequencer (Phase 1b: epoch pipeline + Level 5 hold buffer)
void Topic::InitExportCursorForBroker(int broker_id) {
	if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS) return;
	BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
	absl::MutexLock lock(&export_cursor_mu_);
	export_cursor_by_broker_[broker_id] = ring_start;
}

void Topic::Sequencer5() {
	LOG(INFO) << "Starting Sequencer5 (Phase 1b epoch pipeline) for topic: " << topic_name_;
	ResetCompletedRangeQueue();
	global_batch_seq_.store(0, std::memory_order_release);
	{
		ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
		control_block->committed_seq.store(UINT64_MAX, std::memory_order_release);
		CXL::flush_cacheline(control_block);
		CXL::store_fence();
	}
	committed_seq_updater_stop_.store(false, std::memory_order_release);
	committed_seq_updater_thread_ = std::thread(&Topic::CommittedSeqUpdaterThread, this);
	InitLevel5Shards();
	absl::btree_set<int> registered_brokers;
	GetRegisteredBrokerSet(registered_brokers);

	// Epoch increment on sequencer start (§4.2)
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
	epoch_driver_done_ = false;  // Non-atomic assignment
	current_epoch_for_hold_.store(0, std::memory_order_release);
	for (int i = 0; i < 3; i++) {
		epoch_buffers_[i].state.store(EpochBuffer5::State::IDLE, std::memory_order_relaxed);
	}
	epoch_buffers_[0].reset_and_start();

	// Init per-broker export chain cursors (array: O(1) access in commit path)
	export_cursor_by_broker_.fill(nullptr);
	for (int broker_id : registered_brokers) {
		InitExportCursorForBroker(broker_id);
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
	committed_seq_updater_stop_.store(true, std::memory_order_release);
	committed_seq_updater_cv_.notify_all();
	if (committed_seq_updater_thread_.joinable()) {
		committed_seq_updater_thread_.join();
	}
}

// Order 2: Total order (no per-client ordering). Same epoch pipeline as Sequencer5, but no Level 5
// partition or hold buffer; all batches go directly to ready in arrival order. Design §2.3 Order 2.
void Topic::Sequencer2() {
	LOG(INFO) << "Starting Sequencer2 (Order 2 total order, no per-client state) for topic: " << topic_name_;
	ResetCompletedRangeQueue();
	global_batch_seq_.store(0, std::memory_order_release);
	{
		ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
		control_block->committed_seq.store(UINT64_MAX, std::memory_order_release);
		CXL::flush_cacheline(control_block);
		CXL::store_fence();
	}
	committed_seq_updater_stop_.store(false, std::memory_order_release);
	committed_seq_updater_thread_ = std::thread(&Topic::CommittedSeqUpdaterThread, this);
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
	global_batch_seq_.store(0, std::memory_order_release);
	epoch_index_.store(0, std::memory_order_relaxed);
	last_sequenced_epoch_.store(0, std::memory_order_relaxed);
	for (int i = 0; i < 3; i++) {
		epoch_buffers_[i].state.store(EpochBuffer5::State::IDLE, std::memory_order_relaxed);
	}
	epoch_buffers_[0].reset_and_start();

	export_cursor_by_broker_.fill(nullptr);
	for (int broker_id : registered_brokers) {
		InitExportCursorForBroker(broker_id);
	}

	epoch_driver_thread_ = std::thread(&Topic::EpochDriverThread, this);
	// Order 2 shares the same epoch sequencer as Order 5. All Order 2 batches have client_id==0,
	// so they go to level0; Level 5 paths (hold buffer, ProcessLevel5Batches) are no-ops.
	std::thread epoch_sequencer_thread(&Topic::EpochSequencerThread, this);

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
	committed_seq_updater_stop_.store(true, std::memory_order_release);
	committed_seq_updater_cv_.notify_all();
	if (committed_seq_updater_thread_.joinable()) {
		committed_seq_updater_thread_.join();
	}
}

void Topic::EpochDriverThread() {
	LOG(INFO) << "EpochDriverThread started (epoch_us=" << kEpochUs << ")";
	constexpr uint64_t kNewBrokerCheckInterval = 20000;  // Check every 20000 epochs (~10 s at 500 µs/epoch)
	uint64_t epoch_count = 0;
	while (!stop_threads_) {
		// [[FIX_EPOCH_PAUSE]] Replace 500us sleep with tight cpu_pause loop for microsecond-scale epoch sealing.
		// At 10GB/s, 500us delays cause 500KB backlogs. Use adaptive pause/yield for responsiveness.
		constexpr int kMaxIterations = 50000; // Prevent infinite spinning
		int iterations = 0;
		while (!stop_threads_ && iterations < kMaxIterations) {
			uint64_t cur = epoch_index_.load(std::memory_order_acquire);
			EpochBuffer5& cur_buf = epoch_buffers_[cur % 3];
			if (cur_buf.seal()) {
				uint64_t next = cur + 1;
				EpochBuffer5& next_buf = epoch_buffers_[next % 3];
				// Wait for next buffer with cpu_pause instead of sleep
				int wait_iterations = 0;
				while (!stop_threads_ && !next_buf.is_available() && wait_iterations < 1000) {
					CXL::cpu_pause();
					wait_iterations++;
				}
				if (stop_threads_) break;
				if (next_buf.is_available()) {
					next_buf.reset_and_start();
					epoch_index_.store(next, std::memory_order_release);
				}
				break; // Successfully processed epoch
			}
			CXL::cpu_pause();
			iterations++;
		}

		// Yield occasionally to prevent starvation, but much more frequently than 500us
		if (iterations >= kMaxIterations) {
			std::this_thread::yield();
		}

		// Periodically check for newly registered brokers and spawn scanners
		if (++epoch_count % kNewBrokerCheckInterval == 0) {
			CheckAndSpawnNewScanners();
		}
	}

	// [[TAIL_STALL_FIX]] Seal final epoch on shutdown so batches in COLLECTING state are processed.
	// Without this, when publisher ACK timeout sets stop_threads_, we exit without sealing and
	// ~7,708 messages (last epoch(s)) are never sequenced → ACK shortfall.
	LOG(INFO) << "EpochDriverThread: Sealing final epoch before exit";
	uint64_t final_epoch = epoch_index_.load(std::memory_order_acquire);
	LOG(INFO) << "EpochDriverThread: final_epoch=" << final_epoch 
	          << " last_sequenced=" << last_sequenced_epoch_.load(std::memory_order_acquire);
	EpochBuffer5& final_buf = epoch_buffers_[final_epoch % 3];
	if (final_buf.seal()) {
		// After sealing final epoch, reset the NEXT buffer to COLLECTING so scanners can push
		// late batches that became ready during shutdown.
		LOG(INFO) << "EpochDriverThread: Resetting next buffer for late-arriving batches";
		uint64_t next_epoch = final_epoch + 1;
		EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];

		// Keep shutdown bounded: do not exceed test-script grace timeout.
		constexpr auto kFinalCollectionBudget = std::chrono::milliseconds(1800);
		constexpr auto kFinalSequencerBudget = std::chrono::milliseconds(1800);

		// Wait briefly for next buffer to become reusable.
		auto buf_avail_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
		while (!next_buf.is_available() &&
		       std::chrono::steady_clock::now() < buf_avail_deadline) {
			std::this_thread::sleep_for(std::chrono::microseconds(10));
		}

		if (next_buf.is_available()) {
			next_buf.reset_and_start();
			epoch_index_.store(next_epoch, std::memory_order_release);
			LOG(INFO) << "EpochDriverThread: Next buffer (epoch " << next_epoch << ") ready for collection";

			// Adaptive wait for late arrivals: exit early once collectors are quiescent.
			auto collect_deadline = std::chrono::steady_clock::now() + kFinalCollectionBudget;
			int quiescent_checks = 0;
			while (std::chrono::steady_clock::now() < collect_deadline) {
				if (next_buf.active_collectors.load(std::memory_order_acquire) == 0) {
					if (++quiescent_checks >= 20) break;  // ~2ms quiescent (20 * 100us)
				} else {
					quiescent_checks = 0;
				}
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			}

			// Seal late-arrival epoch too (do not advance epoch_index_ past this).
			if (next_buf.seal()) {
				LOG(INFO) << "EpochDriverThread: Sealed late-arriving epoch " << next_epoch;
			}
		}

		// Wait for sequencer to process final and late-arriving epochs; bounded for fast shutdown.
		auto deadline = std::chrono::steady_clock::now() + kFinalSequencerBudget;
		uint64_t target_seq = final_epoch + 2;  // Expect sequencer to process both epochs
		while (last_sequenced_epoch_.load(std::memory_order_acquire) < target_seq &&
		       std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		LOG(INFO) << "EpochDriverThread: Final epochs sealed, last_sequenced=" 
		          << last_sequenced_epoch_.load(std::memory_order_acquire);
	} else {
		LOG(WARNING) << "EpochDriverThread: Failed to seal final epoch " << final_epoch;
	}
	epoch_driver_done_ = true;  // Non-atomic assignment
}

void Topic::CheckAndSpawnNewScanners() {
	// Get current registered brokers
	absl::btree_set<int> current_brokers;
	GetRegisteredBrokerSet(current_brokers);

	// Check for new brokers and spawn scanners
	absl::MutexLock lock(&scanner_management_mu_);
	for (int broker_id : current_brokers) {
		if (brokers_with_scanners_.find(broker_id) == brokers_with_scanners_.end()) {
			// New broker found - spawn a scanner!
			LOG(INFO) << "[DYNAMIC_SCANNER] Detected newly registered broker " << broker_id
			         << ", spawning BrokerScannerWorker5";

			// Initialize per-broker export cursor for the new broker
			InitExportCursorForBroker(broker_id);

			brokers_with_scanners_.insert(broker_id);
			scanner_threads_.emplace_back(&Topic::BrokerScannerWorker5, this, broker_id);

			LOG(INFO) << "[DYNAMIC_SCANNER] Started BrokerScannerWorker5 for broker " << broker_id
			         << ", total scanners now: " << brokers_with_scanners_.size();
		}
	}
}

/**
 * CommitEpoch: Unified commit logic for both main loop and drain loop
 * Handles GOI writing, export chain setup, CV accumulation, and consumed_through advancement
 */
void Topic::CommitEpoch(
		std::vector<PendingBatch5>& ready,
		std::vector<const PendingBatch5*>& by_slot,
		std::array<size_t, NUM_MAX_BROKERS>& contiguous_consumed_per_broker,
		std::array<bool, NUM_MAX_BROKERS>& broker_seen_in_epoch,
		std::array<uint64_t, NUM_MAX_BROKERS>& cv_max_cumulative,
		std::array<uint64_t, NUM_MAX_BROKERS>& cv_max_pbr_index,
		std::vector<PendingBatch5>& batch_list,
		bool is_drain_mode) {

	// [[NAMING]] total_order = message-level sequence (subscribers); GOI index = slot in GOI array (design §2.4).
	// One atomic per epoch (§3.2): reserve message-order range for this epoch.
	size_t total_msg = 0;
	for (const PendingBatch5& p : ready) total_msg += p.num_msg;
	size_t base_order = global_seq_.fetch_add(total_msg, std::memory_order_relaxed);  // total_order space

	// [PHASE-2D] Fast-path: ready vector is already sorted by broker+slot before calling CommitEpoch
	// Sorting is done in main loop and drain loop to preserve consumed_through contiguity.
	// [PHASE-4] Accumulate per-broker tinode updates
	// Replace hash maps with arrays for better performance
	uint64_t epoch_timestamp_ns = 0;
	if (replication_factor_ > 0) {
		epoch_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// [PHASE-7] Single-pass commit: hold lock for entire epoch; export chain inline with GOI/CV/tinode.
	absl::MutexLock header_lock(&export_cursor_mu_);

	size_t next_order = base_order;  // message order (total_order) for next batch
	GOIEntry* goi = reinterpret_cast<GOIEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kGOIOffset);

	// [PANEL C2/P1] O(1) atomics per epoch: reserve GOI indices once (§3.2)
	size_t num_goi_order5 = 0;
	for (const PendingBatch5& p : ready) {
		if (p.skipped || p.is_held_marker) continue;
		if (p.from_hold || p.hdr != nullptr) num_goi_order5++;
	}
	uint64_t base_batch_index_order5 = global_batch_seq_.fetch_add(num_goi_order5, std::memory_order_relaxed);
	size_t goi_idx_order5 = 0;

	// [COMMIT_DIAG] Count batches committed per broker this epoch
	std::array<size_t, 8> committed_this_epoch{};

	// [PHASE-3D] Regroup flushes for better pipeline utilization
	// 1. Flush all GOI entries
	for (PendingBatch5& p : ready) {
		if (p.skipped || p.is_held_marker) continue;
		uint64_t batch_index = base_batch_index_order5 + goi_idx_order5++;
		GOIEntry* entry = &goi[batch_index];

		if (p.from_hold) {
			const HoldBatchMetadata& m = p.hold_meta;
			entry->global_seq = batch_index;
			entry->total_order = next_order;
			entry->batch_id = m.batch_id;
			entry->broker_id = static_cast<uint16_t>(m.broker_id);
			entry->epoch_sequenced = m.epoch_created;
			entry->blog_offset = m.log_idx;
			entry->payload_size = static_cast<uint32_t>(m.total_size);
			entry->message_count = static_cast<uint32_t>(m.num_msg);
			entry->num_replicated.store(0, std::memory_order_release);
			entry->client_id = m.client_id;
			entry->client_seq = m.batch_seq;
			entry->pbr_index = m.pbr_absolute_index;
			entry->cumulative_message_count = m.start_logical_offset + m.num_msg;
			next_order += m.num_msg;
		} else if (p.hdr != nullptr) {
			p.hdr->total_order = next_order;
			entry->global_seq = batch_index;
			entry->total_order = next_order;
			entry->batch_id = p.cached_batch_id;
			entry->broker_id = static_cast<uint16_t>(p.broker_id);
			entry->epoch_sequenced = p.epoch_created;
			entry->blog_offset = p.cached_log_idx;
			entry->payload_size = static_cast<uint32_t>(p.cached_total_size);
			entry->message_count = static_cast<uint32_t>(p.num_msg);
			entry->num_replicated.store(0, std::memory_order_release);
			entry->client_id = p.client_id;
			entry->client_seq = p.hdr->batch_seq;
			entry->pbr_index = p.cached_pbr_absolute_index;
			entry->cumulative_message_count = p.cached_start_logical_offset + p.num_msg;
			next_order += p.num_msg;
		}
		CXL::flush_cacheline(entry);
		CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(entry) + 64);
	}

	// 2. Flush export chain and BatchHeaders
	for (PendingBatch5& p : ready) {
		if (p.skipped || p.is_held_marker || p.from_hold || p.hdr == nullptr) continue;
		int b = p.broker_id;
		if (b >= 0 && b < NUM_MAX_BROKERS) {
			BatchHeader* cur = export_cursor_by_broker_[b];
			if (cur != nullptr) {
				cur->batch_off_to_export = reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(cur);
				cur->ordered = 1;
				CXL::flush_cacheline(cur);
				CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(cur) + 64);
				BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[b].batch_headers_offset);
				BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(ring_start) + BATCHHEADERS_SIZE);
				BatchHeader* next_cursor = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(cur) + sizeof(BatchHeader));
				if (next_cursor >= ring_end) next_cursor = ring_start;
				export_cursor_by_broker_[b] = next_cursor;
			}
		}
		p.hdr->batch_complete = 0;
		__atomic_store_n(&p.hdr->flags, 0u, __ATOMIC_RELEASE);
		CXL::flush_cacheline(p.hdr);
		CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(p.hdr) + 64);
	}

	// 3. Metadata updates (local accumulation)
	goi_idx_order5 = 0;
	next_order = base_order;
	std::array<size_t, NUM_MAX_BROKERS> ordered_increment{};
	std::array<size_t, NUM_MAX_BROKERS> last_ordered_offset{};
	std::array<bool, NUM_MAX_BROKERS> ordered_broker_seen{};

	for (PendingBatch5& p : ready) {
		if (p.skipped || p.is_held_marker) continue;
		uint64_t batch_index = base_batch_index_order5 + goi_idx_order5++;

		if (p.from_hold) {
			const HoldBatchMetadata& m = p.hold_meta;
			AccumulateCVUpdate(static_cast<uint16_t>(m.broker_id), m.pbr_absolute_index, m.start_logical_offset + m.num_msg, cv_max_cumulative, cv_max_pbr_index);
			if (replication_factor_ > 0) {
				size_t ring_pos = goi_timestamp_write_pos_.fetch_add(1, std::memory_order_relaxed) % kGOITimestampRingSize;
				goi_timestamps_[ring_pos].goi_index.store(batch_index, std::memory_order_relaxed);
				goi_timestamps_[ring_pos].timestamp_ns.store(epoch_timestamp_ns, std::memory_order_release);
			}
			int b = m.broker_id;
			ordered_increment[b] += m.num_msg;
			last_ordered_offset[b] = m.log_idx;
			ordered_broker_seen[b] = true;
			{
				OrderedHoldExportEntry ex;
				ex.log_idx = m.log_idx;
				ex.batch_size = m.total_size;
				ex.total_order = next_order;
				ex.num_messages = m.num_msg;
				{
					absl::MutexLock lock(&hold_export_queues_[b].mu);
					hold_export_queues_[b].q.push_back(ex);
				}
			}
			next_order += m.num_msg;
			if (m.broker_id >= 0 && m.broker_id < static_cast<int>(committed_this_epoch.size()))
				committed_this_epoch[m.broker_id]++;
		} else {
			AccumulateCVUpdate(static_cast<uint16_t>(p.broker_id), p.cached_pbr_absolute_index, p.cached_start_logical_offset + p.num_msg, cv_max_cumulative, cv_max_pbr_index);
			if (replication_factor_ > 0) {
				size_t ring_pos = goi_timestamp_write_pos_.fetch_add(1, std::memory_order_relaxed) % kGOITimestampRingSize;
				goi_timestamps_[ring_pos].goi_index.store(batch_index, std::memory_order_relaxed);
				goi_timestamps_[ring_pos].timestamp_ns.store(epoch_timestamp_ns, std::memory_order_release);
			}
			next_order += p.num_msg;
			int b = p.broker_id;
			ordered_increment[b] += p.num_msg;
			last_ordered_offset[b] = static_cast<size_t>(reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(cxl_addr_));
			ordered_broker_seen[b] = true;

			size_t& next_expected = contiguous_consumed_per_broker[b];
			if (p.slot_offset == next_expected || (next_expected == BATCHHEADERS_SIZE && p.slot_offset == 0)) {
				next_expected = p.slot_offset + sizeof(BatchHeader);
				if (next_expected >= BATCHHEADERS_SIZE) next_expected = BATCHHEADERS_SIZE;
			}
			if (b >= 0 && b < static_cast<int>(committed_this_epoch.size()))
				committed_this_epoch[b]++;
		}
	}

	// [PHASE-4] Write accumulated tinode updates
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (!ordered_broker_seen[b]) continue;
		size_t inc = ordered_increment[b];
		tinode_->offsets[b].ordered += inc;
		tinode_->offsets[b].ordered_offset = last_ordered_offset[b];
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(&tinode_->offsets[b].ordered)));
		CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].ordered_offset));
	}
	// [PHASE-3] Single fence for all CV updates
	FlushAccumulatedCV(cv_max_cumulative, cv_max_pbr_index);

	// Advance consumed_through for all remaining slots that were processed but not ready
	AdvanceConsumedThroughForProcessedSlots(batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch, &cv_max_cumulative, &cv_max_pbr_index);

	if (num_goi_order5 > 0) {
		EnqueueCompletedRange(base_batch_index_order5, base_batch_index_order5 + num_goi_order5);
	}

	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (!broker_seen_in_epoch[b]) continue;
		size_t val = contiguous_consumed_per_broker[b];
		tinode_->offsets[b].batch_headers_consumed_through = val;
		CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].batch_headers_consumed_through));
	}
	CXL::store_fence();
}

void Topic::EpochSequencerThread() {
	LOG(INFO) << "EpochSequencerThread started for topic: " << topic_name_;
	// [[P0.3]] Reuse epoch working vectors to avoid per-iteration allocations.
	std::vector<PendingBatch5> batch_list;
	std::vector<PendingBatch5> level0, level5, ready_level5, ready;
	std::vector<const PendingBatch5*> by_slot;

	while (!stop_threads_) {
		uint64_t last = last_sequenced_epoch_.load(std::memory_order_acquire);
		uint64_t current = epoch_index_.load(std::memory_order_acquire);
		if (last >= current) {
			// [[FIX_SEQUENCER_WAIT_PAUSE]] Replace sleep_for with cpu_pause for epoch waiting.
			CXL::cpu_pause();
			CXL::cpu_pause();
			continue;
		}
		size_t buffer_idx = last % 3;
		EpochBuffer5& buf = epoch_buffers_[buffer_idx];
		if (buf.state.load(std::memory_order_acquire) != EpochBuffer5::State::SEALED) {
			// [[FIX_BUFFER_STATE_PAUSE]] Replace sleep_for with cpu_pause for buffer state waiting.
			CXL::cpu_pause();
			CXL::cpu_pause();
			continue;
		}
		// Clear only when we have work; avoids redundant clear() in idle loop.
		batch_list.clear();
		level0.clear(); level5.clear(); ready_level5.clear(); ready.clear();
		by_slot.clear();
		// [PHASE-10a] Pre-allocate batch_list
		{
			size_t total = 0;
			for (const auto& v : buf.per_broker) total += v.size();
			batch_list.reserve(total);
		}
		for (auto& v : buf.per_broker) {
			batch_list.insert(batch_list.end(),
				std::make_move_iterator(v.begin()),
				std::make_move_iterator(v.end()));
			v.clear();
		}

		// [[PERF_OPTIMIZATION]] Minimal prefetching for batch_list processing
		// Only prefetch if batch_list is large enough to benefit (> 1000 entries)
		if (batch_list.size() > 1000) {
			constexpr size_t kPrefetchAhead = 32;  // Look ahead 32 entries (conservative)
			if (kPrefetchAhead < batch_list.size()) {
				__builtin_prefetch(&batch_list[kPrefetchAhead], 0, 1);
			}
		}

		buf.state.store(EpochBuffer5::State::IDLE, std::memory_order_release);
		last_sequenced_epoch_.store(last + 1, std::memory_order_release);
		current_epoch_for_hold_.store(last + 1, std::memory_order_release);

		// [PHASE-2B] Update cached_epoch_ for sequencer use (avoids CXL read)
		{
			ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
			CXL::flush_cacheline(control_block);
			CXL::load_fence();
			cached_epoch_.store(control_block->epoch.load(std::memory_order_acquire), std::memory_order_release);
		}

		// [[DEADLOCK_FIX]] Do not skip when batch_list empty: run ProcessLevel5Batches so hold
		// buffer ageing/expiry runs every epoch. Otherwise held batches never expire.

		// [[PHASE_1A_EPOCH_FENCING]] Sequencer-side epoch validation (§4.2)
		// Reject batches with stale epoch_created so replicas never see zombie-sequencer data
		uint64_t current_epoch = cached_epoch_.load(std::memory_order_acquire);
		if (current_epoch == 0) {
			ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
			CXL::flush_cacheline(control_block);
			CXL::load_fence();
			current_epoch = control_block->epoch.load(std::memory_order_acquire);
			cached_epoch_.store(current_epoch, std::memory_order_release);
		}
		constexpr uint16_t kMaxEpochAge = 2000;  // Design §4.2: reject if entry.epoch < current_epoch - MAX_EPOCH_AGE
		
		// [PANEL FIX] Do NOT erase stale batches from batch_list! If we erase them, they never reach
		// the consumed_through update logic, so the PBR slots are leaked forever.
		// Instead, mark them as skipped so they flow through to ready and trigger consumed_through advance.
		for (PendingBatch5& p : batch_list) {
			if (p.skipped) continue;
			uint16_t cur = static_cast<uint16_t>(current_epoch & 0xFFFF);
			uint16_t created = p.epoch_created;
			uint16_t age = (cur - created) & 0xFFFF;
			if (age > kMaxEpochAge) {
				LOG_EVERY_N(WARNING, 100) << "Dropping stale batch: age=" << age << " > max=" << kMaxEpochAge
					<< " batch_seq=" << p.batch_seq << " broker=" << p.broker_id;
				p.skipped = true; // Treat as skipped slot (advances consumed_through, no sequencing)
			}
		}

		// [[DEADLOCK_FIX]] Do not skip when batch_list empty after fencing: run hold buffer processing so expiry advances.

		// [[CONSUMED_THROUGH_FIX]] Advance consumed_through for ALL batches in epoch buffer before processing.
		// ProcessLevel5Batches may hold batches due to gaps, but scanner pushed them to epoch buffer,
		// so sequencer must advance consumed_through to allow ring to drain and prevent deadlock.
		// [PHASE-3] Replace hash maps with arrays for better performance
		std::array<size_t, NUM_MAX_BROKERS> contiguous_consumed_per_broker{};
		std::array<bool, NUM_MAX_BROKERS> broker_seen_in_epoch{};
		for (const PendingBatch5& p : batch_list) {
			int b = p.broker_id;
			if (b < 0 || b >= NUM_MAX_BROKERS) continue;
			if (broker_seen_in_epoch[b]) continue;
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
			broker_seen_in_epoch[b] = true;
		}

		// Partition Level 0 (client_id==0) vs Level 5 (client_id!=0)
		// Skipped markers now carry real client_id/batch_seq and go through level5 so hold buffer can advance (§3.2)
		for (PendingBatch5& p : batch_list) {
			if (p.client_id == 0) {
				level0.push_back(std::move(p));
			} else {
				level5.push_back(std::move(p));
			}
		}

		// Process Level 5: hold buffer + gap timeout (§3.2)
		ProcessLevel5Batches(level5, ready_level5);

		// Merge Level 0 + Level 5 ready; preserve order for stable commit (we sort by broker+slot later)
		if (level5.empty() && ready_level5.empty()) {
			ready = std::move(level0);
		} else {
			ready.reserve(level0.size() + ready_level5.size());
			for (PendingBatch5& p : level0) ready.push_back(std::move(p));
			for (PendingBatch5& p : ready_level5) ready.push_back(std::move(p));
		}

		// [PHASE-3] Accumulate CV updates locally; merge Level 5 shard CV updates
		std::array<uint64_t, NUM_MAX_BROKERS> cv_max_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> cv_max_pbr_index{};

		for (auto& shard_ptr : level5_shards_) {
			if (!shard_ptr) continue;
			for (const auto& [bid, cum] : shard_ptr->cv_max_cumulative) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (cum > cv_max_cumulative[bid]) cv_max_cumulative[bid] = cum;
				}
			}
			for (const auto& [bid, pbr] : shard_ptr->cv_max_pbr_index) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (pbr > cv_max_pbr_index[bid]) cv_max_pbr_index[bid] = pbr;
				}
			}
		}

		if (ready.empty()) {
			// No ready batches, but we still need to advance consumed_through for all processed slots
			// to prevent ring deadlock. Advance consumed_through for all slots in batch_list.
			AdvanceConsumedThroughForProcessedSlots(batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch, &cv_max_cumulative, &cv_max_pbr_index);
			continue;
		}

		// Call unified commit logic
		CommitEpoch(ready, by_slot, contiguous_consumed_per_broker, broker_seen_in_epoch,
		           cv_max_cumulative, cv_max_pbr_index, batch_list, /*is_drain_mode=*/false);
	}

	// [[TAIL_STALL_FIX]] Drain remaining sealed epochs before exit.
	LOG(INFO) << "EpochSequencerThread: Draining remaining sealed epochs before exit";
	// [PANEL FIX] Increase deadline to 15s to outlive EpochDriverThread's 6s wait + sealing time
	auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (std::chrono::steady_clock::now() < drain_deadline) {
		uint64_t last = last_sequenced_epoch_.load(std::memory_order_acquire);
		uint64_t current = epoch_index_.load(std::memory_order_acquire);
		if (last >= current) {
			// [PANEL FIX] Only exit if EpochDriverThread has finished sealing everything.
			// Otherwise, wait for it to seal the final late-arriving epoch.
			if (epoch_driver_done_) {
				// [[B0_TAIL_FIX]] Force-expire any remaining hold buffer so CV is advanced for tail ACKs.
				size_t hb = GetTotalHoldBufferSize();
				if (hb > 0) {
					LOG(INFO) << "EpochSequencerThread: Final hold-buffer drain (size=" << hb << ") before exit";
					force_expire_hold_on_next_process_.store(true, std::memory_order_release);
					std::vector<PendingBatch5> level5_empty, ready_level5;
					ProcessLevel5Batches(level5_empty, ready_level5);
					force_expire_hold_on_next_process_.store(false, std::memory_order_release);
					if (!ready_level5.empty()) {
						std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_cumulative{};
						std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_pbr_index{};
						for (auto& shard_ptr : level5_shards_) {
							if (!shard_ptr) continue;
							for (const auto& [bid, cum] : shard_ptr->cv_max_cumulative) {
								if (bid >= 0 && bid < NUM_MAX_BROKERS && cum > drain_cv_max_cumulative[bid])
									drain_cv_max_cumulative[bid] = cum;
							}
							for (const auto& [bid, pbr] : shard_ptr->cv_max_pbr_index) {
								if (bid >= 0 && bid < NUM_MAX_BROKERS && pbr > drain_cv_max_pbr_index[bid])
									drain_cv_max_pbr_index[bid] = pbr;
							}
						}
						for (PendingBatch5& p : ready_level5) {
							if (p.skipped || p.is_held_marker) continue;
							if (p.from_hold) {
								const HoldBatchMetadata& m = p.hold_meta;
								AccumulateCVUpdate(static_cast<uint16_t>(m.broker_id), m.pbr_absolute_index,
									m.start_logical_offset + m.num_msg, drain_cv_max_cumulative, drain_cv_max_pbr_index);
							} else if (p.hdr) {
								AccumulateCVUpdate(static_cast<uint16_t>(p.broker_id), p.cached_pbr_absolute_index,
									p.cached_start_logical_offset + p.num_msg, drain_cv_max_cumulative, drain_cv_max_pbr_index);
							}
						}
						FlushAccumulatedCV(drain_cv_max_cumulative, drain_cv_max_pbr_index);
						CXL::store_fence();
					}
				}
				LOG(INFO) << "EpochSequencerThread: Caught up (last=" << last << " current=" << current 
				          << ") and driver done, exiting drain loop";
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		size_t buffer_idx = last % 3;
		std::vector<PendingBatch5> batch_list;
		EpochBuffer5& buf = epoch_buffers_[buffer_idx];
		if (buf.state.load(std::memory_order_acquire) != EpochBuffer5::State::SEALED) {
			std::this_thread::sleep_for(std::chrono::microseconds(100));
			continue;
		}
		{
			size_t total = 0;
			for (const auto& v : buf.per_broker) total += v.size();
			batch_list.reserve(total);
		}
		LOG(INFO) << "EpochSequencerThread Drain: Processing epoch " << last << " (buffer " << buffer_idx << ")";
		for (auto& v : buf.per_broker) {
			batch_list.insert(batch_list.end(),
				std::make_move_iterator(v.begin()),
				std::make_move_iterator(v.end()));
			v.clear();
		}
		buf.state.store(EpochBuffer5::State::IDLE, std::memory_order_release);
		last_sequenced_epoch_.store(last + 1, std::memory_order_release);
		current_epoch_for_hold_.store(last + 1, std::memory_order_release);

		ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
		CXL::flush_cacheline(control_block);
		CXL::load_fence();
		uint64_t current_epoch = control_block->epoch.load(std::memory_order_acquire);
		constexpr uint16_t kMaxEpochAge = 2000;
		// [[FIX_DRAIN_EPOCH_FENCING]] Mark stale batches as skipped instead of erasing them.
		// Erasing causes PBR slot leaks since slots are never freed. Use skip logic like main loop.
		for (PendingBatch5& p : batch_list) {
			if (p.skipped) continue;
			uint16_t cur = static_cast<uint16_t>(current_epoch & 0xFFFF);
			uint16_t created = p.epoch_created;
			uint16_t age = (cur - created) & 0xFFFF;
			if (age > kMaxEpochAge) {
				LOG_EVERY_N(WARNING, 100) << "Drain: Marking stale batch as skipped: age=" << age << " > max=" << kMaxEpochAge
					<< " batch_seq=" << p.batch_seq << " broker=" << p.broker_id;
				p.skipped = true;
			}
		}

		// [PHASE-3] Replace hash maps with arrays for better performance
		std::array<size_t, NUM_MAX_BROKERS> contiguous_consumed_per_broker{};
		std::array<bool, NUM_MAX_BROKERS> broker_seen_in_epoch{};
		for (const PendingBatch5& p : batch_list) {
			int b = p.broker_id;
			if (b < 0 || b >= NUM_MAX_BROKERS) continue;
			if (broker_seen_in_epoch[b]) continue;
			const volatile void* addr = reinterpret_cast<const volatile void*>(&tinode_->offsets[b].batch_headers_consumed_through);
			CXL::flush_cacheline(const_cast<const void*>(addr));
			CXL::load_fence();
			size_t initial = tinode_->offsets[b].batch_headers_consumed_through;
			if (initial == BATCHHEADERS_SIZE) {
				initial = 0;
			}
			contiguous_consumed_per_broker[b] = initial;
			broker_seen_in_epoch[b] = true;
		}

		std::vector<PendingBatch5> level0, level5;
		for (PendingBatch5& p : batch_list) {
			if (p.client_id == 0) {
				level0.push_back(std::move(p));
			} else {
				level5.push_back(std::move(p));
			}
		}

		std::vector<PendingBatch5> ready_level5;
		ProcessLevel5Batches(level5, ready_level5);

		std::vector<PendingBatch5> ready;
		for (PendingBatch5& p : level0) ready.push_back(std::move(p));
		for (PendingBatch5& p : ready_level5) ready.push_back(std::move(p));

		if (ready.empty()) {
			// No ready batches, but we still need to advance consumed_through for all processed slots
			// to prevent ring deadlock. Advance consumed_through for all slots in batch_list.
			AdvanceConsumedThroughForProcessedSlots(batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch, nullptr, nullptr);
			continue;
		}

		// Call unified commit logic
		// [[DRAIN_MODE_COMMIT]] Use drain-specific CV accumulator arrays
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_pbr_index{};
		for (auto& shard_ptr : level5_shards_) {
			if (!shard_ptr) continue;
			for (const auto& [bid, cum] : shard_ptr->cv_max_cumulative) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (cum > drain_cv_max_cumulative[bid]) drain_cv_max_cumulative[bid] = cum;
				}
			}
			for (const auto& [bid, pbr] : shard_ptr->cv_max_pbr_index) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (pbr > drain_cv_max_pbr_index[bid]) drain_cv_max_pbr_index[bid] = pbr;
				}
			}
		}
		CommitEpoch(ready, by_slot, contiguous_consumed_per_broker, broker_seen_in_epoch,
		           drain_cv_max_cumulative, drain_cv_max_pbr_index, batch_list, /*is_drain_mode=*/true);
	}

	// [[SHARD_SHUTDOWN]] Signal Level 5 workers to exit
	for (auto& shard : level5_shards_) {
		if (!shard) continue;
		{
			std::lock_guard<std::mutex> lock(shard->mu);
			shard->stop = true;
		}
		shard->cv.notify_one();
	}
}

bool Topic::CheckAndInsertBatchId(Level5ShardState& shard, uint64_t batch_id) {
	// Use FastDeduplicator for high-throughput deduplication
	return shard.dedup.check_and_insert(batch_id);
}

void Topic::ClientGc(Level5ShardState& shard) {
	uint64_t epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
	if ((epoch & (kClientGcEpochInterval - 1)) != 0) {
		return;
	}
	std::vector<size_t> evict;
	evict.reserve(shard.client_state.size());
	for (const auto& kv : shard.client_state) {
		const size_t cid = kv.first;
		const ClientState5& st = kv.second;
		if (epoch > st.last_epoch && epoch - st.last_epoch > kClientTtlEpochs) {
			evict.push_back(cid);
		}
	}
	for (size_t cid : evict) {
		auto hold_it = shard.hold_buffer.find(cid);
		if (hold_it != shard.hold_buffer.end()) {
			shard.hold_buffer_size -= hold_it->second.size();
			shard.hold_buffer.erase(hold_it);
		}
		shard.clients_with_held_batches.erase(cid);
		shard.client_state.erase(cid);
		// [[GC_NOTE]] Keep client_highest_committed_ as a durability barrier for late retries.
	}
}

void Topic::ProcessLevel5Batches(std::vector<PendingBatch5>& level5, std::vector<PendingBatch5>& ready) {
	if (level5_shards_.empty() || level5_num_shards_ <= 1) {
		ProcessLevel5BatchesShard(*level5_shards_[0], level5, ready);
		return;
	}

	if (level5_per_shard_cache_.size() != level5_num_shards_) {
		level5_per_shard_cache_.resize(level5_num_shards_);
	}
	for (auto& v : level5_per_shard_cache_) v.clear();

	for (PendingBatch5& p : level5) {
		size_t shard_id = p.client_id % level5_num_shards_;
		level5_per_shard_cache_[shard_id].push_back(std::move(p));
	}

	for (size_t i = 0; i < level5_num_shards_; ++i) {
		Level5ShardState& shard = *level5_shards_[i];
		{
			std::lock_guard<std::mutex> lock(shard.mu);
			shard.input.swap(level5_per_shard_cache_[i]);
			shard.ready.clear();
			shard.done = false;
			shard.has_work = true;
		}
		shard.cv.notify_one();
	}

	for (size_t i = 0; i < level5_num_shards_; ++i) {
		Level5ShardState& shard = *level5_shards_[i];
		std::unique_lock<std::mutex> lock(shard.mu);
		shard.cv.wait(lock, [&shard]() { return shard.done; });
		ready.insert(ready.end(),
			std::make_move_iterator(shard.ready.begin()),
			std::make_move_iterator(shard.ready.end()));
		shard.ready.clear();
	}
}

void Topic::ProcessLevel5BatchesShard(Level5ShardState& shard,
                                     std::vector<PendingBatch5>& level5,
                                     std::vector<PendingBatch5>& ready) {
	if (!shard.deferred_level5.empty()) {
		level5.insert(level5.end(),
			std::make_move_iterator(shard.deferred_level5.begin()),
			std::make_move_iterator(shard.deferred_level5.end()));
		shard.deferred_level5.clear();
	}
	// [PHASE-3] Clear shard CV accumulation for this invocation
	shard.cv_max_cumulative.clear();
	shard.cv_max_pbr_index.clear();
	std::array<uint64_t, NUM_MAX_BROKERS> shard_cv_max_cumulative{};
	std::array<uint64_t, NUM_MAX_BROKERS> shard_cv_max_pbr_index{};

	// [[SKIP_MARKER_FILTER]] Extract skip markers before sorting/grouping.
	// Skip markers (from BrokerScannerWorker5 hole-skip) have client_id=SIZE_MAX.
	// They don't need per-client FIFO tracking - just push directly to ready
	// so EpochSequencerThread can advance consumed_through for their slots.
	auto skip_partition = std::partition(level5.begin(), level5.end(),
		[](const PendingBatch5& p) { return !p.skipped; });
	for (auto it = skip_partition; it != level5.end(); ++it) {
		ready.push_back(std::move(*it));
	}
	level5.erase(skip_partition, level5.end());

	// [PHASE-6] Fast path: single client, no held batches, no deferred.
	// Skip radix sort, dedup check, and hold buffer management when possible.
	if (shard.hold_buffer_size == 0 &&
	    shard.deferred_level5.empty() &&
	    !level5.empty()) {
		bool single_client = true;
		size_t first_client = level5[0].client_id;
		for (size_t i = 1; i < level5.size(); ++i) {
			if (level5[i].client_id != first_client) {
				single_client = false;
				break;
			}
		}
		if (single_client) {
			if (level5.size() > 1) {
				std::sort(level5.begin(), level5.end(),
					[](const PendingBatch5& a, const PendingBatch5& b) {
						return a.batch_seq < b.batch_seq;
					});
			}
			ClientState5& state = shard.client_state[first_client];
			state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
			uint64_t& hc = shard.client_highest_committed
				.try_emplace(first_client, UINT64_MAX).first->second;
			uint64_t next_exp_saved = state.next_expected;
			bool all_in_order = true;
			for (const auto& p : level5) {
				if (p.batch_seq != state.next_expected) {
					all_in_order = false;
					break;
				}
				state.next_expected++;
			}
			if (all_in_order) {
				state.next_expected = next_exp_saved;  // Restore for mark_sequenced/advance
				for (auto& p : level5) {
					state.mark_sequenced(p.batch_seq);
					state.advance_next_expected();
					hc = (hc == UINT64_MAX) ? static_cast<uint64_t>(p.batch_seq)
					    : std::max(hc, static_cast<uint64_t>(p.batch_seq));
					ready.push_back(std::move(p));
				}
				ClientGc(shard);
				return;
			}
			state.next_expected = next_exp_saved;  // Fall through to slow path
		}
	}

	shard.radix_sorter.sort_by_client_id(level5);

	for (auto it = level5.begin(); it != level5.end(); ) {
		size_t client_id = it->client_id;
		auto group_end = it;
		while (group_end != level5.end() && group_end->client_id == client_id) ++group_end;

		shard.radix_sorter.sort_by_client_seq(it, group_end);
		ClientState5& state = shard.client_state[client_id];
		state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
		uint64_t& hc = shard.client_highest_committed.try_emplace(client_id, UINT64_MAX).first->second;
		for (auto jt = it; jt != group_end; ++jt) {
			size_t seq = jt->batch_seq;
			if (state.is_duplicate(seq)) continue;
			if (hc != UINT64_MAX && seq <= hc) {
				state.mark_sequenced(seq);
				if (seq >= state.next_expected) {
					state.next_expected = seq + 1;
				}
				// Accumulate into shard maps; flushed with epoch CV
				if (jt->hdr) {
					AccumulateCVUpdate(static_cast<uint16_t>(jt->broker_id),
						jt->cached_pbr_absolute_index,
						jt->cached_start_logical_offset + jt->num_msg,
						shard_cv_max_cumulative, shard_cv_max_pbr_index);
				}
				continue;
			}
			// In-order batch (seq == next_expected): cannot be duplicate of any
			// committed batch; dedup only needed for batches released from hold.
			if (seq == state.next_expected) {
				state.mark_sequenced(seq);
				state.advance_next_expected();
				ready.push_back(std::move(*jt));
				hc = (hc == UINT64_MAX) ? seq : std::max(hc, static_cast<uint64_t>(seq));
			} else if (seq < state.next_expected) {
				// Accumulate into shard maps
				state.mark_sequenced(seq);
				if (jt->hdr) {
					AccumulateCVUpdate(static_cast<uint16_t>(jt->broker_id),
						jt->cached_pbr_absolute_index,
						jt->cached_start_logical_offset + jt->num_msg,
						shard_cv_max_cumulative, shard_cv_max_pbr_index);
					VLOG(1) << "[L5_LATE] B" << jt->broker_id << " seq=" << seq
					        << " next_expected=" << state.next_expected << " (CV advanced for ACK)";
				}
			} else if (seq > state.next_expected) {
				if (shard.hold_buffer_size >= kHoldBufferMaxEntries) {
					// Backpressure: when hold buffer is full, add brief delay to throttle producers
					std::this_thread::sleep_for(std::chrono::microseconds(10));
					if (shard.deferred_level5.size() < kDeferredL5MaxEntries) {
						shard.deferred_level5.push_back(std::move(*jt));
						continue;
					}
					// Last resort: skip gap to preserve forward progress.
					state.mark_sequenced(seq);
					state.next_expected = seq + 1;
					ready.push_back(std::move(*jt));
					continue;
				}

				auto& client_map = shard.hold_buffer[client_id];
				if (client_map.find(seq) != client_map.end()) {
					continue;
				}

				{
					HoldEntry5 he;
					he.batch = std::move(*jt);
					// [[HOLD_METADATA_COPY]] [PHASE-5] Use cached metadata (L1-hot at scanner time)
					he.meta.log_idx = jt->cached_log_idx;
					he.meta.total_size = jt->cached_total_size;
					he.meta.batch_id = jt->cached_batch_id;
					he.meta.batch_seq = jt->batch_seq;
					he.meta.pbr_absolute_index = jt->cached_pbr_absolute_index;
					he.meta.client_id = jt->client_id;
					he.meta.epoch_created = jt->epoch_created;
					he.meta.broker_id = jt->broker_id;
					he.meta.num_msg = jt->num_msg;
					he.meta.start_logical_offset = jt->cached_start_logical_offset;
					he.hold_start_ns = SteadyNowNs();
					client_map.emplace(seq, he);
					shard.hold_buffer_size++;
					shard.clients_with_held_batches.insert(client_id);
					// [[HOLD_MARKER]] Push placeholder so EpochSequencerThread advances consumed_through for this slot
					PendingBatch5 marker;
					marker.broker_id = jt->broker_id;
					marker.slot_offset = jt->slot_offset;
					marker.scanner_seq = jt->scanner_seq;
					marker.is_held_marker = true;
					marker.hdr = nullptr;
					marker.num_msg = 0;
					ready.push_back(marker);
				}
			}
		}
		it = group_end;
	}

	// Drain hold buffer: in-order entries for active clients
	for (auto sit = shard.clients_with_held_batches.begin(); sit != shard.clients_with_held_batches.end(); ) {
		size_t cid = *sit;
		auto map_it = shard.hold_buffer.find(cid);
		if (map_it == shard.hold_buffer.end()) {
			auto to_erase = sit++;
			shard.clients_with_held_batches.erase(*to_erase);
			continue;
		}
		ClientState5& state = shard.client_state[cid];
		state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
		// [[DEFENSIVE]] Client must have been seen in main loop, but use try_emplace for safety
		uint64_t& hc = shard.client_highest_committed.try_emplace(cid, UINT64_MAX).first->second;
		auto& cmap = map_it->second;
		while (true) {
			auto seq_it = cmap.find(state.next_expected);
			if (seq_it == cmap.end()) break;
			HoldEntry5& he = seq_it->second;
			if (he.batch.batch_seq <= hc) {
				state.mark_sequenced(he.batch.batch_seq);
				state.advance_next_expected();
				cmap.erase(seq_it);
				shard.hold_buffer_size--;
				continue;
			}
			PendingBatch5 b = std::move(he.batch);
			b.from_hold = true;
			b.hold_meta = he.meta;
			state.mark_sequenced(b.batch_seq);
			state.advance_next_expected();
			if (!CheckAndInsertBatchId(shard, he.meta.batch_id)) {
				ready.push_back(std::move(b));
				hc = (hc == UINT64_MAX) ? b.batch_seq : std::max(hc, static_cast<uint64_t>(b.batch_seq));
			}
			cmap.erase(seq_it);
			shard.hold_buffer_size--;
		}
		if (cmap.empty()) {
			shard.hold_buffer.erase(map_it);
			auto to_erase = sit++;
			shard.clients_with_held_batches.erase(*to_erase);
		} else {
			++sit;
		}
	}

	// Age hold buffer: wall-clock timeout on each client's front held entry.
	const uint64_t now_ns = SteadyNowNs();
	shard.expired_hold_buffer.clear();
	shard.expired_hold_keys_buffer.clear();
	shard.expired_hold_keys_buffer.reserve(shard.clients_with_held_batches.size());
	for (size_t cid : shard.clients_with_held_batches) {
		auto map_it = shard.hold_buffer.find(cid);
		if (map_it == shard.hold_buffer.end() || map_it->second.empty()) continue;
		size_t min_seq = SIZE_MAX;
		for (const auto& pair : map_it->second) {
			if (pair.first < min_seq) min_seq = pair.first;
		}
		const auto& front = map_it->second.at(min_seq);
		if (force_expire_hold_on_next_process_.load(std::memory_order_acquire) ||
		    now_ns - front.hold_start_ns >= kOrder5BaseTimeoutNs) {
			shard.expired_hold_keys_buffer.emplace_back(cid, min_seq);
		}
	}
	for (const auto& [cid, seq] : shard.expired_hold_keys_buffer) {
		auto map_it = shard.hold_buffer.find(cid);
		if (map_it == shard.hold_buffer.end()) continue;
		auto& cmap = map_it->second;
		auto entry_it = cmap.find(seq);
		if (entry_it == cmap.end()) continue;
		ExpiredHoldEntry ent;
		ent.client_id = cid;
		ent.seq = seq;
		ent.batch = entry_it->second.batch;
		ent.meta = entry_it->second.meta;
		cmap.erase(entry_it);
		shard.hold_buffer_size--;
		if (cmap.empty()) {
			shard.hold_buffer.erase(map_it);
			shard.clients_with_held_batches.erase(cid);
		}
		shard.expired_hold_buffer.push_back(ent);
	}
	std::sort(shard.expired_hold_buffer.begin(), shard.expired_hold_buffer.end(),
		[](const ExpiredHoldEntry& a, const ExpiredHoldEntry& b) {
			if (a.client_id != b.client_id) return a.client_id < b.client_id;
			return a.seq < b.seq;
		});
	for (ExpiredHoldEntry& ent : shard.expired_hold_buffer) {
		VLOG(1) << "[L5_EXPIRY_ENTRY] B" << ent.batch.broker_id << " client=" << ent.client_id
		        << " seq=" << ent.seq;
		ClientState5& state = shard.client_state[ent.client_id];
		state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
		// [[DEFENSIVE]] Client must have been seen in main loop, but use try_emplace for safety
		uint64_t& hc = shard.client_highest_committed.try_emplace(ent.client_id, UINT64_MAX).first->second;
		if (ent.seq < state.next_expected || ent.seq <= hc) {
			state.mark_sequenced(ent.seq);
			// Advance next_expected so later batches (e.g. seq+1) can drain; otherwise
			// we'd leave next_expected behind and hold valid in-order batches forever.
			if (ent.seq >= state.next_expected)
				state.next_expected = ent.seq + 1;
			// [[B0_ACK_FIX]] [PHASE-3] Accumulate CV for expired/skipped entries into shard maps
			AccumulateCVUpdate(static_cast<uint16_t>(ent.batch.broker_id),
				ent.meta.pbr_absolute_index,
				ent.meta.start_logical_offset + ent.meta.num_msg,
				shard_cv_max_cumulative, shard_cv_max_pbr_index);
			VLOG(1) << "[L5_EXPIRED_SKIP] B" << ent.batch.broker_id << " seq=" << ent.seq
			        << " (CV advanced for ACK)";
			continue;
		}
		// Sequence is beyond next_expected - skip the gap
		state.next_expected = ent.seq + 1;
		state.mark_sequenced(ent.seq);
		if (!CheckAndInsertBatchId(shard, ent.meta.batch_id)) {
			PendingBatch5 b = std::move(ent.batch);
			b.from_hold = true;
			b.hold_meta = ent.meta;
			ready.push_back(std::move(b));
			hc = (hc == UINT64_MAX) ? ent.seq : std::max(hc, static_cast<uint64_t>(ent.seq));
		}
	}

	// Second drain: emit batches that became ready after age_hold advanced next_expected (gap-skip).
	// Ablation pattern: drain_hold → age_hold → drain_hold so unblocked batches are emitted this epoch.
	for (auto sit = shard.clients_with_held_batches.begin(); sit != shard.clients_with_held_batches.end(); ) {
		size_t cid = *sit;
		auto map_it = shard.hold_buffer.find(cid);
		if (map_it == shard.hold_buffer.end()) {
			auto to_erase = sit++;
			shard.clients_with_held_batches.erase(*to_erase);
			continue;
		}
		ClientState5& state = shard.client_state[cid];
		state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
		// [[DEFENSIVE]] Client must have been seen in main loop, but use try_emplace for safety
		uint64_t& hc = shard.client_highest_committed.try_emplace(cid, UINT64_MAX).first->second;
		auto& cmap = map_it->second;
		while (true) {
			auto seq_it = cmap.find(state.next_expected);
			if (seq_it == cmap.end()) break;
			HoldEntry5& he = seq_it->second;
			if (he.batch.batch_seq <= hc) {
				state.mark_sequenced(he.batch.batch_seq);
				state.advance_next_expected();
				cmap.erase(seq_it);
				shard.hold_buffer_size--;
				continue;
			}
			PendingBatch5 b = std::move(he.batch);
			b.from_hold = true;
			b.hold_meta = he.meta;
			state.mark_sequenced(b.batch_seq);
			state.advance_next_expected();
			if (!CheckAndInsertBatchId(shard, he.meta.batch_id)) {
				ready.push_back(std::move(b));
				hc = (hc == UINT64_MAX) ? b.batch_seq : std::max(hc, static_cast<uint64_t>(b.batch_seq));
			}
			cmap.erase(seq_it);
			shard.hold_buffer_size--;
		}
		if (cmap.empty()) {
			shard.hold_buffer.erase(map_it);
			auto to_erase = sit++;
			shard.clients_with_held_batches.erase(*to_erase);
		} else {
			++sit;
		}
	}

	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (shard_cv_max_cumulative[b] > 0) {
			shard.cv_max_cumulative[b] = shard_cv_max_cumulative[b];
		}
		if (shard_cv_max_pbr_index[b] > 0) {
			shard.cv_max_pbr_index[b] = shard_cv_max_pbr_index[b];
		}
	}
	ClientGc(shard);
}

size_t Topic::GetTotalHoldBufferSize() {
	size_t total = 0;
	for (auto& sp : level5_shards_) {
		if (!sp) continue;
		std::lock_guard<std::mutex> lock(sp->mu);
		total += sp->hold_buffer_size;
	}
	return total;
}

void Topic::Level5ShardWorker(size_t shard_id) {
	Level5ShardState& shard = *level5_shards_[shard_id];
	while (true) {
		std::vector<PendingBatch5> input;
		{
			std::unique_lock<std::mutex> lock(shard.mu);
			// [PANEL FIX] Ignore stop_threads_: worker must stay alive until EpochSequencerThread
			// finishes drain and sets shard.stop. Otherwise workers exit early and drain fails.
			shard.cv.wait(lock, [&]() { return shard.has_work || shard.stop; });
			if (shard.stop && !shard.has_work) {
				break;
			}
			shard.has_work = false;
			input.swap(shard.input);
		}

		shard.ready.clear();
		ProcessLevel5BatchesShard(shard, input, shard.ready);

		{
			std::lock_guard<std::mutex> lock(shard.mu);
			shard.done = true;
		}
		shard.cv.notify_one();
	}
}

void Topic::InitLevel5Shards() {
	if (level5_shards_started_.exchange(true)) {
		return;
	}
	size_t shard_count = 1;
	if (const char* env = std::getenv("EMBAR_LEVEL5_SHARDS")) {
		int parsed = std::atoi(env);
		if (parsed > 0) {
			shard_count = static_cast<size_t>(parsed);
		}
	}
	if (shard_count > 32) shard_count = 32;
	level5_num_shards_ = shard_count;
	level5_shards_.resize(level5_num_shards_);
	for (size_t i = 0; i < level5_num_shards_; ++i) {
		level5_shards_[i] = std::make_unique<Level5ShardState>();
	}

	if (level5_num_shards_ > 1) {
		level5_shard_threads_.reserve(level5_num_shards_);
		for (size_t i = 0; i < level5_num_shards_; ++i) {
			level5_shard_threads_.emplace_back(&Topic::Level5ShardWorker, this, i);
		}
		LOG(INFO) << "Sequencer5: Level5 shards enabled, shards=" << level5_num_shards_;
	} else {
		LOG(INFO) << "Sequencer5: Level5 sharding disabled (single shard).";
	}
}

void Topic::BrokerScannerWorker5(int broker_id) {
	if (kEnableDiagnostics) {
		LOG(INFO) << "BrokerScannerWorker5 starting for broker " << broker_id;
	}
	// Wait until tinode of the broker is initialized
	while(tinode_->offsets[broker_id].log_offset == 0){
		std::this_thread::yield();
	}
	if (kEnableDiagnostics) {
		LOG(INFO) << "BrokerScannerWorker5 broker " << broker_id << " initialized, starting scan loop";
	}

	BatchHeader* ring_start_default = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
	BatchHeader* current_batch_header = ring_start_default;
	uint64_t scanner_slot_seq = 0;
	
	// [[CRITICAL FIX: Ring Buffer Boundary]] - Calculate ring end to prevent out-of-bounds access
	// Each broker gets BATCHHEADERS_SIZE bytes per topic for batch headers
	BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(ring_start_default) + BATCHHEADERS_SIZE);
	// Start scanner from consumed_through (next expected slot), not ring start.
	{
		const void* consumed_addr = const_cast<const void*>(
			reinterpret_cast<const volatile void*>(
				&tinode_->offsets[broker_id].batch_headers_consumed_through));
		CXL::flush_cacheline(consumed_addr);
		CXL::load_fence();
		size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
		if (consumed == BATCHHEADERS_SIZE) consumed = 0;
		if (consumed >= BATCHHEADERS_SIZE || (consumed % sizeof(BatchHeader)) != 0) {
			consumed = 0;
		}
		scanner_slot_seq = static_cast<uint64_t>(consumed / sizeof(BatchHeader));
		current_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(ring_start_default) + consumed);
	}

	size_t total_batches_processed = 0;
	auto last_log_time = std::chrono::steady_clock::now();
	auto scanner_heartbeat_time = last_log_time;
	size_t idle_cycles = 0;
	size_t heartbeat_iteration_count = 0;  // [[PERF_OPTIMIZATION]] Counter-based heartbeat to avoid timestamp overhead

	// Scanner only skips truly stuck CLAIMED slots; empty/non-claimed slots are treated as tail.
	const uint64_t kMaxClaimedWaitUs = 10ULL * 1000 * 1000;       // 10 seconds
	const uint64_t kMaxEmptyHoleWaitUs = 10;                       // 10 microseconds (not 2 seconds!)
	uint64_t hole_wait_start_ns = 0;
	uint64_t empty_hole_wait_start_ns = 0;
	uint64_t empty_hole_ring_full_base = 0;
	size_t holes_skipped = 0;  // Diagnostic: track how many slots were skipped after timeout

	// [[PEER_REVIEW #10]] Named constants for scanner timing
	constexpr size_t kIdleCyclesThreshold = 1024;    // Idle cycles before sleeping
	constexpr size_t kHeartbeatIterations = 1000000; // [[PERF_OPTIMIZATION]] ~1M iterations at 1M/sec = ~1s heartbeat
	// [[Phase 2.1]] Bounded CLAIMED wait: prevent permanent liveness failure if broker dies mid-write
	constexpr uint64_t kClaimedHealthCheckIntervalUs = 1ULL * 1000 * 1000;  // Check every 1s

	
	while (!stop_threads_) {

		// [[PERF_CRITICAL]] Conservative prefetching to hide CXL latency (200-500ns)
		// Prefetch 2 slots ahead (256 bytes) - matches original tuning for optimal CXL performance
		{
			BatchHeader* prefetch_target = current_batch_header + 2;
			if (prefetch_target >= ring_end) {
				prefetch_target = ring_start_default + (prefetch_target - ring_end);
			}
			__builtin_prefetch(prefetch_target, 0, 1);  // Read prefetch
		}

		CXL::flush_cacheline(current_batch_header);
		CXL::load_fence();

		// Check current batch header using num_msg + VALID flag gating
		// num_msg is uint32_t in BatchHeader, so read as volatile uint32_t for type safety
		// For non-coherent CXL: volatile prevents compiler caching; ACQUIRE doesn't help cache coherence
		volatile uint32_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
		uint32_t flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
		bool is_claimed = (flags & kBatchHeaderFlagClaimed) != 0;
		bool is_valid = (flags & kBatchHeaderFlagValid) != 0;

		if (is_valid) {
			// [P2] Only invalidate second cacheline if batch is valid
			CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(current_batch_header) + 64);
			CXL::load_fence();
		}

		// Max reasonable: 2MB batch / 64B min message = ~32k messages, use 100k as safety limit
		constexpr uint32_t MAX_REASONABLE_NUM_MSG = 100000;
		// [[DIAGNOSTIC]] batch_complete is read for logging only, not for batch_ready decision.
		// Producer sets flags VALID after full header is published; num_msg sanity is still checked.
		volatile int batch_complete_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->batch_complete;
		bool batch_ready = is_valid && (num_msg_check > 0 && num_msg_check <= MAX_REASONABLE_NUM_MSG);
		
		// [[PERF_OPTIMIZATION]] Lazy timestamp collection - only when needed for hole timing or heartbeat
		// Removed: std::chrono::steady_clock::now() call from every iteration (~25ns overhead)

		// Per-scanner heartbeat for observability; use iteration counter to avoid timestamp overhead
		if (kEnableDiagnostics && ++heartbeat_iteration_count >= kHeartbeatIterations) {
			auto heartbeat_now = std::chrono::steady_clock::now();
			const char* state_str = batch_ready ? "READY" : "EMPTY";
			VLOG(1) << "[Scanner B" << broker_id << "] slot=" << std::hex << current_batch_header << std::dec
				<< " num_msg=" << num_msg_check << " batch_complete=" << batch_complete_check
				<< " flags=0x" << std::hex << flags << std::dec
				<< " valid=" << is_valid << " claimed=" << is_claimed
				<< " state=" << state_str << " batches_collected_total=" << total_batches_processed
				<< " holes_skipped=" << holes_skipped;
			scanner_heartbeat_time = heartbeat_now;
			heartbeat_iteration_count = 0;
		}
		
		if (!batch_ready) {
			// Only truly empty slots (!VALID && !CLAIMED) are tail.
			// VALID-with-bad-num_msg is treated as in-flight metadata visibility lag and follows bounded wait.
			if (!is_claimed && !is_valid) {
				// Detect orphan hole: producer cursor moved past this slot but it stayed empty.
				// This prevents permanent ingest deadlock when a reservation path leaves a hole.
				size_t current_slot_offset = reinterpret_cast<uint8_t*>(current_batch_header) -
					reinterpret_cast<uint8_t*>(ring_start_default);
				size_t producer_next_offset = cached_next_slot_offset_.load(std::memory_order_acquire);
				uint64_t producer_next_slot_seq = 0;
				if (use_lock_free_pbr_ && num_slots_ > 0) {
					producer_next_slot_seq = pbr_state_.load(std::memory_order_acquire).next_slot_seq;
					producer_next_offset = static_cast<size_t>((producer_next_slot_seq % num_slots_) * sizeof(BatchHeader));
				}
				bool producer_ahead = use_lock_free_pbr_
					? (producer_next_slot_seq > scanner_slot_seq)
					: (producer_next_offset != current_slot_offset);
				uint64_t ring_full_now = ring_full_count_.load(std::memory_order_relaxed);
				if (empty_hole_wait_start_ns == 0) {
					// [[PERF_OPTIMIZATION]] Lazy timestamp: only call now() when entering wait state
					auto empty_hole_now = std::chrono::steady_clock::now();
					empty_hole_wait_start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
						empty_hole_now.time_since_epoch()).count();
					empty_hole_ring_full_base = ring_full_now;
				}
				// Empty-slot liveness: producer_ahead can be false in wrap/pressure states.
				// If ring-full pressure is rising while this slot stays empty, treat it as an orphan hole.
				bool ring_pressure_rising = (ring_full_now > empty_hole_ring_full_base + 1024);
				bool should_age_empty_hole = producer_ahead || ring_pressure_rising;
				if (should_age_empty_hole) {
					// [[PERF_OPTIMIZATION]] Lazy timestamp: only call now() when checking timeout
					auto empty_check_now = std::chrono::steady_clock::now();
					uint64_t empty_check_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
						empty_check_now.time_since_epoch()).count();
					uint64_t empty_wait_us = (empty_check_now_ns - empty_hole_wait_start_ns) / 1000;
					if (empty_wait_us >= kMaxEmptyHoleWaitUs) {
						++holes_skipped;
						PendingBatch5 skip_marker;
						skip_marker.hdr = current_batch_header;
						skip_marker.broker_id = broker_id;
						skip_marker.num_msg = 0;
						skip_marker.client_id = SIZE_MAX;
						skip_marker.batch_seq = SIZE_MAX;
						skip_marker.slot_offset = current_slot_offset;
						skip_marker.epoch_created = 0;
						skip_marker.skipped = true;
						skip_marker.scanner_seq = next_scanner_seq_.fetch_add(1, std::memory_order_relaxed);

						uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
						EpochBuffer5& buf = epoch_buffers_[epoch % 3];
						if (buf.enter_collection()) {
							buf.per_broker[broker_id].push_back(skip_marker);
							buf.exit_collection();
								BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
									reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
								if (next_batch_header >= ring_end) next_batch_header = ring_start_default;
								current_batch_header = next_batch_header;
								++scanner_slot_seq;
								empty_hole_wait_start_ns = 0;
								empty_hole_ring_full_base = ring_full_now;
								hole_wait_start_ns = 0;
							continue;
						}
					}
				}
				hole_wait_start_ns = 0;
				++idle_cycles;
				if (idle_cycles >= kIdleCyclesThreshold) {
					// [[FIX_IDLE_YIELD]] Use yield() for idle scanner - allows other threads to run when no batches available.
					// 1024 idle cycles means no work for ~1024 ring slots, so yield to prevent CPU waste.
					std::this_thread::yield();
					idle_cycles = 0;
				} else {
					CXL::cpu_pause();
				}
				continue;
			}

			// CLAIMED but not VALID: bounded wait, then skip only on stuck/failed producer.
			if (hole_wait_start_ns == 0) {
				// [[PERF_OPTIMIZATION]] Lazy timestamp: only call now() when entering wait state
				auto claimed_hole_now = std::chrono::steady_clock::now();
				hole_wait_start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
					claimed_hole_now.time_since_epoch()).count();
			}
			empty_hole_wait_start_ns = 0;

			// [[PERF_OPTIMIZATION]] Lazy timestamp: only call now() when checking timeout
			auto claimed_check_now = std::chrono::steady_clock::now();
			uint64_t claimed_check_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
				claimed_check_now.time_since_epoch()).count();
			uint64_t waited_us = (claimed_check_now_ns - hole_wait_start_ns) / 1000;
			if (waited_us < kMaxClaimedWaitUs) {
				++idle_cycles;
				if (idle_cycles >= kIdleCyclesThreshold) {
					// [[FIX_CLAIMED_YIELD]] Use yield() for CLAIMED slot waits - allows producer to make progress.
					// CLAIMED slots indicate producer is working, so yield to give producer CPU time.
					std::this_thread::yield();
					idle_cycles = 0;
				} else {
					CXL::cpu_pause();
				}
				continue;
			}

			{
				// [[PERF_OPTIMIZATION]] Reuse the timestamp from timeout check above
				uint64_t claimed_waited_us = (claimed_check_now_ns - hole_wait_start_ns) / 1000;
				if (claimed_waited_us > kClaimedHealthCheckIntervalUs) {
					absl::btree_set<int> alive_brokers;
					GetRegisteredBrokerSet(alive_brokers);
					if (alive_brokers.find(broker_id) == alive_brokers.end()) {
						LOG(ERROR) << "[Scanner B" << broker_id
							<< "] Broker DEAD but slot still CLAIMED — forcing skip (no clear)";
					}
				}
				// [[B0_ACK_FIX]] Same-process re-check before CLAIMED skip: re-read up to 5×50ms.
				if (broker_id == 0) {
					bool recheck_ready = false;
					for (int recheck = 0; recheck < 5 && !recheck_ready; ++recheck) {
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
						// [PHASE-0 FIX] Both cache lines for re-check
						CXL::invalidate_cacheline_for_read(current_batch_header);
						CXL::invalidate_cacheline_for_read(
							reinterpret_cast<const uint8_t*>(current_batch_header) + 64);
						CXL::full_fence();
						uint32_t re_flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
						volatile uint32_t re_num = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
						if ((re_flags & kBatchHeaderFlagValid) && re_num > 0 && re_num <= MAX_REASONABLE_NUM_MSG) {
							recheck_ready = true;
						}
					}
					if (recheck_ready) {
						hole_wait_start_ns = 0;
						continue;  // Next iteration will see ready and process
					}
				}
				// Phase 2: Ultimate timeout or broker dead — force skip to prevent permanent stall.
				// Do NOT clear CLAIMED: if we did, a late producer could write VALID and we'd process
				// the same batch twice on next wrap (Order 2 has no dedup). Skip + advance only; on
				// next wrap slot is still CLAIMED (skip again), or VALID (process), or empty.
				LOG(ERROR) << "[Scanner B" << broker_id << "] CLAIMED slot timeout ("
					<< (kMaxClaimedWaitUs / 1000000) << "s), forcing skip. DATA MAY BE LOST for this batch.";
				hole_wait_start_ns = 0;
				++holes_skipped;

				// Push SKIP marker (same as hole-skip) so EpochSequencerThread advances consumed_through
				{
					size_t slot_offset = reinterpret_cast<uint8_t*>(current_batch_header) -
						reinterpret_cast<uint8_t*>(ring_start_default);
					PendingBatch5 skip_marker;
					skip_marker.hdr = current_batch_header;
					skip_marker.broker_id = broker_id;
					skip_marker.num_msg = 0;
					skip_marker.client_id = SIZE_MAX;
					skip_marker.batch_seq = SIZE_MAX;
					skip_marker.slot_offset = slot_offset;
					skip_marker.epoch_created = 0;
					skip_marker.skipped = true;
					skip_marker.scanner_seq = next_scanner_seq_.fetch_add(1, std::memory_order_relaxed);

					uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
					EpochBuffer5& buf = epoch_buffers_[epoch % 3];
					if (buf.enter_collection()) {
						buf.per_broker[broker_id].push_back(skip_marker);
						buf.exit_collection();
							BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
								reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
							if (next_batch_header >= ring_end) next_batch_header = ring_start_default;
								current_batch_header = next_batch_header;
								++scanner_slot_seq;
								if (idle_cycles >= kIdleCyclesThreshold) {
									// [[FIX_SCANNER_ADVANCE_PAUSE]] Replace sleep_for with cpu_pause for slot advancement.
									CXL::cpu_pause();
									CXL::cpu_pause();
									CXL::cpu_pause();
									CXL::cpu_pause();
							idle_cycles = 0;
						}
					} else {
						// [[FIX_SCANNER_RETRY_PAUSE]] Replace sleep_for with cpu_pause for retry logic.
						CXL::cpu_pause();
						CXL::cpu_pause();
						CXL::cpu_pause();
						CXL::cpu_pause();
					}
				}
				continue;
			}
		}

	// Batch is ready - reset hole wait state
	hole_wait_start_ns = 0;

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
	pending.scanner_seq = next_scanner_seq_.fetch_add(1, std::memory_order_relaxed);
	// [PHASE-5] Cache metadata while in L1 (just invalidated both cache lines above)
	pending.cached_log_idx = current_batch_header->log_idx;
	pending.cached_total_size = current_batch_header->total_size;
	pending.cached_batch_id = current_batch_header->batch_id;
	pending.cached_pbr_absolute_index = current_batch_header->pbr_absolute_index;
	pending.cached_start_logical_offset = current_batch_header->start_logical_offset;

	// [[COMPLETE_DATA_LOSS_FIX]] Once batch is ready, it MUST be pushed to epoch buffer
	// before advancing the scanner position. This ensures no batches are lost during shutdown.
	// Spin-wait until we successfully push to an available epoch buffer.
	{
		bool pushed = false;
		int retry_count = 0;
		constexpr int kMaxRetries = 10000;  // ~1 second of retries with cpu_pause
		
		while (!pushed && retry_count < kMaxRetries) {
			uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
			EpochBuffer5& buf = epoch_buffers_[epoch % 3];
			
			if (buf.enter_collection()) {
				// Successfully entered collection; push the batch.
				buf.per_broker[broker_id].push_back(pending);
				buf.exit_collection();
				pushed = true;
			} else {
				// Buffer sealed; wait briefly for next epoch to be available for collection
				CXL::cpu_pause();
				++retry_count;
				
				// Occasional micro-sleep to prevent busy-wait during shutdown
				if (retry_count % 100 == 0) {
					std::this_thread::sleep_for(std::chrono::microseconds(10));
				}
			}
		}
		
		if (!pushed) {
			// [PANEL C5] Do not advance: retry same slot next iteration (Property 5: Progress).
			// Advancing would lose the batch (PBR slot never sequenced). Sleep then retry.
			if (kEnableDiagnostics) {
				LOG(ERROR) << "[PUSH_FAILURE] BrokerScannerWorker5 [B" << broker_id
				           << "] Failed to push batch after " << kMaxRetries << " retries; will retry slot (batch_seq="
				           << pending.batch_seq << "). Do not advancing scanner.";
				static std::atomic<uint64_t> push_retry_count{0};
				push_retry_count.fetch_add(1, std::memory_order_relaxed);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
	}
	total_batches_processed++;
	idle_cycles = 0;

	// Periodic status logging (VLOG to avoid hot-path overhead during throughput tests)
	auto now = std::chrono::steady_clock::now();
		if (kEnableDiagnostics && std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
			VLOG(2) << "BrokerScannerWorker5 [B" << broker_id << "]: Processed " << total_batches_processed 
			        << " batches, current tinode.ordered=" << tinode_->offsets[broker_id].ordered;
			last_log_time = now;
		}

	// Advance to next batch header
	BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
	
	// Ring Buffer Boundary Check
		if (next_batch_header >= ring_end) {
			next_batch_header = ring_start_default;
		}
		
		current_batch_header = next_batch_header;
		++scanner_slot_seq;
	}

	// [[B0_ACK_FIX]] Drain phase: after stop_threads_, process any remaining ready batches so the
	// last B0 batch (e.g. ~1,927 msgs) that became VALID just before shutdown is not lost.
	// Without this, we exit the main loop without re-checking the current slot; if NetworkManager
	// wrote VALID after our last check, that batch would never be pushed → B0 ordered short.
	constexpr uint64_t kDrainDurationMs = 2000;
	constexpr uint32_t kMaxReasonableNumMsg = 100000;
	auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDrainDurationMs);
	size_t drain_pushed = 0;
	LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: Starting drain (up to " << kDrainDurationMs << " ms)";

	while (std::chrono::steady_clock::now() < drain_deadline) {
		// [PHASE-0 FIX] Invalidate both cache lines in drain phase
		CXL::flush_cacheline(current_batch_header);
		CXL::flush_cacheline(
			reinterpret_cast<const uint8_t*>(current_batch_header) + 64);
		CXL::load_fence();
		volatile uint32_t num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
		uint32_t flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
		bool is_valid = (flags & kBatchHeaderFlagValid) != 0;
		bool batch_ready = is_valid && (num_msg_check > 0 && num_msg_check <= kMaxReasonableNumMsg);

		if (batch_ready) {
			size_t slot_offset = reinterpret_cast<uint8_t*>(current_batch_header) -
				reinterpret_cast<uint8_t*>(ring_start_default);
			PendingBatch5 pending;
			pending.hdr = const_cast<BatchHeader*>(current_batch_header);
			pending.broker_id = broker_id;
			pending.num_msg = num_msg_check;
			pending.client_id = current_batch_header->client_id;
			pending.batch_seq = current_batch_header->batch_seq;
			pending.slot_offset = slot_offset;
			pending.epoch_created = static_cast<uint16_t>(current_batch_header->epoch_created);
			pending.scanner_seq = next_scanner_seq_.fetch_add(1, std::memory_order_relaxed);
			pending.cached_log_idx = current_batch_header->log_idx;
			pending.cached_total_size = current_batch_header->total_size;
			pending.cached_batch_id = current_batch_header->batch_id;
			pending.cached_pbr_absolute_index = current_batch_header->pbr_absolute_index;
			pending.cached_start_logical_offset = current_batch_header->start_logical_offset;

			bool pushed = false;
			for (int r = 0; r < 5000 && !pushed; ++r) {
				uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
				EpochBuffer5& buf = epoch_buffers_[epoch % 3];
				if (buf.enter_collection()) {
					buf.per_broker[broker_id].push_back(pending);
					buf.exit_collection();
					pushed = true;
					drain_pushed++;
					total_batches_processed++;
				} else {
					std::this_thread::sleep_for(std::chrono::microseconds(10));
				}
			}
			if (!pushed) {
				// [PANEL C5] Do not advance: retry same slot next iteration (Property 5: Progress).
				LOG(WARNING) << "BrokerScannerWorker5 [B" << broker_id << "] drain: failed to push batch_seq="
				             << current_batch_header->batch_seq << " after retries; will retry slot";
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
		}

		BatchHeader* next_batch_header_drain = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
		if (next_batch_header_drain >= ring_end) next_batch_header_drain = ring_start_default;
		current_batch_header = next_batch_header_drain;
		++scanner_slot_seq;

		if (!batch_ready) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	if (drain_pushed > 0) {
		LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: Drain complete, pushed " << drain_pushed
		          << " batches (total_batches_processed=" << total_batches_processed << ")";
	}
}

// Helper method to advance consumed_through for processed slots
// [[WRAP_FIX]] Handle ring wrap: when next_expected==BATCHHEADERS_SIZE, slot 0 is the next expected.
void Topic::AdvanceConsumedThroughForProcessedSlots(
    const std::vector<PendingBatch5>& batch_list,
    std::array<size_t, NUM_MAX_BROKERS>& contiguous_consumed_per_broker,
    const std::array<bool, NUM_MAX_BROKERS>& broker_seen_in_epoch,
    const std::array<uint64_t, NUM_MAX_BROKERS>* cv_max_cumulative,
    const std::array<uint64_t, NUM_MAX_BROKERS>* cv_max_pbr_index) {

	std::vector<const PendingBatch5*> by_slot;
	by_slot.reserve(batch_list.size());
	for (const PendingBatch5& p : batch_list) by_slot.push_back(&p);
	std::sort(by_slot.begin(), by_slot.end(),
		[](const PendingBatch5* a, const PendingBatch5* b) {
			if (a->broker_id != b->broker_id) return a->broker_id < b->broker_id;
			return a->slot_offset < b->slot_offset;
		});
	for (const PendingBatch5* p : by_slot) {
		int b = p->broker_id;
		if (b < 0 || b >= NUM_MAX_BROKERS) continue;
		size_t& next_expected = contiguous_consumed_per_broker[b];
		if (p->slot_offset == next_expected ||
		    (next_expected == BATCHHEADERS_SIZE && p->slot_offset == 0)) {
			next_expected = p->slot_offset + sizeof(BatchHeader);
			if (next_expected >= BATCHHEADERS_SIZE) next_expected = BATCHHEADERS_SIZE;
		}
	}
	// Write back consumed_through advances
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (!broker_seen_in_epoch[b]) continue;
		size_t val = contiguous_consumed_per_broker[b];
		tinode_->offsets[b].batch_headers_consumed_through = val;
		CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].batch_headers_consumed_through));
	}
	// [BUG_FIX] Flush accumulated CV if provided (for late-arriving/skipped L5 batches)
	if (cv_max_cumulative && cv_max_pbr_index) {
		FlushAccumulatedCV(*cv_max_cumulative, *cv_max_pbr_index);
	}
	CXL::store_fence();
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

	const void* seq_region = const_cast<const void*>(static_cast<const volatile void*>(&tinode_->offsets[broker].ordered));
	CXL::flush_cacheline(seq_region);
	
	// [[LIFECYCLE]] Clear flags and batch_complete so scanner skips slot (no VALID); keep num_msg so export can read metadata
	batch_to_order->batch_complete = 0;
	__atomic_store_n(&batch_to_order->flags, 0u, __ATOMIC_RELEASE);

	// BatchHeader is 128B (2 cachelines); flush both for non-coherent CXL visibility
	CXL::flush_cacheline(batch_to_order);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(batch_to_order) + 64);

	// Single fence for all flushes - reduces fence overhead
	CXL::store_fence();

	// Set up export chain (GOI equivalent)
	header_for_sub->batch_off_to_export = (reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(header_for_sub));
	header_for_sub->ordered = 1;

	header_for_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header_for_sub) + sizeof(BatchHeader));

	VLOG(3) << "Orderer5: Assigned batch-level order " << start_total_order 
			<< " to batch with " << num_messages << " messages from broker " << broker;
}
} // namespace Embarcadero


/**
 * Start/Stop GOIRecoveryThread (added in Topic constructor/destructor)
 * This is a placeholder - actual thread start/stop will be added to constructor/destructor
 */
