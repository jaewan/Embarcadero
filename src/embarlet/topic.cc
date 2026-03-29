#include "topic.h"
#include "../cxl_manager/scalog_local_sequencer.h"
#include "../cxl_manager/lazylog_local_sequencer.h"
#include "common/performance_utils.h"
#include "../common/wire_formats.h"
#include "../common/order_level.h"
#include "../common/env_flags.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>


namespace Embarcadero {

constexpr size_t kReplicationNotStarted = std::numeric_limits<size_t>::max();

static inline uint64_t SteadyNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

static bool ShouldEnableOrder5Trace() {
	static const bool enabled = ReadEnvBoolStrict("EMBARCADERO_ORDER5_TRACE", false);
	return enabled;
}

static bool ShouldEnableOrder5PhaseDiag() {
	static const bool enabled = ReadEnvBoolLenient("EMBARCADERO_ORDER5_PHASE_DIAG", false);
	return enabled;
}

static bool ShouldEnableFrontierTrace() {
	static const bool enabled = ReadEnvBoolLenient("EMBAR_FRONTIER_TRACE", false);
	return enabled;
}

enum Order5FlightKind : uint32_t {
	kOrder5FlightDriver = 1,
	kOrder5FlightCommit = 2,
	kOrder5FlightCV = 3,
	kOrder5FlightExpiry = 4,
	kOrder5FlightDisconnect = 5,
};

static const char* Order5FlightKindToString(uint32_t kind) {
	switch (kind) {
		case kOrder5FlightDriver: return "driver";
		case kOrder5FlightCommit: return "commit";
		case kOrder5FlightCV: return "cv";
		case kOrder5FlightExpiry: return "expiry";
		case kOrder5FlightDisconnect: return "disconnect";
		default: return "unknown";
	}
}

static inline bool ShouldCaptureOrder5Flight(int order, int broker_id) {
	return order == 5 && broker_id == 0;
}

static inline uint64_t MakeClientBrokerStreamKey(size_t client_id, int broker_id) {
	return (static_cast<uint64_t>(client_id) << 16) |
	       static_cast<uint16_t>(broker_id & 0xFFFF);
}

static inline bool UsesTrueClientChainOrdering(int order) {
	return order == kOrderStrong;
}

static uint64_t GetOrder5HoldTimeoutNs(bool replicated_ack2_mode) {
	if (const char* env = std::getenv("EMBARCADERO_ORDER5_HOLD_TIMEOUT_MS")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			return static_cast<uint64_t>(parsed) * 1000ULL * 1000ULL;
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_ORDER5_HOLD_TIMEOUT_MS='" << env
		             << "'; using runtime default";
	}
	// Keep the default hold timeout tight enough that replicated ORDER=5 runs do not
	// accumulate a large disconnect-time expiry wave. The force-expire window still
	// handles final tail drain explicitly.
	const uint64_t default_ms = 100ULL;
	return default_ms * 1000ULL * 1000ULL;
}

// Once an ORDER=5 batch is copied into the hold buffer, its original ring slot must stop
// looking publishable immediately. The eventual from-hold commit cannot safely clear p.hdr,
// because the ring slot may already have been reused by then.
static inline void InvalidateOrder5HeldSlot(BatchHeader* hdr) {
	if (!hdr) return;
	hdr->batch_complete = 0;
	__atomic_store_n(&hdr->flags, 0u, __ATOMIC_RELEASE);
	CXL::flush_cacheline(hdr);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(hdr) + 64);
	CXL::store_fence();
}

void Topic::RecordOrder5FlightEvent(
		uint32_t kind, uint32_t broker, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
	if (!ShouldCaptureOrder5Flight(order_, broker_id_)) return;
	const uint64_t pos = order5_flight_write_pos_.fetch_add(1, std::memory_order_relaxed);
	Order5FlightEvent& slot = order5_flight_ring_[pos % kOrder5FlightRingSize];
	slot.ts_ns = SteadyNowNs();
	slot.kind = kind;
	slot.broker = broker;
	slot.a = a;
	slot.b = b;
	slot.c = c;
	slot.d = d;
}

void Topic::DumpOrder5FlightRecorder(const char* reason) {
	if (!ShouldCaptureOrder5Flight(order_, broker_id_)) return;
	if (order5_flight_dumped_.test_and_set(std::memory_order_acq_rel)) return;

	const uint64_t end = order5_flight_write_pos_.load(std::memory_order_acquire);
	const uint64_t begin = (end > kOrder5FlightRingSize) ? (end - kOrder5FlightRingSize) : 0;
	LOG(ERROR) << "[ORDER5_FLIGHT_DUMP_BEGIN]"
	           << " reason=" << (reason ? reason : "unknown")
	           << " topic=" << topic_name_
	           << " broker=" << broker_id_
	           << " events=" << (end - begin)
	           << " epoch_index=" << epoch_index_.load(std::memory_order_acquire)
	           << " last_sequenced=" << last_sequenced_epoch_.load(std::memory_order_acquire)
	           << " hold_entries=" << GetTotalHoldBufferSize()
	           << " force_expire_until_ns=" << force_expire_hold_until_ns_.load(std::memory_order_acquire);
	for (uint64_t i = begin; i < end; ++i) {
		const Order5FlightEvent& ev = order5_flight_ring_[i % kOrder5FlightRingSize];
		LOG(ERROR) << "[ORDER5_FLIGHT]"
		           << " idx=" << i
		           << " kind=" << Order5FlightKindToString(ev.kind)
		           << " ts_ns=" << ev.ts_ns
		           << " broker=" << ev.broker
		           << " a=" << ev.a
		           << " b=" << ev.b
		           << " c=" << ev.c
		           << " d=" << ev.d;
	}
	LOG(ERROR) << "[ORDER5_FLIGHT_DUMP_END]"
	           << " reason=" << (reason ? reason : "unknown");
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
		for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
			scanner_pushed_batches_[i].store(0, std::memory_order_relaxed);
			scanner_pushed_msgs_[i].store(0, std::memory_order_relaxed);
			sequencer_committed_batches_[i].store(0, std::memory_order_relaxed);
			sequencer_committed_msgs_[i].store(0, std::memory_order_relaxed);
		}

		if (seq_type_ == CORFU && order_ != Embarcadero::kOrderTotal) {
			LOG(ERROR) << "Corfu supports only ORDER=2 in this implementation (got ORDER="
			          << order_ << ")";
		}

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
		} else if (seq_type == LAZYLOG) {
			if (replication_factor_ > 0) {
				scalog_replication_client_ = std::make_unique<Scalog::ScalogReplicationClient>(
					topic_name,
					replication_factor_,
					"localhost",
					broker_id_,
					LAZYLOG_REP_PORT
				);
				if (!scalog_replication_client_->Connect()) {
					LOG(ERROR) << "LazyLog replication client failed to connect to replica";
				}
			}
			GetCXLBufferFunc = &Topic::LazyLogGetCXLBuffer;
			} else {
				// Set buffer function based on order
				if (order_ == 3) {
					GetCXLBufferFunc = &Topic::Order3GetCXLBuffer;
				} else {
					GetCXLBufferFunc = &Topic::EmbarcaderoGetCXLBuffer;
				}
			}


		// Seed broker_epoch_ from the CXL ControlBlock so the first
		// CheckEpochOnce() does not falsely detect a stale epoch.
		// Without this, broker_epoch_ starts at 0 while CXL epoch is 1
		// (set by broker 0 at init), causing a guaranteed 100ms sleep on
		// every first batch across all brokers.
		if (cxl_addr_) {
			ControlBlock* cb = reinterpret_cast<ControlBlock*>(cxl_addr_);
			CXL::flush_cacheline(cb);
			CXL::load_fence();
			uint64_t initial_epoch = cb->epoch.load(std::memory_order_acquire);
			broker_epoch_.store(initial_epoch, std::memory_order_release);
		}

		// Constructor completes initialization without starting threads
		// Call Start() method separately to begin thread execution
}

void Topic::Start() {
	std::atomic_thread_fence(std::memory_order_seq_cst);
	// Start delegation thread if needed (Stage 2: Local Ordering)
	// [PHASE-1] Skip DelegationThread for the epoch-ordered modes and Order 2 with EMBARCADERO sequencer.
	// Reason 1: Order 4/5/2 subscribers use GetBatchToExportWithMetadata (GOI/CV-based),
	//           not GetMessageAddr (which needs DelegationThread's written/written_addr).
	// Reason 2: DelegationThread and BrokerScannerWorker5 both read the same PBR ring,
	//           causing CXL cache line invalidation ping-pong (~30% scanner latency hit).
	// Reason 3: DelegationThread has a ring-wrap bug (Phase 9) that doesn't affect Order 4/5/2
	//           if DelegationThread is disabled.
	bool skip_delegation = false;
	if (Embarcadero::UsesEpochSequencerPath(order_) && seq_type_ == EMBARCADERO) {
		skip_delegation = true;
		LOG(INFO) << "Topic " << topic_name_ << ": DelegationThread disabled for Order " << order_
		          << "(subscribers use GetBatchToExportWithMetadata; eliminates scanner contention)";
	}
	if (order_ == 2 && seq_type_ == EMBARCADERO) {
		skip_delegation = true;
		LOG(INFO) << "Topic " << topic_name_ << ": DelegationThread disabled for Order 2 "
		          << "(subscribers use GetBatchToExportWithMetadata)";
	}
	// Corfu ORDER=2: CorfuGetCXLBuffer returns batch_header_location=nullptr, so
	// ReservePBRSlotAfterRecv is never called and batch_complete is never set.
	// DelegationThread would spin-wait forever on batch_complete, wasting a full core
	// and generating spurious CXL cache-line traffic.
	if (order_ == 2 && seq_type_ == CORFU) {
		skip_delegation = true;
		LOG(INFO) << "Topic " << topic_name_ << ": DelegationThread disabled for Corfu Order 2 "
		          << "(PBR ring never written; delegation would busy-spin indefinitely)";
	}
	// [[PERF: ORDER=0 fast path]] When the fast path is enabled, PBR is never written,
	// so DelegationThread would spin-wait on CXL forever, wasting a CPU core and CXL bandwidth.
	const bool order0_fast_path_requested =
		ReadEnvBoolStrict("EMBARCADERO_ORDER0_FAST_PATH", true);
	const bool order0_fast_path_compatible =
		!(ack_level_ == 2 && replication_factor_ > 0);
	if (order_ == 0 && seq_type_ == EMBARCADERO &&
	    order0_fast_path_requested && order0_fast_path_compatible) {
		skip_delegation = true;
		LOG(INFO) << "Topic " << topic_name_ << ": DelegationThread disabled for Order 0 "
		          << "(fast path: network thread handles ACK cursor directly, no PBR)";
	} else if (order_ == 0 && seq_type_ == EMBARCADERO &&
	           order0_fast_path_requested && !order0_fast_path_compatible) {
		LOG(INFO) << "Topic " << topic_name_
		          << ": ORDER0 fast path disabled because ACK=2 with replication requires PBR/replication tracking";
	}
			if (!skip_delegation && seq_type_ != KAFKA) {
				delegationThreads_.emplace_back(&Topic::DelegationThread, this);
		}

	// Head node runs centralized sequencers; Scalog/LazyLog local sequencers run on every broker.
	if (broker_id_ == 0 || seq_type_ == SCALOG || seq_type_ == LAZYLOG) {
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
				else if (Embarcadero::UsesEpochSequencerPath(order_)){
					LOG(INFO) << "Creating Sequencer5 thread for order level " << order_;
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
			case LAZYLOG:
				if (order_ == Embarcadero::kOrderTotal){
					sequencerThread_ = std::thread(&Topic::StartLazyLogLocalSequencer, this);
				} else {
					LOG(ERROR) << "LazyLog baseline requires ORDER=2 (got ORDER=" << order_ << ")";
				}
				break;
			case CORFU:
				if (order_ == Embarcadero::kOrderTotal) {
					VLOG(3) << "Corfu running in canonical ORDER=2 mode.";
				} else {
					LOG(ERROR) << "Corfu supports only ORDER=2 in this implementation (got ORDER="
					           << order_ << ")";
				}
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

void Topic::StartLazyLogLocalSequencer() {
	BatchHeader* batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	LazyLog::LazyLogLocalSequencer lazylog_local_sequencer(tinode_, broker_id_, cxl_addr_, topic_name_, batch_header);
	lazylog_local_sequencer.SendLocalProgress(topic_name_, stop_threads_volatile_);
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
	// Delegation is batch-based only. Legacy message-by-message fallback was
	// removed to keep one maintained execution path.
	BatchHeader* current_batch = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);

	// [PHASE-8] Ring boundaries for wrap check (prevents OOB read after ring wrap)
	BatchHeader* delegation_ring_start = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	BatchHeader* delegation_ring_end = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(delegation_ring_start) + BATCHHEADERS_SIZE);

	// [[PERFORMANCE FIX]]: Batch flush optimization (DEV-002)
	// Flush every 8 batches or every 64KB of data, whichever comes first.
	constexpr size_t BATCH_FLUSH_INTERVAL = 8;
	constexpr size_t BYTE_FLUSH_INTERVAL = 64 * 1024;
	size_t batches_since_flush = 0;
	size_t bytes_since_flush = 0;

	while (!stop_threads_) {
		if (current_batch && __atomic_load_n(&current_batch->batch_complete, __ATOMIC_ACQUIRE)) {
			if (current_batch->num_msg > 0) {
				// [[BLOG_HEADER]] ORDER=0 and epoch-sequenced orders both skip per-message field writes when BlogHeader
				// is enabled. For ORDER=0, DelegationThread is disabled so this is a dead path, but
				// keeping the condition symmetric avoids surprise if the fast-path flag is toggled.
				bool skip_per_message_writes =
					((order_ == 0 || UsesEpochSequencerPath(order_)) && HeaderUtils::ShouldUseBlogHeader());

				if (!skip_per_message_writes) {
					MessageHeader* batch_first_msg = reinterpret_cast<MessageHeader*>(
						reinterpret_cast<uint8_t*>(cxl_addr_) + current_batch->log_idx);
					MessageHeader* msg_ptr = batch_first_msg;
					for (size_t i = 0; i < current_batch->num_msg; ++i) {
						msg_ptr->logical_offset = logical_offset_;
						msg_ptr->segment_header = reinterpret_cast<uint8_t*>(msg_ptr) - CACHELINE_SIZE;

						size_t current_padded_size = msg_ptr->paddedSize;
						const size_t min_msg_size = sizeof(MessageHeader);
						const size_t max_msg_size = 1024 * 1024;
						if (current_padded_size < min_msg_size || current_padded_size > max_msg_size) {
							static thread_local size_t error_count = 0;
							if (++error_count % 1000 == 1) {
								LOG(ERROR) << "DelegationThread: Invalid paddedSize=" << current_padded_size
								           << " for topic " << topic_name_ << ", broker " << broker_id_
								           << " (error #" << error_count << ")";
							}
							CXL::cpu_pause();
							break;
						}

						msg_ptr->next_msg_diff = current_padded_size;
						*reinterpret_cast<unsigned long long int*>(msg_ptr->segment_header) =
							static_cast<unsigned long long int>(
								reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(msg_ptr->segment_header));

						if (i < current_batch->num_msg - 1) {
							msg_ptr = reinterpret_cast<MessageHeader*>(
								reinterpret_cast<uint8_t*>(msg_ptr) + current_padded_size);
						}

						logical_offset_++;
					}

					if (order_ != 0) {
						const bool count_based_written =
							(seq_type_ == SCALOG || seq_type_ == LAZYLOG);
						size_t written_val = count_based_written ? logical_offset_ : logical_offset_ - 1;
						UpdateTInodeWritten(
							written_val,
							static_cast<unsigned long long int>(
								reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_)));
						if (seq_type_ == SCALOG || seq_type_ == LAZYLOG) {
							tinode_->offsets[broker_id_].validated_written_byte_offset =
								current_batch->log_idx + current_batch->total_size;
						}
					}
				} else {
					BlogMessageHeader* batch_first_msg = reinterpret_cast<BlogMessageHeader*>(
						reinterpret_cast<uint8_t*>(cxl_addr_) + current_batch->log_idx);
					BlogMessageHeader* msg_ptr = batch_first_msg;
					for (size_t i = 0; i < current_batch->num_msg; ++i) {
						if (msg_ptr->size > wire::MAX_MESSAGE_PAYLOAD_SIZE) {
							LOG(ERROR) << "DelegationThread: Message " << i
							           << " in batch has excessive payload size=" << msg_ptr->size
							           << " (max=" << wire::MAX_MESSAGE_PAYLOAD_SIZE << "), aborting batch";
							break;
						}

						size_t header_size = sizeof(BlogMessageHeader);
						size_t padded_size = wire::ComputeMessageStride(header_size, msg_ptr->size);
						if (padded_size < 64 || padded_size > wire::MAX_MESSAGE_PAYLOAD_SIZE + header_size + 64) {
							LOG(ERROR) << "DelegationThread: Message " << i
							           << " computed invalid stride=" << padded_size
							           << " from payload_size=" << msg_ptr->size << ", aborting batch";
							break;
						}

						if (i > 0) {
							size_t batch_end_estimate = current_batch->log_idx + current_batch->total_size;
							size_t msg_end = static_cast<size_t>(
								reinterpret_cast<uint8_t*>(msg_ptr) + padded_size - reinterpret_cast<uint8_t*>(cxl_addr_));
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
						if (i < current_batch->num_msg - 1) {
							msg_ptr = reinterpret_cast<BlogMessageHeader*>(
								reinterpret_cast<uint8_t*>(msg_ptr) + padded_size);
						}
					}

					if (order_ != 0) {
						const bool count_based_written =
							(seq_type_ == SCALOG || seq_type_ == LAZYLOG);
						size_t written_val = count_based_written ? logical_offset_ : logical_offset_ - 1;
						UpdateTInodeWritten(
							written_val,
							static_cast<unsigned long long int>(
								reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_)));
						if (seq_type_ == SCALOG || seq_type_ == LAZYLOG) {
							tinode_->offsets[broker_id_].validated_written_byte_offset =
								current_batch->log_idx + current_batch->total_size;
						}
					}
				}
			}

			batches_since_flush++;
			bytes_since_flush += current_batch->total_size;
			if (batches_since_flush >= BATCH_FLUSH_INTERVAL || bytes_since_flush >= BYTE_FLUSH_INTERVAL) {
				CXL::flush_cacheline(const_cast<const void*>(static_cast<volatile void*>(&tinode_->offsets[broker_id_])));
				CXL::store_fence();
				batches_since_flush = 0;
				bytes_since_flush = 0;
			}

			BatchHeader* next_batch = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(current_batch) + sizeof(BatchHeader));
			if (next_batch >= delegation_ring_end) {
				next_batch = delegation_ring_start;
			}
			current_batch = next_batch;
			continue;
		}

		CXL::cpu_pause();
	}
}

void Topic::GetRegisteredBrokerSet(absl::btree_set<int>& registered_brokers){
	//TODO(Jae) Placeholder
	if (!get_registered_brokers_callback_(registered_brokers, nullptr /* msg_to_order removed */, tinode_)) {
		LOG(ERROR) << "GetRegisteredBrokerSet: Callback failed to get registered brokers.";
		registered_brokers.clear(); // Ensure set is empty on failure
	}
}

// [[ORDER5_PERF_GUARD]]
// Legacy Order4 implementation is intentionally kept in this TU.
// Runtime now routes ORDER=4 through the epoch sequencer, but removing these functions
// still changed code layout enough to regress throughput in profiling.
__attribute__((cold, noinline)) void Topic::Sequencer4() {
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

	// Per-client ACK tracking: batch-level update (outside message loop to avoid per-message lock overhead)
	UpdatePerClientOrdered(static_cast<uint32_t>(batch_to_order->client_id), num_messages);

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
	if (num_slots_ == 0) {
		LOG(ERROR) << "Corfu ORDER=2: num_slots_ is 0; cannot place batch metadata.";
		return nullptr;
	}

	// Calculate addresses
	const unsigned long long int segment_metadata =
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;
	BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);
	const size_t slot = batch_header.batch_seq % num_slots_;
	BatchHeader* slot_header = &batch_header_log[slot];

	// Get log address with batch offset
	log = reinterpret_cast<void*>(log_addr_.load()
			+ batch_header.log_idx);

	// Check for segment boundary issues
	CheckSegmentBoundary(log, msg_size, segment_metadata);

	slot_header->batch_seq = batch_header.batch_seq;
	slot_header->pbr_absolute_index = batch_header.batch_seq;
	slot_header->total_size = batch_header.total_size;
	slot_header->num_msg = batch_header.num_msg;
	slot_header->broker_id = broker_id_;
	slot_header->ordered = 0;
	slot_header->batch_off_to_export = 0;
	slot_header->total_order = batch_header.total_order;
	slot_header->log_idx = static_cast<size_t>(
			reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_)
			);
	CXL::flush_cacheline(slot_header);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(slot_header) + 64);
	CXL::store_fence();

	// Return replication/completion callback.
	// NOTE: log_ptr (the first parameter) is always nullptr for Corfu — the network
	// manager passes `(void*)header` where `header` is only set in the KAFKA parse
	// block, which is skipped for CORFU.  All data access uses the captured `log`
	// pointer (set above from log_addr_ + batch_header.log_idx).
	return [this, batch_header, log](void* /*log_ptr*/, size_t /*placeholder*/) {
		// Handle replication if needed
		if (replication_factor_ > 0 && corfu_replication_client_) {
			const bool replicated = corfu_replication_client_->ReplicateData(
					batch_header.log_idx,
					batch_header.total_size,
					log
					);
			if (!replicated) {
				LOG(ERROR) << "CORFU replication failed for batch_seq=" << batch_header.batch_seq
				           << " log_idx=" << batch_header.log_idx
				           << " size=" << batch_header.total_size
				           << " client_id=" << batch_header.client_id
				           << "; not advancing replication_done / durable frontier.";
				return;
			}
			RecordCorfuOrder2DurableCompletion(
					batch_header.batch_seq, batch_header.num_msg, batch_header.client_id);
		} // end: if (replication_factor_ > 0 && corfu_replication_client_)

		RecordCorfuOrder2BatchCompletion(batch_header.batch_seq, batch_header.num_msg, batch_header.client_id);
	};
}

void Topic::RecordCorfuOrder2BatchCompletion(uint64_t batch_seq, uint32_t num_msg, uint32_t client_id) {
	if (seq_type_ != CORFU || order_ != Embarcadero::kOrderTotal || num_slots_ == 0) {
		return;
	}

	uint64_t contiguous_advanced = 0;
	uint64_t messages_to_ack = 0;
	// Per-client deltas collected while holding order2_mu_; applied to per-client map separately
	// to keep the two locks independent (no nested lock acquisition).
	absl::flat_hash_map<uint32_t, uint64_t> per_client_delta;
	{
		absl::MutexLock lock(&corfu_order2_mu_);
		auto [it, inserted] = corfu_order2_completed_.emplace(batch_seq, std::make_pair(num_msg, client_id));
		if (!inserted) {
			LOG(WARNING) << "Corfu ORDER=2: duplicate completion for batch_seq=" << batch_seq;
			return;
		}

		while (true) {
			auto next_it = corfu_order2_completed_.find(corfu_order2_next_seq_);
			if (next_it == corfu_order2_completed_.end()) {
				break;
			}
			const uint32_t slot_num_msg = next_it->second.first;
			const uint32_t slot_client_id = next_it->second.second;
			const size_t slot = static_cast<size_t>(corfu_order2_next_seq_ % num_slots_);
			BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);
			BatchHeader* slot_header = &batch_header_log[slot];
			CXL::flush_cacheline(slot_header);
			CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(slot_header) + 64);
			CXL::full_fence();
			if (slot_header->pbr_absolute_index != corfu_order2_next_seq_) {
				// The CXL metadata slot was overwritten by a newer batch that mapped to the same
				// ring slot (num_slots_ too small for outstanding window).  We still know num_msg
				// from corfu_order2_completed_, so we can advance the ordered cursor to keep the
				// ACK path moving — the message data itself is intact in BLog.
				// Do NOT set slot_header->ordered here; that slot now belongs to a different batch.
				static std::atomic<uint64_t> freshness_errors{0};
				uint64_t err_n = freshness_errors.fetch_add(1, std::memory_order_relaxed) + 1;
				LOG(ERROR) << "Corfu ORDER=2 slot freshness mismatch #" << err_n
				           << ": slot=" << slot
				           << " expected_seq=" << corfu_order2_next_seq_
				           << " observed_seq=" << slot_header->pbr_absolute_index
				           << " — num_slots_=" << num_slots_ << " is too small; increase ring size."
				           << " Advancing ordered cursor using map num_msg to avoid ACK deadlock.";
				// Advance anyway using the num_msg we registered — avoids permanent ACK freeze.
				messages_to_ack += slot_num_msg;
				per_client_delta[slot_client_id] += slot_num_msg;
				corfu_order2_completed_.erase(next_it);
				corfu_order2_next_seq_++;
				contiguous_advanced++;
				continue;
			}

			messages_to_ack += slot_num_msg;
			per_client_delta[slot_client_id] += slot_num_msg;
			slot_header->ordered = 1;
			CXL::flush_cacheline(slot_header);
			CXL::store_fence();

			corfu_order2_completed_.erase(next_it);
			corfu_order2_next_seq_++;
			contiguous_advanced++;
		}
	}

	if (messages_to_ack > 0) {
		// Update global ordered counter (used by subscribers to track broker-wide progress).
		tinode_->offsets[broker_id_].ordered += messages_to_ack;
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(&tinode_->offsets[broker_id_].ordered)));
		CXL::store_fence();

		// Update per-client ordered counters (used by per-publisher AckThread for correct ACK).
		// Held separately from corfu_order2_mu_ to avoid nested locking.
		for (auto& [cid, cnt] : per_client_delta) {
			UpdatePerClientOrdered(cid, cnt);
		}
	}

	if (contiguous_advanced > 0 && VLOG_IS_ON(4)) {
		VLOG(4) << "Corfu ORDER=2 advanced contiguous frontier by " << contiguous_advanced
		        << " batches, acked_messages+=" << messages_to_ack
		        << " next_seq=" << corfu_order2_next_seq_;
	}
}

void Topic::RecordCorfuOrder2DurableCompletion(uint64_t batch_seq, uint32_t num_msg, uint32_t client_id) {
	if (seq_type_ != CORFU || order_ != Embarcadero::kOrderTotal || replication_factor_ <= 0) {
		return;
	}

	uint64_t messages_to_ack = 0;
	absl::flat_hash_map<uint32_t, uint64_t> per_client_delta;
	{
		absl::MutexLock lock(&corfu_order2_durable_mu_);
		auto [it, inserted] =
				corfu_order2_durable_completed_.emplace(batch_seq, std::make_pair(num_msg, client_id));
		if (!inserted) {
			LOG(WARNING) << "Corfu ORDER=2 durable: duplicate completion for batch_seq=" << batch_seq;
			return;
		}

		while (true) {
			auto next_it = corfu_order2_durable_completed_.find(corfu_order2_durable_next_seq_);
			if (next_it == corfu_order2_durable_completed_.end()) {
				break;
			}
			const uint32_t slot_num_msg = next_it->second.first;
			const uint32_t slot_client_id = next_it->second.second;
			messages_to_ack += slot_num_msg;
			per_client_delta[slot_client_id] += slot_num_msg;
			corfu_order2_durable_completed_.erase(next_it);
			corfu_order2_durable_next_seq_++;
		}
	}

	if (messages_to_ack == 0) {
		return;
	}

	// Advance durable frontier only for contiguous durable completions.
	const uint64_t start =
			corfu_ack2_durable_count_.fetch_add(messages_to_ack, std::memory_order_relaxed);
	const uint64_t last_offset = start + messages_to_ack - 1;

	// Mark replication_done using canonical replication set indices.
	int num_brokers = get_num_brokers_callback_();
	for (int i = 0; i < replication_factor_; i++) {
		int b = Embarcadero::GetReplicationSetBroker(
				broker_id_, replication_factor_, num_brokers, i);
		if (tinode_->replicate_tinode) {
			replica_tinode_->offsets[b].replication_done[broker_id_] = last_offset;
			CXL::flush_cacheline(const_cast<const void*>(
					reinterpret_cast<const volatile void*>(
							&replica_tinode_->offsets[b].replication_done[broker_id_])));
		}
		tinode_->offsets[b].replication_done[broker_id_] = last_offset;
		CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(
				&tinode_->offsets[b].replication_done[broker_id_])));
	}
	CXL::store_fence();

	for (auto& [cid, cnt] : per_client_delta) {
		UpdatePerClientDurable(cid, cnt);
	}
}

void Topic::UpdatePerClientOrdered(uint32_t client_id, uint64_t count) {
	absl::MutexLock lock(&per_client_mu_);
	per_client_ordered_[client_id] += count;
}

uint64_t Topic::GetClientOrdered(uint32_t client_id) const {
	absl::MutexLock lock(&per_client_mu_);
	auto it = per_client_ordered_.find(client_id);
	return (it != per_client_ordered_.end()) ? it->second : 0;
}

uint64_t Topic::GetClientDurable(uint32_t client_id) const {
	absl::MutexLock lock(&per_client_durable_mu_);
	auto it = per_client_durable_.find(client_id);
	return (it != per_client_durable_.end()) ? it->second : 0;
}

bool Topic::SupportsPerClientAckLevel1() const {
	// ACK level 1 is "ordered frontier" semantics.
	// We currently maintain per-client ordered frontier in:
	// - CORFU ORDER=2 via RecordCorfuOrder2BatchCompletion
	// - EMBARCADERO ORDER=4 via AssignOrder
	//
	// ORDER=5 is different: ordered frontiers are advanced from shared CXL state across broker
	// processes, but per_client_ordered_ lives only in the local broker process. Non-owner
	// brokers can therefore observe tinode/CV progress without having a populated local
	// per-client map, which would pin ACK1 at zero. Until per-client ORDER=5 frontiers are
	// published in shared memory, fall back to the legacy shared frontier path for ACK1.
	if (seq_type_ == CORFU) {
		return order_ == Embarcadero::kOrderTotal;
	}
	if (seq_type_ == EMBARCADERO) {
		return order_ == Embarcadero::kOrderPerBroker;
	}
	return false;
}

bool Topic::SupportsPerClientAckLevel2Durable() const {
	// Durability attribution is currently explicit in CORFU path where replication callback
	// advances per-client durable frontier only after replication_done writes are committed.
	return seq_type_ == CORFU &&
	       order_ == Embarcadero::kOrderTotal &&
	       replication_factor_ > 0 &&
	       corfu_replication_client_ != nullptr;
}

void Topic::UpdatePerClientDurable(uint32_t client_id, uint64_t count) {
	absl::MutexLock lock(&per_client_durable_mu_);
	per_client_durable_[client_id] += count;
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

// [[ORDER5_PERF_GUARD]] See Sequencer4() note above.
__attribute__((cold, noinline)) std::function<void(void*, size_t)> Topic::Order4GetCXLBuffer(
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

    static const bool kCxlScalogMode =
        (getenv("SCALOG_CXL_MODE") != nullptr &&
         std::string(getenv("SCALOG_CXL_MODE")) == "1");

    uint64_t pbr_idx = broker_pbr_counters_[broker_id_].fetch_add(1, std::memory_order_relaxed);
    batch_header.pbr_absolute_index = pbr_idx;
    batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) | pbr_idx;

    BatchHeader* batch_header_ring = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
    size_t num_slots = BATCHHEADERS_SIZE / sizeof(BatchHeader);
    size_t slot_idx = static_cast<size_t>(pbr_idx % num_slots);
    batch_header_location = &batch_header_ring[slot_idx];
    __atomic_store_n(&batch_header_location->batch_complete, 0, __ATOMIC_RELEASE);

	// Calculate addresses
	const unsigned long long int segment_metadata =
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;

	// Allocate space in log
	log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
    batch_header.log_idx = reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_);

	// Check for segment boundary
	CheckSegmentBoundary(log, msg_size, segment_metadata);

    size_t rep_offset = 0;
    if (!kCxlScalogMode) {
        rep_offset = scalog_batch_offset_.fetch_add(batch_header.total_size, std::memory_order_relaxed);
    }

	// Return replication callback
	return [this, batch_header, log, rep_offset, kCxlScalogMode](void* log_ptr, size_t /*placeholder*/) {
		if (kCxlScalogMode) {
			return;
		}
		// Handle replication if needed
		if (replication_factor_ > 0 && scalog_replication_client_) {
				scalog_replication_client_->ReplicateData(
						rep_offset,
						batch_header.total_size,
						batch_header.num_msg,
						log
				);
		}
	};
}

std::function<void(void*, size_t)> Topic::LazyLogGetCXLBuffer(
		BatchHeader &batch_header,
		const char topic[TOPIC_NAME_SIZE],
		void* &log,
		void* &segment_header,
		size_t &logical_offset,
		BatchHeader* &batch_header_location,
		bool /*epoch_already_checked*/) {

	static const bool kCxlLazyLogMode =
		(getenv("LAZYLOG_CXL_MODE") != nullptr &&
		 std::string(getenv("LAZYLOG_CXL_MODE")) == "1");

	uint64_t pbr_idx = broker_pbr_counters_[broker_id_].fetch_add(1, std::memory_order_relaxed);
	batch_header.pbr_absolute_index = pbr_idx;
	batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) | pbr_idx;

	BatchHeader* batch_header_ring = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	size_t num_slots = BATCHHEADERS_SIZE / sizeof(BatchHeader);
	size_t slot_idx = static_cast<size_t>(pbr_idx % num_slots);
	batch_header_location = &batch_header_ring[slot_idx];
	__atomic_store_n(&batch_header_location->batch_complete, 0, __ATOMIC_RELEASE);

	const unsigned long long int segment_metadata =
		reinterpret_cast<unsigned long long int>(current_segment_);
	const size_t msg_size = batch_header.total_size;

	log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
	batch_header.log_idx = reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_);
	CheckSegmentBoundary(log, msg_size, segment_metadata);

	size_t rep_offset = 0;
	if (!kCxlLazyLogMode) {
		rep_offset = scalog_batch_offset_.fetch_add(batch_header.total_size, std::memory_order_relaxed);
	}

	return [this, batch_header, log, rep_offset, kCxlLazyLogMode](void* /*log_ptr*/, size_t /*placeholder*/) {
		if (kCxlLazyLogMode) {
			return;
		}
		if (replication_factor_ > 0 && scalog_replication_client_) {
			scalog_replication_client_->ReplicateData(
					rep_offset,
					batch_header.total_size,
					batch_header.num_msg,
					log);
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

	// [[PERF FIX]] Ring gating check moved OUTSIDE mutex for ORDER=0.
	// In lock-free PBR mode, pre-recv approximate gating can false-positive because
	// cached_next_slot_offset_ is not the authoritative producer cursor.
	// Use ReservePBRSlotLockFree() as the single source of truth for slot admission.
	bool skip_ring_gating = (order_ == 0 || use_lock_free_pbr_);
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
	if (broker_id_ == 0 && TopicDiagnosticsEnabled()) {
		static std::atomic<uint64_t> b0_pbr_claimed_log{0};
		uint64_t n = b0_pbr_claimed_log.fetch_add(1, std::memory_order_relaxed);
		size_t slot_off = reinterpret_cast<uint8_t*>(batch_headers_log) - reinterpret_cast<uint8_t*>(first_batch_headers_addr_);
		if (n < 10 || (n % 5000 == 0 && n > 0))
			LOG(INFO) << "[B0_PBR_WRITE] slot_offset=" << slot_off << " flags=CLAIMED written";
	}
	batch_header_location = reinterpret_cast<BatchHeader*>(batch_headers_log);
	return true;
}

bool Topic::PublishPBRSlotDirect(const BatchHeader& batch_header, BatchHeader* batch_header_location) {
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

bool Topic::PublishPBRSlotAfterRecv(const BatchHeader& batch_header, BatchHeader* batch_header_location) {
	if (!batch_header_location) return false;

	if (seq_type_ == EMBARCADERO && Embarcadero::UsesEpochSequencerPath(order_)) {
		// ORDER=4/5 already tolerate out-of-arrival-order batches in the sequencer via hold/expiry
		// hold/expiry logic. Delaying slot publication until pbr_absolute_index becomes contiguous
		// can strand a large tail of fully received batches behind one missing index. Make each
		// completed batch visible immediately and let the sequencer/CV path recover contiguity.
		return PublishPBRSlotDirect(batch_header, batch_header_location);
	}

	return PublishPBRSlotDirect(batch_header, batch_header_location);
}

void Topic::RequestOrder5HoldExpiryOnce() {
	if (seq_type_ == EMBARCADERO && Embarcadero::UsesEpochSequencerPath(order_)) {
		const uint64_t kDisconnectForceExpireWindowNs =
			(replication_factor_ > 0) ? 90'000'000'000ULL : 2'500'000'000ULL;
		const uint64_t new_deadline = SteadyNowNs() + kDisconnectForceExpireWindowNs;
		uint64_t cur_deadline = force_expire_hold_until_ns_.load(std::memory_order_acquire);
		while (cur_deadline < new_deadline &&
		       !force_expire_hold_until_ns_.compare_exchange_weak(
		           cur_deadline, new_deadline, std::memory_order_release, std::memory_order_acquire)) {
		}

		// A disconnect often means publishers have finished sending and the last useful work
		// is sitting in the current COLLECTING epoch. Nudge the epoch state machine forward so
		// tail batches do not wait for a later shutdown-only drain path.
		uint64_t cur_epoch = epoch_index_.load(std::memory_order_acquire);
		EpochBuffer5& cur_buf = epoch_buffers_[cur_epoch % 3];
		if (ShouldEnableOrder5Trace()) {
			LOG(INFO) << "[ORDER5_TRACE_DISCONNECT_DRAIN_REQUEST]"
			          << " deadline_ns=" << new_deadline
			          << " cur_epoch=" << cur_epoch
			          << " cur_state=" << static_cast<int>(cur_buf.state.load(std::memory_order_acquire));
		}
		RecordOrder5FlightEvent(
			kOrder5FlightDisconnect,
			static_cast<uint32_t>(broker_id_),
			new_deadline,
			cur_epoch,
			static_cast<uint64_t>(cur_buf.state.load(std::memory_order_acquire)),
			force_expire_hold_until_ns_.load(std::memory_order_acquire));
		const EpochBuffer5::State cur_state = cur_buf.state.load(std::memory_order_acquire);
		auto advance_to_successor_epoch = [&](uint64_t base_epoch) {
			const uint64_t next_epoch = base_epoch + 1;
			EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];
			EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
			if (next_state == EpochBuffer5::State::IDLE) {
				next_buf.reset_and_start();
				next_state = next_buf.state.load(std::memory_order_acquire);
			}
			if (next_state == EpochBuffer5::State::COLLECTING ||
			    next_state == EpochBuffer5::State::SEALED) {
				uint64_t expected = base_epoch;
				epoch_index_.compare_exchange_strong(
					expected, next_epoch, std::memory_order_release, std::memory_order_acquire);
			}
			return next_state;
		};
		if (cur_state == EpochBuffer5::State::IDLE) {
			// ACK-stall disconnect nudges can arrive after ingress has quiesced and the epoch driver has
			// already fallen back to an IDLE buffer. In that state no new sealed epoch is produced, so
			// hold/deferred-only work never gets another sequencer pass. Bootstrap an empty epoch here so
			// the sequencer can run ProcessLevel5Batches() and drain held tail state without new ingress.
			if (cur_buf.reset_and_start() && cur_buf.seal()) {
				const EpochBuffer5::State next_state = advance_to_successor_epoch(cur_epoch);
				if (ShouldEnableOrder5Trace()) {
					LOG(INFO) << "[ORDER5_TRACE_DISCONNECT_IDLE_BOOTSTRAP]"
					          << " sealed_epoch=" << cur_epoch
					          << " next_epoch=" << (cur_epoch + 1)
					          << " next_state=" << static_cast<int>(next_state);
				}
			}
			return;
		}
		if (cur_state == EpochBuffer5::State::SEALED) {
			const EpochBuffer5::State next_state = advance_to_successor_epoch(cur_epoch);
			if (ShouldEnableOrder5Trace()) {
				LOG(INFO) << "[ORDER5_TRACE_DISCONNECT_SEALED_ADVANCE]"
				          << " sealed_epoch=" << cur_epoch
				          << " next_epoch=" << (cur_epoch + 1)
				          << " next_state=" << static_cast<int>(next_state);
			}
			return;
		}
		if (cur_state == EpochBuffer5::State::COLLECTING &&
		    cur_buf.seal()) {
			const uint64_t next_epoch = cur_epoch + 1;
			EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];
			EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
			if (next_state == EpochBuffer5::State::IDLE) {
				next_buf.reset_and_start();
				next_state = next_buf.state.load(std::memory_order_acquire);
			}
			if (next_state == EpochBuffer5::State::COLLECTING ||
			    next_state == EpochBuffer5::State::SEALED) {
				uint64_t expected = cur_epoch;
				epoch_index_.compare_exchange_strong(
					expected, next_epoch, std::memory_order_release, std::memory_order_acquire);
			}
			if (ShouldEnableOrder5Trace()) {
				LOG(INFO) << "[ORDER5_TRACE_DISCONNECT_DRAIN]"
				          << " sealed_epoch=" << cur_epoch
				          << " next_epoch=" << next_epoch
				          << " next_state=" << static_cast<int>(next_state);
			}
		}
	}
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
 * Used by legacy export/readers that consume ordered batch headers.
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
	// Corfu ORDER=2 does not advance CompletionVector like ORDER=5/3 paths.
	// Fall back to legacy ordered-slot export using monotonic expected_batch_offset.
	if (seq_type_ == CORFU && order_ == 2 && num_slots_ > 0 && cxl_addr_) {
		BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
		size_t slot = expected_batch_offset % num_slots_;
		BatchHeader* header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * slot);
		CXL::flush_cacheline(header);
		CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(header) + 64);
		CXL::full_fence();
		if (header->pbr_absolute_index != expected_batch_offset) {
			if (header->pbr_absolute_index > expected_batch_offset) {
				// Gap: later batch arrived, wait for missing sequence.
				return false;
			}
			LOG(ERROR) << "Corfu ORDER=2 export freshness violation: slot=" << slot
			           << " expected_seq=" << expected_batch_offset
			           << " observed_seq=" << header->pbr_absolute_index;
			return false;
		}
		if (header->ordered == 0 || header->num_msg == 0 || header->total_size == 0) {
			return false;
		}
		batch_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + header->log_idx;
		batch_size = header->total_size;
		batch_total_order = header->total_order;
		num_messages = header->num_msg;
		expected_batch_offset++;
		return true;
	}
	// LazyLog ORDER=2 currently exports from the ordered slot ring and does not
	// drive CompletionVector progress. Reuse legacy slot export and derive
	// metadata directly from the batch payload.
	if (seq_type_ == LAZYLOG && order_ == Embarcadero::kOrderTotal) {
		if (!GetBatchToExport(expected_batch_offset, batch_addr, batch_size)) {
			return false;
		}
		if (batch_addr == nullptr || batch_size < sizeof(MessageHeader)) {
			return false;
		}

		auto* first = reinterpret_cast<MessageHeader*>(batch_addr);
		batch_total_order = first->total_order;

		uint32_t counted = 0;
		size_t remaining = batch_size;
		uint8_t* cursor = reinterpret_cast<uint8_t*>(batch_addr);
		while (remaining >= sizeof(MessageHeader)) {
			auto* hdr = reinterpret_cast<MessageHeader*>(cursor);
			const size_t stride = static_cast<size_t>(hdr->next_msg_diff);
			if (stride == 0 || stride > remaining) {
				break;
			}
			++counted;
			cursor += stride;
			remaining -= stride;
		}
		num_messages = (counted > 0) ? counted : 1;
		return true;
	}

	const bool trace_order5 = ShouldEnableOrder5Trace() && (order_ == 5);
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
	// [[EXPORT_BY_TOTAL_ORDER]] [PHASE-8] Hold queue is peeked/popped atomically later.
	bool have_hold = false;
	size_t hold_total_order = 0;
	OrderedHoldExportEntry hold_front;

	// [[PHASE_2_CV_EXPORT]] O(1) export: use CompletionVector instead of linear scan (design §3.4).
	// expected_batch_offset = next PBR absolute index to export (monotonic).
	bool have_ring = false;
	size_t ring_total_order = 0;
	void* ring_batch_addr = nullptr;
	size_t ring_batch_size = 0;
	uint32_t ring_num_messages = 0;
	size_t next_pbr = expected_batch_offset;
	uint64_t cv_completed_pbr_head = kNoProgress;
	uint64_t cv_completed_logical = 0;
	uint64_t ring_slot_ordered = 0;
	uint64_t ring_slot_pbr_idx = 0;
	uint32_t ring_slot_flags = 0;
	uint32_t ring_slot_num_msg = 0;

	if (num_slots_ > 0 && cxl_addr_) {
		CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
		CompletionVectorEntry* my_cv = &cv[broker_id_];
		CXL::flush_cacheline(my_cv);
		CXL::full_fence();  // MFENCE orders CLFLUSHOPT before loads (per Intel SDM §11.12)
		cv_completed_pbr_head = my_cv->completed_pbr_head.load(std::memory_order_acquire);
		cv_completed_logical = my_cv->completed_logical_offset.load(std::memory_order_acquire);

		if (cv_completed_pbr_head != kNoProgress && next_pbr <= cv_completed_pbr_head) {
			size_t slot = next_pbr % num_slots_;
			BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
			BatchHeader* header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * slot);
			CXL::flush_cacheline(header);
			CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(header) + 64);
			CXL::full_fence();  // MFENCE required: CLFLUSHOPT only ordered by MFENCE (Intel SDM §8.2.5)
			ring_slot_ordered = header->ordered;
			ring_slot_pbr_idx = header->pbr_absolute_index;
			ring_slot_flags = header->flags;
			ring_slot_num_msg = header->num_msg;
			// ORDER=5 export readiness is driven by CompletionVector (sequencer completion),
			// not by local per-slot ordered bit on non-head brokers.
			// Non-head brokers can have ordered==0 while CV has already advanced.
			if (header->pbr_absolute_index == next_pbr &&
			    header->num_msg > 0 &&
			    header->total_size > 0) {
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

					ring_batch_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + actual->log_idx;
					ring_batch_size = actual->total_size;
					ring_num_messages = actual->num_msg;
				have_ring = true;
			}
		}
	}

	auto trace_export = [&](const char* result, const char* source) {
		if (!trace_order5) return;
		static thread_local size_t last_next_pbr = static_cast<size_t>(-1);
		static thread_local uint64_t last_cv_pbr = kNoProgress;
		static thread_local uint64_t last_cv_logical = static_cast<uint64_t>(-1);
		static thread_local uint64_t last_slot_ordered = static_cast<uint64_t>(-1);
		static thread_local uint64_t last_slot_pbr = static_cast<uint64_t>(-1);
		static thread_local uint32_t last_slot_flags = 0;
		static thread_local uint32_t last_slot_num_msg = 0;
		static thread_local uint64_t last_log_ns = 0;
		const uint64_t now_ns = SteadyNowNs();
		const bool changed =
			(next_pbr != last_next_pbr) ||
			(cv_completed_pbr_head != last_cv_pbr) ||
			(cv_completed_logical != last_cv_logical) ||
			(ring_slot_ordered != last_slot_ordered) ||
			(ring_slot_pbr_idx != last_slot_pbr) ||
			(ring_slot_flags != last_slot_flags) ||
			(ring_slot_num_msg != last_slot_num_msg);
		const bool periodic = (now_ns - last_log_ns) >= 200000000ULL;  // 200ms
		if (!changed && !periodic) return;
		LOG(INFO) << "[ORDER5_TRACE_EXPORT B" << broker_id_ << "]"
		          << " result=" << result
		          << " source=" << source
		          << " next_pbr=" << next_pbr
		          << " cv_pbr_head=" << cv_completed_pbr_head
		          << " cv_logical=" << cv_completed_logical
		          << " have_hold=" << (have_hold ? 1 : 0)
		          << " have_ring=" << (have_ring ? 1 : 0)
		          << " slot_ordered=" << ring_slot_ordered
		          << " slot_pbr_idx=" << ring_slot_pbr_idx
		          << " slot_flags=" << ring_slot_flags
		          << " slot_num_msg=" << ring_slot_num_msg
		          << " expected_batch_offset=" << expected_batch_offset;
		last_next_pbr = next_pbr;
		last_cv_pbr = cv_completed_pbr_head;
		last_cv_logical = cv_completed_logical;
		last_slot_ordered = ring_slot_ordered;
		last_slot_pbr = ring_slot_pbr_idx;
		last_slot_flags = ring_slot_flags;
		last_slot_num_msg = ring_slot_num_msg;
		last_log_ns = now_ns;
	};

	// Choose hold-vs-ring with hold entries tied to expected PBR frontier.
	// Held batches are the export source for slots that were drained from hold after the
	// ring cursor had already moved on, so we must never discard them just because the
	// subscriber cursor/CV has advanced past their PBR index.
	bool return_hold = false;
	bool return_late_hold = false;
	while (true) {
		{
			absl::MutexLock lock(&hold_export_queues_[broker_id_].mu);
			have_hold = false;
			if (!hold_export_queues_[broker_id_].q.empty()) {
				hold_front = hold_export_queues_[broker_id_].q.front();
				hold_total_order = hold_front.total_order;
				have_hold = true;
				if (hold_front.pbr_absolute_index < next_pbr) {
					// This held batch may still be the only exportable copy. Serve it as a
					// late hold instead of dropping it; keep the cursor monotonic below.
					hold_export_queues_[broker_id_].q.pop_front();
					return_hold = true;
					return_late_hold = true;
				} else if (hold_front.pbr_absolute_index == next_pbr &&
				           (!have_ring || hold_total_order <= ring_total_order)) {
					hold_export_queues_[broker_id_].q.pop_front();
					return_hold = true;
				}
			}
		}
		break;
	}
	if (return_hold) {
		batch_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + hold_front.log_idx;
		batch_size = hold_front.batch_size;
		batch_total_order = hold_front.total_order;
		num_messages = hold_front.num_messages;
		expected_batch_offset = std::max(next_pbr, hold_front.pbr_absolute_index + 1);
		trace_export("hit", return_late_hold ? "hold_late" : "hold");
		return true;
	}
	if (have_ring) {
		batch_addr = ring_batch_addr;
		batch_size = ring_batch_size;
		batch_total_order = ring_total_order;
		num_messages = ring_num_messages;
		expected_batch_offset = next_pbr + 1;
		trace_export("hit", "ring");
		return true;
	}

	trace_export("miss", "none");

	return false;
}

void Topic::AdvanceCVForSequencer(uint16_t broker_id, uint64_t pbr_index, uint64_t cumulative_msg_count) {
	// [[PHASE_2_CV_EXPORT]] ack_level=1: sequencer advances CV so export can proceed without waiting for replication.
	// IMPORTANT: topic-level ack_level is not a safe gate here (topics can outlive client ack-level mix).
	// Sequencer progress must not depend on a per-client ACK policy.

	// [[B0_ACK_FIX]] Advance on cumulative_msg_count (ACK offset), not pbr_index. Late-arriving batches
	// (e.g. seq=0 after seq=1,2 expired) have lower pbr_index but must still advance ACK so broker sees progress.
	// For replicated topics, the chain-replication tail exclusively owns completed_* durability fields.
	// Letting the sequencer also write completed_pbr_head races tail ACK2 updates on the same CXL line and
	// can publish a newer PBR head alongside an older durable logical frontier.
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	CompletionVectorEntry* entry = &cv[broker_id];
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
	const bool sequencer_owns_durable_cv = (replication_factor_ <= 1);
	// CV entries live in CXL memory and are shared with the replication tail. Refresh the full line
	// before modifying it so we do not flush a newer sequencer/pbr update alongside a stale durable
	// offset (or vice versa).
	CXL::flush_cacheline(entry);
	CXL::full_fence();
	// Advance sequencer logical offset (ACK1 frontier) when this batch extends the cumulative count
	for (;;) {
		uint64_t cur_offset = entry->sequencer_logical_offset.load(std::memory_order_acquire);
		if (cumulative_msg_count <= cur_offset) break;
		if (entry->sequencer_logical_offset.compare_exchange_strong(cur_offset, cumulative_msg_count, std::memory_order_release)) {
			if (sequencer_owns_durable_cv) {
				// Only unreplicated topics let the sequencer own the durable/export frontier directly.
				uint64_t cur_pbr = entry->completed_pbr_head.load(std::memory_order_acquire);
				if (cur_pbr == kNoProgress || pbr_index > cur_pbr) {
					entry->completed_pbr_head.store(pbr_index, std::memory_order_release);
				}
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

void Topic::FlushAccumulatedCVLogicalOnly(
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index_plus_one) {
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
	const bool sequencer_owns_durable_cv = (replication_factor_ <= 1);

	for (int broker_id = 0; broker_id < NUM_MAX_BROKERS; ++broker_id) {
		const uint64_t cumulative = max_cumulative[broker_id];
		const uint64_t pbr_index_plus_one = max_pbr_index_plus_one[broker_id];
		if (cumulative == 0 && pbr_index_plus_one == 0) continue;
		const uint64_t pbr_index = (pbr_index_plus_one == 0) ? 0 : (pbr_index_plus_one - 1);

		CompletionVectorEntry* entry = &cv[broker_id];
		CXL::flush_cacheline(entry);
		// CompletionVector lives in CXL memory; CLFLUSHOPT is only ordered by SFENCE/MFENCE.
		// Using LFENCE here can re-read a stale line and republish a newer PBR head with an older
		// durable logical frontier.
		CXL::full_fence();

		const uint64_t prev_cur = entry->sequencer_logical_offset.load(std::memory_order_acquire);
		const uint64_t prev_durable = entry->completed_logical_offset.load(std::memory_order_acquire);
		const uint64_t prev_pbr = entry->completed_pbr_head.load(std::memory_order_acquire);
		bool advanced_logical = false;
		bool advanced_durable = false;
		bool advanced_pbr = false;
		uint64_t post_cur = prev_cur;
		uint64_t post_durable = prev_durable;
		uint64_t post_pbr = prev_pbr;
		if (cumulative > prev_cur) {
			uint64_t cur = prev_cur;
			while (cumulative > cur) {
				if (entry->sequencer_logical_offset.compare_exchange_strong(
				        cur, cumulative, std::memory_order_release, std::memory_order_acquire)) {
					advanced_logical = true;
					post_cur = cumulative;
					break;
				}
			}
			if (!advanced_logical) {
				post_cur = cur;
			}
		}
		if (sequencer_owns_durable_cv) {
			// ORDER=5 late/terminalized batches can bypass the normal commit accumulation path.
			// In the unreplicated case the sequencer therefore also owns durable/export visibility.
			if (cumulative > prev_durable) {
				uint64_t cur = prev_durable;
				while (cumulative > cur) {
					if (entry->completed_logical_offset.compare_exchange_strong(
					        cur, cumulative, std::memory_order_release, std::memory_order_acquire)) {
						advanced_durable = true;
						post_durable = cumulative;
						break;
					}
				}
				if (!advanced_durable) {
					post_durable = cur;
				}
			}
			if (pbr_index_plus_one != 0 && (prev_pbr == kNoProgress || pbr_index > prev_pbr)) {
				uint64_t cur = prev_pbr;
				while (cur == kNoProgress || pbr_index > cur) {
					if (entry->completed_pbr_head.compare_exchange_strong(
					        cur, pbr_index, std::memory_order_release, std::memory_order_acquire)) {
						advanced_pbr = true;
						post_pbr = pbr_index;
						break;
					}
				}
				if (!advanced_pbr) {
					post_pbr = cur;
				}
			}
		}
		if (!advanced_logical && !advanced_durable && !advanced_pbr) continue;
		RecordOrder5FlightEvent(
			kOrder5FlightCV,
			static_cast<uint32_t>(broker_id),
			prev_cur,
			post_cur,
			prev_pbr,
			post_pbr);
		if (ShouldEnableOrder5Trace() && order_ == 5) {
			LOG(INFO) << "[ORDER5_TRACE_CV_LOGICAL_ONLY B" << broker_id << "]"
			          << " cv_logical:" << prev_cur << "->"
			          << post_cur
			          << " cv_durable:" << prev_durable << "->"
			          << post_durable
			          << " cv_pbr:" << prev_pbr << "->"
			          << post_pbr;
		}

		CXL::flush_cacheline(entry);
	}
	CXL::store_fence();
}

void Topic::FlushAccumulatedCV(
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index) {
	// [PHASE-3] O(brokers) CXL writes + 1 fence (was O(batches) fences)
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
	const bool sequencer_owns_durable_cv = (replication_factor_ <= 1);

	for (int broker_id = 0; broker_id < NUM_MAX_BROKERS; ++broker_id) {
		CompletionVectorEntry* entry = &cv[broker_id];

		// [PANEL C3/R3] On non-coherent CXL, relaxed load can read from local cache (stale).
		// CLFLUSHOPT is only ordered by SFENCE/MFENCE, so use a full fence before reading shared CV
		// state that may also be advanced by the replication tail.
		CXL::flush_cacheline(entry);
		CXL::full_fence();

		// Do not gate sequencer progress on topic-level ack_level. Client ack policy is per-connection.
		uint64_t cur = entry->sequencer_logical_offset.load(std::memory_order_acquire);
		const uint64_t prev_cur = cur;
		uint64_t cur_pbr = entry->completed_pbr_head.load(std::memory_order_acquire);
		const uint64_t prev_pbr = cur_pbr;
		const uint64_t pbr_val = max_pbr_index[broker_id];
		const uint64_t cumulative = max_cumulative[broker_id];
		bool advanced_logical = false;
		bool advanced_pbr = false;
		uint64_t post_cumulative = prev_cur;
		uint64_t post_pbr = prev_pbr;
		if (cumulative > cur) {
			while (cumulative > cur) {
				if (entry->sequencer_logical_offset.compare_exchange_strong(
				        cur, cumulative, std::memory_order_release, std::memory_order_acquire)) {
					advanced_logical = true;
					post_cumulative = cumulative;
					break;
				}
			}
			if (!advanced_logical) {
				post_cumulative = cur;
			}
		}
		if (sequencer_owns_durable_cv &&
		    (cur_pbr == kNoProgress || pbr_val > cur_pbr)) {
			while (cur_pbr == kNoProgress || pbr_val > cur_pbr) {
				if (entry->completed_pbr_head.compare_exchange_strong(
				        cur_pbr, pbr_val, std::memory_order_release, std::memory_order_acquire)) {
					advanced_pbr = true;
					post_pbr = pbr_val;
					break;
				}
			}
			if (!advanced_pbr) {
				post_pbr = cur_pbr;
			}
		}
		if (order_ == 5 && post_pbr > prev_pbr && post_cumulative <= prev_cur) {
			order5_ack_order_violations_.fetch_add(1, std::memory_order_relaxed);
		}
		if (advanced_logical || advanced_pbr) {
			RecordOrder5FlightEvent(
				kOrder5FlightCV,
				static_cast<uint32_t>(broker_id),
				prev_cur,
				post_cumulative,
				prev_pbr,
				post_pbr);
		}
		if (ShouldEnableOrder5Trace() && order_ == 5 &&
		    (advanced_logical || (prev_pbr == kNoProgress || post_pbr > prev_pbr))) {
			LOG(INFO) << "[ORDER5_TRACE_CV B" << broker_id << "]"
			          << " cv_logical:" << prev_cur << "->"
			          << post_cumulative
			          << " cv_pbr_head:" << prev_pbr << "->"
			          << post_pbr;
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
	static std::atomic<uint64_t> last_enqueue_trace_ns{0};
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
				if (ShouldEnableOrder5Trace() && order_ == 5) {
					const uint64_t now_ns = SteadyNowNs();
					uint64_t last_ns = last_enqueue_trace_ns.load(std::memory_order_relaxed);
					if (now_ns - last_ns >= 1'000'000'000ULL &&
					    last_enqueue_trace_ns.compare_exchange_strong(
					        last_ns, now_ns, std::memory_order_relaxed)) {
						LOG(INFO) << "[ORDER5_TRACE_CR_ENQUEUE]"
						          << " range=[" << start << "," << end << ")"
						          << " depth=" << depth + 1
						          << " retries=" << completed_ranges_enqueue_retries_.load(std::memory_order_relaxed)
						          << " pending_peak=" << committed_updater_pending_peak_.load(std::memory_order_relaxed);
					}
				}
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
	uint64_t last_trace_ns = SteadyNowNs();

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
			if (ShouldEnableOrder5Trace() && order_ == 5) {
				const uint64_t now_ns = SteadyNowNs();
				if (now_ns - last_trace_ns >= 1'000'000'000ULL) {
					LOG(INFO) << "[ORDER5_TRACE_COMMITTED_UPDATER]"
					          << " committed_seq=" << new_committed
					          << " next_expected_start=" << next_expected_start
					          << " pending_size=" << pending.size()
					          << " ring_depth="
					          << (completed_ranges_tail_.load(std::memory_order_relaxed) -
					              completed_ranges_head_.load(std::memory_order_relaxed))
					          << " pending_peak=" << committed_updater_pending_peak_.load(std::memory_order_relaxed);
					last_trace_ns = now_ns;
				}
			}
			idle_spins = 0;
		}

		if (ShouldEnableOrder5Trace() && order_ == 5 && !advanced) {
			const uint64_t now_ns = SteadyNowNs();
			if (now_ns - last_trace_ns >= 1'000'000'000ULL) {
				const uint64_t ring_depth =
					completed_ranges_tail_.load(std::memory_order_relaxed) -
					completed_ranges_head_.load(std::memory_order_relaxed);
				if (!pending.empty()) {
					const CompletedRange r = pending.top();
					LOG(INFO) << "[ORDER5_TRACE_COMMITTED_STALL]"
					          << " next_expected_start=" << next_expected_start
					          << " head_range=[" << r.start << "," << r.end << ")"
					          << " pending_size=" << pending.size()
					          << " ring_depth=" << ring_depth
					          << " stop=" << committed_seq_updater_stop_.load(std::memory_order_relaxed);
				} else if (ring_depth > 0) {
					LOG(INFO) << "[ORDER5_TRACE_COMMITTED_WAIT]"
					          << " next_expected_start=" << next_expected_start
					          << " ring_depth=" << ring_depth
					          << " pending_size=0";
				}
				last_trace_ns = now_ns;
			}
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
void Topic::PushOrder0Batch(uint64_t log_idx, uint32_t total_size, uint32_t num_msg) {
	uint64_t cursor = order0_batch_write_cursor_.fetch_add(1, std::memory_order_relaxed);
	auto& slot = order0_batch_ring_[cursor & (kOrder0BatchRingSize - 1)];
	slot.log_idx = log_idx;
	slot.total_size = total_size;
	slot.num_msg = num_msg;
	// Release store: makes log_idx/total_size/num_msg visible to readers after sequence.
	slot.sequence.store(cursor + 1, std::memory_order_release);
}

bool Topic::ReadOrder0Batch(uint64_t& read_cursor,
		uint64_t& out_log_idx,
		uint32_t& out_total_size,
		uint32_t& out_num_msg) const {
	const auto& slot = order0_batch_ring_[read_cursor & (kOrder0BatchRingSize - 1)];
	uint64_t seq = slot.sequence.load(std::memory_order_acquire);
	if (seq != read_cursor + 1) {
		if (seq > read_cursor + 1) {
			// Subscriber fell behind; ring slot has been overwritten. Skip ahead.
			static std::atomic<uint64_t> overrun_log_count{0};
			if (overrun_log_count.fetch_add(1, std::memory_order_relaxed) % 100 == 0) {
				LOG(WARNING) << "ORDER=0 subscribe ring overrun: read_cursor=" << read_cursor
				             << " seq=" << seq << " (subscriber too slow, skipping "
				             << (seq - 1 - read_cursor) << " batches)";
			}
			read_cursor = seq - 1;
		}
		return false;
	}
	out_log_idx = slot.log_idx;
	out_total_size = slot.total_size;
	out_num_msg = slot.num_msg;
	read_cursor++;
	return true;
}

bool Topic::GetMessageAddr(
		size_t &last_offset,
		void* &last_addr,
		void* &messages,
		size_t &messages_size) {
	auto advance_order0_cursor = [&](void* payload_end_addr) -> void* {
		const size_t align = (replication_factor_ > 0) ? 4096UL : 64UL;
		uintptr_t addr = reinterpret_cast<uintptr_t>(payload_end_addr);
		return reinterpret_cast<void*>((addr + align - 1) & ~(align - 1));
	};

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
				int ready_replicas = 0;
				for (int i = 0; i < replication_factor_; i++) {
					int b = Embarcadero::GetReplicationSetBroker(broker_id_, replication_factor_, num_brokers, i);
					volatile uint64_t* rep_done_ptr = &tinode_->offsets[b].replication_done[broker_id_];
					CXL::flush_cacheline(const_cast<const void*>(
						reinterpret_cast<const volatile void*>(rep_done_ptr)));
					r[i] = *rep_done_ptr;
					if (r[i] == kReplicationNotStarted) {
						continue;
					}
					ready_replicas++;
					if (min > r[i]) {
						min = r[i];
					}
				}
				CXL::load_fence();

				if (ready_replicas < replication_factor_ || min == kReplicationNotStarted) {
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

		// No order-0 messages have become visible yet. Starting from first_message_addr_
		// here can export garbage from a freshly allocated segment before the first batch arrives.
		if (combined_offset == 0 && last_addr == nullptr) {
			return false;
		}
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

		if (order_ == 0) {
			// [[CXL_VISIBILITY]] Invalidate cache before reading paddedSize
			CXL::flush_cacheline(CXL::ToFlushable(start_msg_header));
			CXL::load_fence();

			// Wait for message to be written if necessary (paddedSize is set by publisher)
			while (start_msg_header->paddedSize == 0) {
				CXL::flush_cacheline(CXL::ToFlushable(start_msg_header));
				CXL::load_fence();
				std::this_thread::yield();
			}

			// We DO NOT move start_msg_header for order 0! It is ALREADY pointing to the next unread message!
		} else {
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
		}
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
			        << " broker=" << broker_id_ << " topic=" << topic_name_
			        << " (message not visible yet; retrying without advancing cursor)";
			return false;
		}
	} else {
		// Set output message pointer
		messages = static_cast<void*>(start_msg_header);
	}

#ifdef MULTISEGMENT
	LOG(FATAL) << "MULTISEGMENT IS DEFINED!";
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
	size_t full_size;
	if (order_ == 0) {
		full_size = reinterpret_cast<uint8_t*>(combined_addr) - reinterpret_cast<uint8_t*>(start_msg_header);
	} else {
		full_size = reinterpret_cast<uint8_t*>(combined_addr) -
			reinterpret_cast<uint8_t*>(start_msg_header) +
			reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize;
	}

	// Order 0: cap export at 2MB per call to reduce mutex/send overhead (batch message export)
	constexpr size_t kMaxExportBatchBytes = 2UL << 20;
	// [[BLOG_HEADER]] ORDER=0 uses BlogMessageHeader when enabled; stride = ComputeStrideV2(size)
	// instead of paddedSize. The readiness check (size == 0) works for both formats because
	// BlogMessageHeader::size and MessageHeader::paddedSize both occupy bytes 0-3 (little-endian).
	const bool blog_order0 = (order_ == 0 && HeaderUtils::ShouldUseBlogHeader());
	if (order_ == 0 && full_size > kMaxExportBatchBytes) {
		MessageHeader* cur = start_msg_header;
		size_t accumulated = 0;
		MessageHeader* stop_at = nullptr;
		while (cur != combined_addr) {
			CXL::flush_cacheline(CXL::ToFlushable(cur));
			CXL::load_fence();
			size_t step;
			if (blog_order0) {
				uint32_t payload_size = reinterpret_cast<BlogMessageHeader*>(cur)->size;
				if (payload_size == 0) break;  // message not yet visible
				step = wire::ComputeStrideV2(payload_size);
			} else {
				step = cur->paddedSize;
				if (step == 0) break;  // message not yet visible
			}
			if (accumulated + step > kMaxExportBatchBytes) break;
			accumulated += step;
			stop_at = cur;
			cur = reinterpret_cast<MessageHeader*>(reinterpret_cast<uint8_t*>(cur) + step);
		}
		if (stop_at != nullptr) {
			CXL::flush_cacheline(CXL::ToFlushable(stop_at));
			CXL::load_fence();
			if (order_ == 0) {
				size_t stop_stride;
				if (blog_order0) {
					stop_stride = wire::ComputeStrideV2(reinterpret_cast<BlogMessageHeader*>(stop_at)->size);
				} else {
					stop_stride = stop_at->paddedSize;
				}
				// Use the total written count as last_offset so the "caught up" check works
				// correctly regardless of header format (logical_offset is unused in ORDER=0).
				last_offset = combined_offset;
				last_addr = advance_order0_cursor(
					reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(stop_at) + stop_stride));
			} else {
				last_offset = stop_at->logical_offset;
				if (last_offset == static_cast<size_t>(-1))
					last_offset = (combined_offset > 0) ? (combined_offset - 1) : 0;
				last_addr = stop_at;
			}
			messages_size = accumulated;
		} else {
			// No fully visible message was available for export yet. Keep the cursor unchanged
			// and retry instead of speculatively consuming the whole written frontier.
			return false;
		}
	} else {
		CXL::flush_cacheline(CXL::ToFlushable(combined_addr));
		CXL::load_fence();
		if (order_ == 0) {
			last_offset = combined_offset;
			last_addr = advance_order0_cursor(combined_addr);
		} else {
			last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
			last_addr = combined_addr;
		}
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
	epoch_driver_done_.store(false, std::memory_order_release);
	current_epoch_for_hold_.store(0, std::memory_order_release);
	for (int i = 0; i < 3; i++) {
		epoch_buffers_[i].state.store(EpochBuffer5::State::IDLE, std::memory_order_relaxed);
	}
	CHECK(epoch_buffers_[0].reset_and_start());

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
			scanner_shutdown_drained_[broker_id].store(false, std::memory_order_release);
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
	CHECK(epoch_buffers_[0].reset_and_start());

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
			scanner_shutdown_drained_[broker_id].store(false, std::memory_order_release);
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
	unsigned epoch_us = kEpochUs;
	if (const char* env = std::getenv("EMBAR_ORDER5_EPOCH_US")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0' && parsed >= 100 && parsed <= 5000) {
			epoch_us = static_cast<unsigned>(parsed);
		} else {
			LOG(WARNING) << "Ignoring invalid EMBAR_ORDER5_EPOCH_US='" << env
			             << "' (expected integer in [100, 5000]); using default " << kEpochUs;
		}
	}
	LOG(INFO) << "EpochDriverThread started (epoch_us=" << epoch_us << ")";
	constexpr uint64_t kNewBrokerCheckInterval = 20000;  // Check every 20000 epochs (~10 s at 500 µs/epoch)
	const auto epoch_duration = std::chrono::microseconds(epoch_us);
	auto next_seal_deadline = std::chrono::steady_clock::now() + epoch_duration;
	uint64_t epoch_count = 0;
	const bool order5_phase_diag = (order_ == 5 && ShouldEnableOrder5PhaseDiag());
	auto last_driver_diag = std::chrono::steady_clock::now();
	while (!stop_threads_) {
		// Pace sealing to the configured epoch duration to preserve batching efficiency.
		auto now = std::chrono::steady_clock::now();
		const bool disconnect_drain_active =
			SteadyNowNs() < force_expire_hold_until_ns_.load(std::memory_order_acquire);
		if (!disconnect_drain_active && now < next_seal_deadline) {
			auto remaining = next_seal_deadline - now;
			if (remaining > std::chrono::microseconds(50)) {
				std::this_thread::sleep_for(remaining - std::chrono::microseconds(25));
			} else {
				CXL::cpu_pause();
			}
			continue;
		}
		if (disconnect_drain_active) {
			RecordOrder5FlightEvent(
				kOrder5FlightDriver,
				static_cast<uint32_t>(broker_id_),
				epoch_index_.load(std::memory_order_acquire),
				last_sequenced_epoch_.load(std::memory_order_acquire),
				force_expire_hold_until_ns_.load(std::memory_order_acquire),
				1);
		}
		if (disconnect_drain_active) {
			// Disconnect-time tail drain is latency-sensitive: keep retrying seal/advance instead
			// of waiting for the normal epoch cadence. Re-anchor the next normal deadline so we
			// do not run a catch-up burst once the drain window closes.
			next_seal_deadline = now + epoch_duration;
		} else {
			next_seal_deadline += epoch_duration;
			if (now - next_seal_deadline > epoch_duration) {
				// If delayed (e.g., scheduler pause), re-anchor deadline to avoid catch-up bursts.
				next_seal_deadline = now + epoch_duration;
			}
		}

		constexpr int kMaxIterations = 50000; // Prevent infinite spinning
		int iterations = 0;
		while (!stop_threads_ && iterations < kMaxIterations) {
			uint64_t cur = epoch_index_.load(std::memory_order_acquire);
			EpochBuffer5& cur_buf = epoch_buffers_[cur % 3];
			uint64_t last_seq = last_sequenced_epoch_.load(std::memory_order_acquire);
			EpochBuffer5::State cur_state = cur_buf.state.load(std::memory_order_acquire);
			// Recovery path: epoch_index must always point at a collectable epoch. If it points at
			// an IDLE buffer, scanners have nowhere to publish and the sequencer can stall forever.
			if (cur_state == EpochBuffer5::State::IDLE) {
				if (cur_buf.reset_and_start()) {
					break;
				}
				cur_state = cur_buf.state.load(std::memory_order_acquire);
				if (cur_state == EpochBuffer5::State::COLLECTING) {
					break;
				}
			}
			// Recovery path: if current epoch is already SEALED and no index advance occurred,
			// re-establish a COLLECTING buffer so scanners cannot livelock on sealed/idle states.
				if (cur_state == EpochBuffer5::State::SEALED) {
					uint64_t next = cur + 1;
					EpochBuffer5& next_buf = epoch_buffers_[next % 3];
					EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
					if (next_state == EpochBuffer5::State::SEALED && last_seq >= cur) {
						// Tail case: scanners can populate and a prior driver pass can seal the successor
						// before epoch_index advances. Move forward so the sequencer can consume it.
						epoch_index_.store(next, std::memory_order_release);
						break;
					}
					if (next_state == EpochBuffer5::State::COLLECTING) {
						epoch_index_.store(next, std::memory_order_release);
						break;
					}
				if (next_state == EpochBuffer5::State::IDLE && last_seq >= cur) {
					if (!next_buf.reset_and_start()) {
						++iterations;
						continue;
					}
					epoch_index_.store(next, std::memory_order_release);
					break;
				}
			}
			if (cur_buf.seal()) {
				uint64_t next = cur + 1;
				EpochBuffer5& next_buf = epoch_buffers_[next % 3];
				// Critical invariant: after sealing current epoch, ensure there is always a
				// COLLECTING successor before returning. Waiting only for IDLE can deadlock
				// if the successor is already COLLECTING.
				int wait_iterations = 0;
					while (!stop_threads_) {
						EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
						if (next_state == EpochBuffer5::State::SEALED) {
							epoch_index_.store(next, std::memory_order_release);
							break;
						}
						if (next_state == EpochBuffer5::State::COLLECTING) {
							epoch_index_.store(next, std::memory_order_release);
							break;
						}
					if (next_state == EpochBuffer5::State::IDLE) {
						if (!next_buf.reset_and_start()) {
							CXL::cpu_pause();
							++wait_iterations;
							if ((wait_iterations % 8192) == 0) {
								std::this_thread::yield();
							}
							continue;
						}
						epoch_index_.store(next, std::memory_order_release);
						break;
					}
					CXL::cpu_pause();
					++wait_iterations;
					if ((wait_iterations % 8192) == 0) {
						std::this_thread::yield();
					}
				}
				if (stop_threads_) break;
				break; // Successfully processed epoch
			}
			CXL::cpu_pause();
			iterations++;
		}

			// Yield occasionally to prevent starvation, but much more frequently than 500us
			if (iterations >= kMaxIterations) {
				if (order5_phase_diag) {
					auto now = std::chrono::steady_clock::now();
					if (now - last_driver_diag >= std::chrono::seconds(1)) {
						last_driver_diag = now;
						uint64_t cur = epoch_index_.load(std::memory_order_acquire);
						uint64_t last_seq = last_sequenced_epoch_.load(std::memory_order_acquire);
						auto state_to_cstr = [](EpochBuffer5::State s) {
							switch (s) {
								case EpochBuffer5::State::IDLE: return "IDLE";
								case EpochBuffer5::State::RESETTING: return "RESETTING";
								case EpochBuffer5::State::COLLECTING: return "COLLECTING";
								case EpochBuffer5::State::SEALED: return "SEALED";
							}
							return "UNKNOWN";
						};
						auto active_count = [&](size_t idx) {
							int active = 0;
							for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
								if (epoch_buffers_[idx].broker_active[i].load(std::memory_order_acquire)) {
									++active;
								}
							}
							return active;
						};
						EpochBuffer5::State s0 = epoch_buffers_[0].state.load(std::memory_order_acquire);
						EpochBuffer5::State s1 = epoch_buffers_[1].state.load(std::memory_order_acquire);
						EpochBuffer5::State s2 = epoch_buffers_[2].state.load(std::memory_order_acquire);
						LOG(INFO) << "[ORDER5_PHASE_DIAG driver]"
						          << " cur=" << cur
						          << " last_sequenced=" << last_seq
						          << " state0=" << state_to_cstr(s0) << "(active=" << active_count(0) << ")"
						          << " state1=" << state_to_cstr(s1) << "(active=" << active_count(1) << ")"
						          << " state2=" << state_to_cstr(s2) << "(active=" << active_count(2) << ")";
					}
				}
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
		// After sealing the final steady-state epoch, keep sealing a small bounded number
		// of shutdown collection epochs so batches that become visible during disconnect/drain
		// are not stranded in a trailing COLLECTING buffer.
		LOG(INFO) << "EpochDriverThread: Resetting trailing buffers for late-arriving batches";
		const auto kFinalCollectionBudget =
			(replication_factor_ > 0) ? std::chrono::seconds(12) : std::chrono::milliseconds(1800);
		const auto kFinalSequencerBudget =
			(replication_factor_ > 0) ? std::chrono::seconds(12) : std::chrono::milliseconds(1800);
		const int kMaxTrailingEpochs = (replication_factor_ > 0) ? 64 : 3;
		uint64_t target_seq = final_epoch + 1;
		auto collect_deadline = std::chrono::steady_clock::now() + kFinalCollectionBudget;

		for (int step = 0; step < kMaxTrailingEpochs; ++step) {
			uint64_t next_epoch = target_seq;
			EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];
			auto buf_avail_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
			while (!next_buf.is_available() &&
			       std::chrono::steady_clock::now() < buf_avail_deadline) {
				std::this_thread::sleep_for(std::chrono::microseconds(10));
			}
			if (!next_buf.is_available()) {
				break;
			}

			if (!next_buf.reset_and_start()) {
				break;
			}
			epoch_index_.store(next_epoch, std::memory_order_release);
			LOG(INFO) << "EpochDriverThread: Trailing epoch " << next_epoch << " ready for collection";

			int quiescent_checks = 0;
			bool saw_activity = false;
			while (std::chrono::steady_clock::now() < collect_deadline) {
				bool any_active = false;
				for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
					if (next_buf.broker_active[i].load(std::memory_order_acquire)) {
						any_active = true;
						saw_activity = true;
						break;
					}
				}
				if (!any_active) {
					if (++quiescent_checks >= 20) break;  // ~2ms quiescent
				} else {
					quiescent_checks = 0;
				}
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			}

			if (!next_buf.seal()) {
				break;
			}

			size_t buffered_batches = 0;
			for (const auto& q : next_buf.per_broker) buffered_batches += q.size();
			const size_t held_batches = GetTotalHoldBufferSize();
			LOG(INFO) << "EpochDriverThread: Sealed trailing epoch " << next_epoch
			          << " (buffered_batches=" << buffered_batches
			          << ", held_batches=" << held_batches
			          << ", saw_activity=" << (saw_activity ? "yes" : "no") << ")";
			target_seq = next_epoch + 1;

			if (!saw_activity && buffered_batches == 0 && held_batches == 0) {
				break;
			}
		}

		// Wait for sequencer to process final and trailing epochs; bounded for fast shutdown.
		auto deadline = std::chrono::steady_clock::now() + kFinalSequencerBudget;
		while (last_sequenced_epoch_.load(std::memory_order_acquire) < target_seq &&
		       std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		LOG(INFO) << "EpochDriverThread: Final epochs sealed, last_sequenced="
		          << last_sequenced_epoch_.load(std::memory_order_acquire);
	} else {
		LOG(WARNING) << "EpochDriverThread: Failed to seal final epoch " << final_epoch;
	}
	epoch_driver_done_.store(true, std::memory_order_release);
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
			scanner_shutdown_drained_[broker_id].store(false, std::memory_order_release);

			brokers_with_scanners_.insert(broker_id);
			scanner_threads_.emplace_back(&Topic::BrokerScannerWorker5, this, broker_id);

			LOG(INFO) << "[DYNAMIC_SCANNER] Started BrokerScannerWorker5 for broker " << broker_id
			         << ", total scanners now: " << brokers_with_scanners_.size();
		}
	}
}

bool Topic::HaveAllScannerDrainsCompleted() {
	absl::MutexLock lock(&scanner_management_mu_);
	for (int broker_id : brokers_with_scanners_) {
		if (!scanner_shutdown_drained_[broker_id].load(std::memory_order_acquire)) {
			return false;
		}
	}
	return true;
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
	std::array<uint64_t, NUM_MAX_BROKERS> goi_cumulative_tracker{};
	std::array<uint64_t, NUM_MAX_BROKERS> cv_cumulative_tracker{};
	std::array<bool, NUM_MAX_BROKERS> goi_tracker_initialized{};
	std::array<bool, NUM_MAX_BROKERS> cv_tracker_initialized{};
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	auto ensure_goi_tracker = [&](int broker_id) {
		if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS || goi_tracker_initialized[broker_id]) return;
		goi_cumulative_tracker[broker_id] = tinode_->offsets[broker_id].ordered;
		goi_tracker_initialized[broker_id] = true;
	};
	auto ensure_cv_tracker = [&](int broker_id) {
		if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS || cv_tracker_initialized[broker_id]) return;
		const uint64_t base = tinode_->offsets[broker_id].ordered;
		CompletionVectorEntry* entry = &cv[broker_id];
		CXL::flush_cacheline(entry);
		CXL::full_fence();
		const uint64_t cv_logical =
			entry->sequencer_logical_offset.load(std::memory_order_acquire);
		const uint64_t cv_durable =
			entry->completed_logical_offset.load(std::memory_order_acquire);
		cv_cumulative_tracker[broker_id] = std::max(base, std::max(cv_logical, cv_durable));
		cv_tracker_initialized[broker_id] = true;
	};

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
				const int owner_broker = m.broker_id;
				ensure_goi_tracker(owner_broker);
				const uint64_t cumulative_msg_count =
					(owner_broker >= 0 && owner_broker < NUM_MAX_BROKERS)
						? (goi_cumulative_tracker[owner_broker] += m.num_msg)
						: static_cast<uint64_t>(m.num_msg);
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
				entry->cumulative_message_count = cumulative_msg_count;
				// Publish GOI index last so readers never treat a partially-written entry as valid.
				entry->global_seq = batch_index;
				next_order += m.num_msg;
			} else if (p.hdr != nullptr) {
				const int owner_broker = p.broker_id;
				ensure_goi_tracker(owner_broker);
				const uint64_t cumulative_msg_count =
					(owner_broker >= 0 && owner_broker < NUM_MAX_BROKERS)
						? (goi_cumulative_tracker[owner_broker] += p.num_msg)
						: static_cast<uint64_t>(p.num_msg);
				p.hdr->total_order = next_order;
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
				entry->cumulative_message_count = cumulative_msg_count;
				// Publish GOI index last so readers never treat a partially-written entry as valid.
				entry->global_seq = batch_index;
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
				const size_t batch_headers_offset = tinode_->offsets[b].batch_headers_offset;
				if (batch_headers_offset != 0) {
					BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(cxl_addr_) + batch_headers_offset);
					BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(ring_start) + BATCHHEADERS_SIZE);
					BatchHeader* cur = export_cursor_by_broker_[b];

					// Late topic creation on non-head brokers can leave an old/null cursor.
					// Rebind to ring_start once batch_headers_offset is published.
					if (cur == nullptr || cur < ring_start || cur >= ring_end) {
						cur = ring_start;
						export_cursor_by_broker_[b] = cur;
					}

					cur->batch_off_to_export = reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(cur);
					cur->ordered = 1;
					CXL::flush_cacheline(cur);
					CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(cur) + 64);

					BatchHeader* next_cursor = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(cur) + sizeof(BatchHeader));
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
	// Per-client delta accumulated here; flushed to per_client_ordered_ after [PHASE-4].
	absl::flat_hash_map<uint32_t, uint64_t> per_client_delta_epoch;

	for (PendingBatch5& p : ready) {
		if (p.skipped || p.is_held_marker) continue;
		uint64_t batch_index = base_batch_index_order5 + goi_idx_order5++;

		if (p.from_hold) {
			const HoldBatchMetadata& m = p.hold_meta;
				const int owner_broker = m.broker_id;
				ensure_cv_tracker(owner_broker);
				const uint64_t cumulative_msg_count =
					(owner_broker >= 0 && owner_broker < NUM_MAX_BROKERS)
						? (cv_cumulative_tracker[owner_broker] += m.num_msg)
						: static_cast<uint64_t>(m.num_msg);
				AccumulateCVUpdate(static_cast<uint16_t>(m.broker_id), m.pbr_absolute_index, cumulative_msg_count, cv_max_cumulative, cv_max_pbr_index);
				if (replication_factor_ > 0) {
					size_t ring_pos = goi_timestamp_write_pos_.fetch_add(1, std::memory_order_relaxed) % kGOITimestampRingSize;
					goi_timestamps_[ring_pos].goi_index.store(batch_index, std::memory_order_relaxed);
					goi_timestamps_[ring_pos].timestamp_ns.store(SteadyNowNs(), std::memory_order_release);
				}
		int b = m.broker_id;
		ordered_increment[b] += m.num_msg;
		last_ordered_offset[b] = m.log_idx;
		ordered_broker_seen[b] = true;
		per_client_delta_epoch[static_cast<uint32_t>(p.client_id)] += m.num_msg;
		{
			OrderedHoldExportEntry ex;
				ex.log_idx = m.log_idx;
				ex.batch_size = m.total_size;
				ex.total_order = next_order;
				ex.num_messages = m.num_msg;
				ex.pbr_absolute_index = m.pbr_absolute_index;
				{
					absl::MutexLock lock(&hold_export_queues_[b].mu);
					hold_export_queues_[b].q.push_back(ex);
				}
			}
			next_order += m.num_msg;
			if (m.broker_id >= 0 && m.broker_id < static_cast<int>(committed_this_epoch.size()))
				committed_this_epoch[m.broker_id]++;
		} else {
				const int owner_broker = p.broker_id;
				ensure_cv_tracker(owner_broker);
				const uint64_t cumulative_msg_count =
					(owner_broker >= 0 && owner_broker < NUM_MAX_BROKERS)
						? (cv_cumulative_tracker[owner_broker] += p.num_msg)
						: static_cast<uint64_t>(p.num_msg);
				AccumulateCVUpdate(static_cast<uint16_t>(p.broker_id), p.cached_pbr_absolute_index, cumulative_msg_count, cv_max_cumulative, cv_max_pbr_index);
				if (replication_factor_ > 0) {
					size_t ring_pos = goi_timestamp_write_pos_.fetch_add(1, std::memory_order_relaxed) % kGOITimestampRingSize;
					goi_timestamps_[ring_pos].goi_index.store(batch_index, std::memory_order_relaxed);
					goi_timestamps_[ring_pos].timestamp_ns.store(SteadyNowNs(), std::memory_order_release);
				}
		next_order += p.num_msg;
		int b = p.broker_id;
		ordered_increment[b] += p.num_msg;
		last_ordered_offset[b] = static_cast<size_t>(reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(cxl_addr_));
		ordered_broker_seen[b] = true;
		per_client_delta_epoch[static_cast<uint32_t>(p.client_id)] += p.num_msg;

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
		sequencer_committed_batches_[b].fetch_add(static_cast<uint64_t>(committed_this_epoch[b]), std::memory_order_relaxed);
		sequencer_committed_msgs_[b].fetch_add(static_cast<uint64_t>(inc), std::memory_order_relaxed);
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(&tinode_->offsets[b].ordered)));
		CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].ordered_offset));
	}
	// Per-client ordered update (one lock acquisition per epoch, not per batch).
	if (!per_client_delta_epoch.empty()) {
		absl::MutexLock lock(&per_client_mu_);
		for (auto& [cid, cnt] : per_client_delta_epoch) {
			per_client_ordered_[cid] += cnt;
		}
	}
	if (ShouldEnableFrontierTrace() && order_ == 5) {
		static thread_local uint64_t last_frontier_log_ns = 0;
		const uint64_t now_ns = SteadyNowNs();
		if (now_ns - last_frontier_log_ns >= 1'000'000'000ULL) {
			for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
				if (!ordered_broker_seen[b]) continue;
				LOG(INFO) << "[FRONTIER_TRACE_COMMIT B" << b << "]"
				          << " ordered=" << tinode_->offsets[b].ordered
				          << " ordered_inc=" << ordered_increment[b]
				          << " cv_target_logical=" << cv_max_cumulative[b]
				          << " cv_target_pbr=" << cv_max_pbr_index[b]
				          << " consumed_through=" << contiguous_consumed_per_broker[b];
			}
			last_frontier_log_ns = now_ns;
		}
	}
	if (ShouldEnableOrder5Trace() && order_ == 5 && num_goi_order5 > 0) {
		for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
			if (!ordered_broker_seen[b]) continue;
			LOG(INFO) << "[ORDER5_TRACE_COMMIT B" << b << "]"
			          << " epoch_base_order=" << base_order
			          << " total_msg=" << total_msg
			          << " goi_range=[" << base_batch_index_order5 << ","
			          << (base_batch_index_order5 + num_goi_order5) << ")"
			          << " ordered_inc=" << ordered_increment[b]
			          << " ordered_after=" << tinode_->offsets[b].ordered
			          << " consumed_through=" << contiguous_consumed_per_broker[b]
			          << " committed_batches=" << committed_this_epoch[b];
		}
	}
	if (order_ == 5 && num_goi_order5 > 0) {
		RecordOrder5FlightEvent(
			kOrder5FlightCommit,
			static_cast<uint32_t>(broker_id_),
			base_order,
			total_msg,
			base_batch_index_order5,
			num_goi_order5);
	}
	// [PHASE-3] Single fence for all CV updates
	FlushAccumulatedCV(cv_max_cumulative, cv_max_pbr_index);

	// Advance consumed_through for all remaining slots that were processed but not ready
	AdvanceConsumedThroughForProcessedSlots(batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch, nullptr, nullptr);

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
	uint64_t last_idle_hold_tick_ns = 0;
	const bool order5_phase_diag = (order_ == 5 && ShouldEnableOrder5PhaseDiag());
	auto last_phase_diag = std::chrono::steady_clock::now();
	auto emit_phase_diag = [&](const char* tag) {
		if (!order5_phase_diag) return;
		auto now = std::chrono::steady_clock::now();
		if (now - last_phase_diag < std::chrono::seconds(1)) return;
		last_phase_diag = now;
		auto state_to_cstr = [](EpochBuffer5::State s) {
			switch (s) {
				case EpochBuffer5::State::IDLE: return "IDLE";
				case EpochBuffer5::State::RESETTING: return "RESETTING";
				case EpochBuffer5::State::COLLECTING: return "COLLECTING";
				case EpochBuffer5::State::SEALED: return "SEALED";
			}
			return "UNKNOWN";
		};
		auto active_count = [&](size_t idx) {
			int active = 0;
			for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
				if (epoch_buffers_[idx].broker_active[i].load(std::memory_order_acquire)) {
					++active;
				}
			}
			return active;
		};
		EpochBuffer5::State s0 = epoch_buffers_[0].state.load(std::memory_order_acquire);
		EpochBuffer5::State s1 = epoch_buffers_[1].state.load(std::memory_order_acquire);
		EpochBuffer5::State s2 = epoch_buffers_[2].state.load(std::memory_order_acquire);
		LOG(INFO) << "[ORDER5_PHASE_DIAG " << tag << "]"
		          << " epoch_index=" << epoch_index_.load(std::memory_order_relaxed)
		          << " last_sequenced=" << last_sequenced_epoch_.load(std::memory_order_relaxed)
		          << " state0=" << state_to_cstr(s0) << "(active=" << active_count(0) << ")"
		          << " state1=" << state_to_cstr(s1) << "(active=" << active_count(1) << ")"
		          << " state2=" << state_to_cstr(s2) << "(active=" << active_count(2) << ")"
		          << " B0(push_b=" << scanner_pushed_batches_[0].load(std::memory_order_relaxed)
		          << ",push_m=" << scanner_pushed_msgs_[0].load(std::memory_order_relaxed)
		          << ",commit_b=" << sequencer_committed_batches_[0].load(std::memory_order_relaxed)
		          << ",commit_m=" << sequencer_committed_msgs_[0].load(std::memory_order_relaxed)
		          << ",ordered=" << tinode_->offsets[0].ordered
		          << ",consumed=" << tinode_->offsets[0].batch_headers_consumed_through << ")"
		          << " B1(push_b=" << scanner_pushed_batches_[1].load(std::memory_order_relaxed)
		          << ",commit_b=" << sequencer_committed_batches_[1].load(std::memory_order_relaxed)
		          << ",ordered=" << tinode_->offsets[1].ordered << ")"
		          << " B2(push_b=" << scanner_pushed_batches_[2].load(std::memory_order_relaxed)
		          << ",commit_b=" << sequencer_committed_batches_[2].load(std::memory_order_relaxed)
		          << ",ordered=" << tinode_->offsets[2].ordered << ")"
		          << " B3(push_b=" << scanner_pushed_batches_[3].load(std::memory_order_relaxed)
		          << ",commit_b=" << sequencer_committed_batches_[3].load(std::memory_order_relaxed)
		          << ",ordered=" << tinode_->offsets[3].ordered << ")";
	};
	auto run_idle_hold_tick = [&]() {
		if (!Embarcadero::UsesEpochSequencerPath(order_) || seq_type_ != EMBARCADERO) return;
		const uint64_t now_ns = SteadyNowNs();
		if (now_ns - last_idle_hold_tick_ns < 1'000'000ULL) return;
		const bool force_expire_active =
			now_ns < force_expire_hold_until_ns_.load(std::memory_order_acquire);
		const size_t hold_before = GetTotalHoldBufferSize();
		size_t deferred_before = 0;
		for (const auto& shard_ptr : level5_shards_) {
			if (!shard_ptr) continue;
			std::lock_guard<std::mutex> lock(shard_ptr->mu);
			deferred_before += shard_ptr->deferred_level5.size();
		}
		if (hold_before == 0 && deferred_before == 0 && !force_expire_active) return;
		std::vector<PendingBatch5> idle_level5_empty;
		std::vector<PendingBatch5> idle_ready_level5;
		ProcessLevel5Batches(idle_level5_empty, idle_ready_level5);
		if (!idle_ready_level5.empty()) {
			std::array<size_t, NUM_MAX_BROKERS> idle_contiguous_consumed{};
			std::array<bool, NUM_MAX_BROKERS> idle_broker_seen{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_max_cumulative{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_max_pbr_index{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_logical_only_cumulative{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_logical_only_pbr_index{};
			for (auto& shard_ptr : level5_shards_) {
				if (!shard_ptr) continue;
				for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS && cum > idle_cv_logical_only_cumulative[bid]) {
						idle_cv_logical_only_cumulative[bid] = cum;
					}
				}
				for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS && pbr > idle_cv_logical_only_pbr_index[bid]) {
						idle_cv_logical_only_pbr_index[bid] = pbr;
					}
				}
			}
			std::vector<const PendingBatch5*> idle_by_slot;
			std::vector<PendingBatch5> idle_batch_list;
			CommitEpoch(idle_ready_level5, idle_by_slot, idle_contiguous_consumed, idle_broker_seen,
			           idle_cv_max_cumulative, idle_cv_max_pbr_index, idle_batch_list,
			           /*is_drain_mode=*/false);
			FlushAccumulatedCVLogicalOnly(idle_cv_logical_only_cumulative, idle_cv_logical_only_pbr_index);
		} else {
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_logical_only_cumulative{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_logical_only_pbr_index{};
			for (auto& shard_ptr : level5_shards_) {
				if (!shard_ptr) continue;
				for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS && cum > idle_cv_logical_only_cumulative[bid]) {
						idle_cv_logical_only_cumulative[bid] = cum;
					}
				}
				for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS && pbr > idle_cv_logical_only_pbr_index[bid]) {
						idle_cv_logical_only_pbr_index[bid] = pbr;
					}
				}
			}
			FlushAccumulatedCVLogicalOnly(idle_cv_logical_only_cumulative, idle_cv_logical_only_pbr_index);
		}
		last_idle_hold_tick_ns = now_ns;
	};

	while (!stop_threads_) {
		emit_phase_diag("loop");
		uint64_t last = last_sequenced_epoch_.load(std::memory_order_acquire);
		uint64_t current = epoch_index_.load(std::memory_order_acquire);
		if (last >= current) {
			run_idle_hold_tick();
			// [[FIX_SEQUENCER_WAIT_PAUSE]] Replace sleep_for with cpu_pause for epoch waiting.
			CXL::cpu_pause();
			CXL::cpu_pause();
			continue;
		}
		size_t buffer_idx = last % 3;
		EpochBuffer5& buf = epoch_buffers_[buffer_idx];
		if (buf.state.load(std::memory_order_acquire) != EpochBuffer5::State::SEALED) {
			// Tail-progress tick: when no epoch is sealed, still age/expire hold-buffer entries
			// so final batches do not wait indefinitely for new input traffic.
			run_idle_hold_tick();
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
			std::lock_guard<std::mutex> data_lock(buf.data_mu);
			for (const auto& v : buf.per_broker) total += v.size();
			batch_list.reserve(total);
		}
		{
			std::lock_guard<std::mutex> data_lock(buf.data_mu);
			for (auto& v : buf.per_broker) {
				batch_list.insert(batch_list.end(),
					std::make_move_iterator(v.begin()),
					std::make_move_iterator(v.end()));
				v.clear();
			}
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
				order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
				order5_stale_epoch_skips_.fetch_add(1, std::memory_order_relaxed);
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
		std::array<uint64_t, NUM_MAX_BROKERS> cv_logical_only_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> cv_logical_only_pbr_index{};

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
			for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (cum > cv_logical_only_cumulative[bid]) cv_logical_only_cumulative[bid] = cum;
				}
			}
			for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (pbr > cv_logical_only_pbr_index[bid]) cv_logical_only_pbr_index[bid] = pbr;
				}
			}
		}

		if (ready.empty()) {
			// No ready batches, but we still need to advance consumed_through for all processed slots
			// to prevent ring deadlock. Advance consumed_through for all slots in batch_list.
			AdvanceConsumedThroughForProcessedSlots(batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch, &cv_max_cumulative, &cv_max_pbr_index);
			FlushAccumulatedCVLogicalOnly(cv_logical_only_cumulative, cv_logical_only_pbr_index);
			continue;
		}

		// Call unified commit logic
		CommitEpoch(ready, by_slot, contiguous_consumed_per_broker, broker_seen_in_epoch,
		           cv_max_cumulative, cv_max_pbr_index, batch_list, /*is_drain_mode=*/false);
		FlushAccumulatedCVLogicalOnly(cv_logical_only_cumulative, cv_logical_only_pbr_index);
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
			if (epoch_driver_done_.load(std::memory_order_acquire)) {
				EpochBuffer5& cur_buf = epoch_buffers_[current % 3];
				if (cur_buf.state.load(std::memory_order_acquire) == EpochBuffer5::State::COLLECTING &&
				    cur_buf.seal()) {
					const uint64_t next_epoch = current + 1;
					EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];
					EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
					if (next_state == EpochBuffer5::State::IDLE) {
						next_buf.reset_and_start();
						next_state = next_buf.state.load(std::memory_order_acquire);
					}
					if (next_state == EpochBuffer5::State::COLLECTING ||
					    next_state == EpochBuffer5::State::SEALED) {
						uint64_t expected = current;
						epoch_index_.compare_exchange_strong(
							expected, next_epoch, std::memory_order_release, std::memory_order_acquire);
					}
					if (ShouldEnableOrder5Trace() && order_ == 5) {
						LOG(INFO) << "[ORDER5_TRACE_DRAIN_SEAL]"
						          << " sealed_epoch=" << current
						          << " next_epoch=" << next_epoch
						          << " scanners_shutdown_pending="
						          << (!HaveAllScannerDrainsCompleted() ? 1 : 0);
					}
					continue;
				}
				if (!HaveAllScannerDrainsCompleted()) {
					std::this_thread::sleep_for(std::chrono::microseconds(100));
					continue;
				}
				// [[B0_TAIL_FIX]] Force one last hold/deferred drain so shutdown does not strand
				// tail batches after scanners have already stopped producing new input.
				size_t hb = GetTotalHoldBufferSize();
				bool has_deferred_level5 = false;
				for (auto& shard_ptr : level5_shards_) {
					if (!shard_ptr) continue;
					std::lock_guard<std::mutex> lock(shard_ptr->mu);
					if (!shard_ptr->deferred_level5.empty()) {
						has_deferred_level5 = true;
						break;
					}
				}
					if (hb > 0 || has_deferred_level5) {
						LOG(INFO) << "EpochSequencerThread: Final hold/deferred drain (hold_size="
						          << hb << ", has_deferred=" << (has_deferred_level5 ? 1 : 0)
						          << ") before exit";
						force_expire_hold_until_ns_.store(
							SteadyNowNs() + 20'000'000ULL, std::memory_order_release);
						std::vector<PendingBatch5> level5_empty, ready_level5;
						ProcessLevel5Batches(level5_empty, ready_level5);
						if (!ready_level5.empty()) {
							std::array<size_t, NUM_MAX_BROKERS> drain_contiguous_consumed{};
							std::array<bool, NUM_MAX_BROKERS> drain_broker_seen{};
							for (const PendingBatch5& p : ready_level5) {
								int b = p.from_hold ? p.hold_meta.broker_id : p.broker_id;
								if (b < 0 || b >= NUM_MAX_BROKERS || drain_broker_seen[b]) continue;
								const volatile void* addr = reinterpret_cast<const volatile void*>(
									&tinode_->offsets[b].batch_headers_consumed_through);
								CXL::flush_cacheline(const_cast<const void*>(addr));
								CXL::load_fence();
								size_t initial = tinode_->offsets[b].batch_headers_consumed_through;
								if (initial == BATCHHEADERS_SIZE) initial = 0;
								drain_contiguous_consumed[b] = initial;
								drain_broker_seen[b] = true;
							}
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_cumulative{};
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_pbr_index{};
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_cumulative{};
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_pbr_index{};
							for (auto& shard_ptr : level5_shards_) {
								if (!shard_ptr) continue;
								for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
									if (bid >= 0 && bid < NUM_MAX_BROKERS &&
									    cum > drain_cv_logical_only_cumulative[bid]) {
										drain_cv_logical_only_cumulative[bid] = cum;
									}
								}
								for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
									if (bid >= 0 && bid < NUM_MAX_BROKERS &&
									    pbr > drain_cv_logical_only_pbr_index[bid]) {
										drain_cv_logical_only_pbr_index[bid] = pbr;
									}
								}
							}
							std::vector<const PendingBatch5*> drain_by_slot;
							std::vector<PendingBatch5> empty_batch_list;
							CommitEpoch(ready_level5, drain_by_slot, drain_contiguous_consumed, drain_broker_seen,
							           drain_cv_max_cumulative, drain_cv_max_pbr_index,
							           empty_batch_list, /*is_drain_mode=*/true);
							FlushAccumulatedCVLogicalOnly(
								drain_cv_logical_only_cumulative, drain_cv_logical_only_pbr_index);
						}
						else {
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_cumulative{};
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_pbr_index{};
							for (auto& shard_ptr : level5_shards_) {
								if (!shard_ptr) continue;
								for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
									if (bid >= 0 && bid < NUM_MAX_BROKERS &&
									    cum > drain_cv_logical_only_cumulative[bid]) {
										drain_cv_logical_only_cumulative[bid] = cum;
									}
								}
								for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
									if (bid >= 0 && bid < NUM_MAX_BROKERS &&
									    pbr > drain_cv_logical_only_pbr_index[bid]) {
										drain_cv_logical_only_pbr_index[bid] = pbr;
									}
								}
							}
							FlushAccumulatedCVLogicalOnly(
								drain_cv_logical_only_cumulative, drain_cv_logical_only_pbr_index);
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
			std::lock_guard<std::mutex> data_lock(buf.data_mu);
			for (const auto& v : buf.per_broker) total += v.size();
			batch_list.reserve(total);
		}
		LOG(INFO) << "EpochSequencerThread Drain: Processing epoch " << last << " (buffer " << buffer_idx << ")";
		{
			std::lock_guard<std::mutex> data_lock(buf.data_mu);
			for (auto& v : buf.per_broker) {
				batch_list.insert(batch_list.end(),
					std::make_move_iterator(v.begin()),
					std::make_move_iterator(v.end()));
				v.clear();
			}
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
				order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
				order5_stale_epoch_skips_.fetch_add(1, std::memory_order_relaxed);
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
			std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_cumulative{};
			std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_pbr_index{};
			for (auto& shard_ptr : level5_shards_) {
				if (!shard_ptr) continue;
				for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS &&
					    cum > drain_cv_logical_only_cumulative[bid]) {
						drain_cv_logical_only_cumulative[bid] = cum;
					}
				}
				for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS &&
					    pbr > drain_cv_logical_only_pbr_index[bid]) {
						drain_cv_logical_only_pbr_index[bid] = pbr;
					}
				}
			}
			FlushAccumulatedCVLogicalOnly(
				drain_cv_logical_only_cumulative, drain_cv_logical_only_pbr_index);
			continue;
		}

		// Call unified commit logic
		// [[DRAIN_MODE_COMMIT]] Use drain-specific CV accumulator arrays
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_pbr_index{};
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_pbr_index{};
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
			for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (cum > drain_cv_logical_only_cumulative[bid]) {
						drain_cv_logical_only_cumulative[bid] = cum;
					}
				}
			}
			for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (pbr > drain_cv_logical_only_pbr_index[bid]) {
						drain_cv_logical_only_pbr_index[bid] = pbr;
					}
				}
			}
		}
		CommitEpoch(ready, by_slot, contiguous_consumed_per_broker, broker_seen_in_epoch,
		           drain_cv_max_cumulative, drain_cv_max_pbr_index, batch_list, /*is_drain_mode=*/true);
		FlushAccumulatedCVLogicalOnly(
			drain_cv_logical_only_cumulative, drain_cv_logical_only_pbr_index);
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
		auto hold_it = shard.hold_buffer.find(cid);
		if (hold_it != shard.hold_buffer.end() && !hold_it->second.empty()) {
			continue;
		}
		if (shard.clients_with_held_batches.contains(cid)) {
			continue;
		}
		if (!shard.deferred_level5.empty()) {
			continue;
		}
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
		shard.client_emitted_tracker.erase(cid);
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
	const bool true_client_chain = UsesTrueClientChainOrdering(order_);
	auto record_hold_insert = [&](size_t client_id, const std::map<size_t, HoldEntry5>& hold_map) {
		if (!true_client_chain) return;
		ClientPressureStats& stats = shard.client_pressure_stats[client_id];
		stats.hold_inserts++;
		stats.max_held_depth = std::max<uint64_t>(stats.max_held_depth, hold_map.size());
	};
	auto record_expiry = [&](size_t client_id) {
		if (!true_client_chain) return;
		shard.client_pressure_stats[client_id].expiries++;
	};
	auto record_forced_skip = [&](size_t client_id) {
		if (!true_client_chain) return;
		shard.client_pressure_stats[client_id].forced_skips++;
	};
	auto record_late_drop = [&](size_t client_id) {
		if (!true_client_chain) return;
		shard.client_pressure_stats[client_id].late_drops++;
	};
	auto accumulate_logical_only = [&](int broker_id, uint64_t start_logical_offset,
	                                  uint32_t num_msg, uint64_t pbr_index) {
		if (!true_client_chain) return;
		if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS) return;
		const uint64_t cumulative = start_logical_offset + static_cast<uint64_t>(num_msg);
		auto& slot = shard.cv_logical_only_cumulative[broker_id];
		if (cumulative > slot) slot = cumulative;
		const uint64_t encoded_pbr_index = pbr_index + 1;
		auto& pbr_slot = shard.cv_logical_only_pbr_index[broker_id];
		if (encoded_pbr_index > pbr_slot) pbr_slot = encoded_pbr_index;
	};
	auto terminalize_already_emitted = [&](size_t client_id, int broker_id, uint64_t start_logical_offset,
	                                      uint32_t num_msg, uint64_t batch_id, uint64_t seq,
	                                      uint64_t pbr_index) {
		if (!true_client_chain) return;
		accumulate_logical_only(broker_id, start_logical_offset, num_msg, pbr_index);
		record_late_drop(client_id);
		CheckAndInsertBatchId(shard, batch_id);
		if (ShouldEnableOrder5Trace()) {
			LOG(INFO) << "[ORDER5_TRACE_TERMINAL_ALREADY_EMITTED B" << broker_id << "]"
			          << " client=" << client_id
			          << " seq=" << seq
			          << " pbr=" << pbr_index
			          << " cumulative=" << (start_logical_offset + static_cast<uint64_t>(num_msg));
		}
	};
	if (!shard.deferred_level5.empty()) {
		level5.insert(level5.end(),
			std::make_move_iterator(shard.deferred_level5.begin()),
			std::make_move_iterator(shard.deferred_level5.end()));
		shard.deferred_level5.clear();
	}
	// [PHASE-3] Clear shard CV accumulation for this invocation
	shard.cv_max_cumulative.clear();
	shard.cv_max_pbr_index.clear();
	shard.cv_logical_only_cumulative.clear();
	shard.cv_logical_only_pbr_index.clear();
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
			if (true_client_chain) {
				std::vector<PendingBatch5*> ordered;
				ordered.reserve(level5.size());
				for (auto& p : level5) {
					ordered.push_back(&p);
				}
				if (ordered.size() > 1) {
					std::sort(ordered.begin(), ordered.end(),
						[](const PendingBatch5* a, const PendingBatch5* b) {
							return a->batch_seq < b->batch_seq;
						});
				}
				ClientState5& state = shard.client_state[first_client];
				ClientEmitTracker& emitted = shard.client_emitted_tracker[first_client];
				state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
				if (state.next_expected == 0 && emitted.contiguous_max != UINT64_MAX) {
					state.next_expected = emitted.contiguous_max + 1;
					state.highest_sequenced = emitted.contiguous_max;
				}
				if (state.next_expected == 0 &&
				    state.highest_sequenced == 0 &&
				    emitted.contiguous_max == UINT64_MAX) {
					// True client-chain ordering is defined over the full client sequence, not the
					// first fragment we happened to observe in this epoch. Starting at the first
					// observed batch_seq can silently treat earlier striped batches as "late" and
					// drop them behind the frontier. New clients always start from seq 0.
					state.next_expected = 0;
				}
				for (PendingBatch5* p : ordered) {
					size_t seq = p->batch_seq;
						if (state.is_duplicate(seq) &&
						    emitted.IsEmitted(static_cast<uint64_t>(seq))) {
							terminalize_already_emitted(
								first_client, p->broker_id, p->cached_start_logical_offset,
								p->num_msg, p->cached_batch_id, seq, p->cached_pbr_absolute_index);
							continue;
						}
						if (emitted.IsEmitted(static_cast<uint64_t>(seq))) {
						state.mark_sequenced(seq);
						if (seq >= state.next_expected) state.next_expected = seq + 1;
							terminalize_already_emitted(
								first_client, p->broker_id, p->cached_start_logical_offset,
								p->num_msg, p->cached_batch_id, seq, p->cached_pbr_absolute_index);
							continue;
						}
					if (seq == state.next_expected) {
						state.mark_sequenced(seq);
						state.advance_next_expected();
						if (!CheckAndInsertBatchId(shard, p->cached_batch_id)) {
							emitted.MarkEmitted(static_cast<uint64_t>(seq));
							ready.push_back(std::move(*p));
						}
					} else if (seq < state.next_expected) {
						state.mark_sequenced(seq);
						accumulate_logical_only(
							p->broker_id, p->cached_start_logical_offset, p->num_msg,
							p->cached_pbr_absolute_index);
						record_late_drop(first_client);
						CheckAndInsertBatchId(shard, p->cached_batch_id);
					} else {
						if (shard.hold_buffer_size >= kHoldBufferMaxEntries) {
							std::this_thread::sleep_for(std::chrono::microseconds(10));
							if (shard.deferred_level5.size() < kDeferredL5MaxEntries) {
								shard.deferred_level5.push_back(std::move(*p));
								continue;
							}
							state.mark_sequenced(seq);
							state.next_expected = seq + 1;
							order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
							order5_hold_buffer_forced_skips_.fetch_add(1, std::memory_order_relaxed);
							record_forced_skip(first_client);
							emitted.MarkEmitted(static_cast<uint64_t>(seq));
							ready.push_back(std::move(*p));
							continue;
						}

						auto& stream_map = shard.hold_buffer[first_client];
						if (stream_map.find(seq) != stream_map.end()) continue;

						HoldEntry5 he;
						he.batch = std::move(*p);
						he.meta.log_idx = p->cached_log_idx;
						he.meta.total_size = p->cached_total_size;
						he.meta.batch_id = p->cached_batch_id;
						he.meta.batch_seq = p->batch_seq;
						he.meta.pbr_absolute_index = p->cached_pbr_absolute_index;
						he.meta.client_id = p->client_id;
						he.meta.epoch_created = p->epoch_created;
						he.meta.broker_id = p->broker_id;
						he.meta.num_msg = p->num_msg;
						he.meta.start_logical_offset = p->cached_start_logical_offset;
						he.hold_start_ns = SteadyNowNs();
						stream_map.emplace(seq, he);
						record_hold_insert(first_client, stream_map);
						shard.hold_buffer_size++;
						shard.clients_with_held_batches.insert(first_client);
						InvalidateOrder5HeldSlot(p->hdr);

						PendingBatch5 marker;
						marker.broker_id = p->broker_id;
						marker.slot_offset = p->slot_offset;
						marker.is_held_marker = true;
						marker.hdr = nullptr;
						marker.num_msg = 0;
						ready.push_back(marker);
					}
				}
			} else {
				absl::flat_hash_map<int, std::vector<PendingBatch5*>> by_broker;
				by_broker.reserve(8);
				for (auto& p : level5) {
					by_broker[p.broker_id].push_back(&p);
				}
				for (auto& [broker_id, vec] : by_broker) {
					if (vec.size() > 1) {
						std::sort(vec.begin(), vec.end(),
							[](const PendingBatch5* a, const PendingBatch5* b) {
								return a->batch_seq < b->batch_seq;
							});
					}
					const uint64_t stream_key = MakeClientBrokerStreamKey(first_client, broker_id);
					ClientState5& state = shard.client_state[stream_key];
					ClientEmitTracker& emitted = shard.client_emitted_tracker[stream_key];
					state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
					if (state.next_expected == 0 && emitted.contiguous_max != UINT64_MAX) {
						state.next_expected = emitted.contiguous_max + 1;
						state.highest_sequenced = emitted.contiguous_max;
					}
					if (state.next_expected == 0 && !vec.empty()) {
						state.next_expected = vec.front()->batch_seq;
					}
					for (PendingBatch5* p : vec) {
						size_t seq = p->batch_seq;
						if (state.is_duplicate(seq) &&
						    emitted.IsEmitted(static_cast<uint64_t>(seq))) {
							continue;
						}
						if (emitted.IsEmitted(static_cast<uint64_t>(seq))) {
							state.mark_sequenced(seq);
							if (seq >= state.next_expected) state.next_expected = seq + 1;
							if (!CheckAndInsertBatchId(shard, p->cached_batch_id)) {
								ready.push_back(std::move(*p));
								emitted.MarkEmitted(static_cast<uint64_t>(seq));
								order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
							}
							continue;
						}
						if (seq == state.next_expected) {
							state.mark_sequenced(seq);
							state.advance_next_expected();
							if (!CheckAndInsertBatchId(shard, p->cached_batch_id)) {
								emitted.MarkEmitted(static_cast<uint64_t>(seq));
								ready.push_back(std::move(*p));
							}
						} else if (seq < state.next_expected) {
							state.mark_sequenced(seq);
							if (!CheckAndInsertBatchId(shard, p->cached_batch_id)) {
								order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
								ready.push_back(std::move(*p));
								emitted.MarkEmitted(static_cast<uint64_t>(seq));
							}
						} else {
							if (shard.hold_buffer_size >= kHoldBufferMaxEntries) {
								std::this_thread::sleep_for(std::chrono::microseconds(10));
								if (shard.deferred_level5.size() < kDeferredL5MaxEntries) {
									shard.deferred_level5.push_back(std::move(*p));
									continue;
								}
								state.mark_sequenced(seq);
								state.next_expected = seq + 1;
								order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
								order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
								order5_hold_buffer_forced_skips_.fetch_add(1, std::memory_order_relaxed);
								emitted.MarkEmitted(static_cast<uint64_t>(seq));
								ready.push_back(std::move(*p));
								continue;
							}

							auto& stream_map = shard.hold_buffer[stream_key];
							if (stream_map.find(seq) != stream_map.end()) continue;

							HoldEntry5 he;
							he.batch = std::move(*p);
							he.meta.log_idx = p->cached_log_idx;
							he.meta.total_size = p->cached_total_size;
							he.meta.batch_id = p->cached_batch_id;
							he.meta.batch_seq = p->batch_seq;
							he.meta.pbr_absolute_index = p->cached_pbr_absolute_index;
							he.meta.client_id = p->client_id;
							he.meta.epoch_created = p->epoch_created;
							he.meta.broker_id = p->broker_id;
							he.meta.num_msg = p->num_msg;
							he.meta.start_logical_offset = p->cached_start_logical_offset;
							he.hold_start_ns = SteadyNowNs();
							stream_map.emplace(seq, he);
							shard.hold_buffer_size++;
							shard.clients_with_held_batches.insert(stream_key);
							InvalidateOrder5HeldSlot(p->hdr);

							PendingBatch5 marker;
							marker.broker_id = p->broker_id;
							marker.slot_offset = p->slot_offset;
							marker.is_held_marker = true;
							marker.hdr = nullptr;
							marker.num_msg = 0;
							ready.push_back(marker);
						}
					}
				}
			}
			ClientGc(shard);
			return;
		}
	}

	if (true_client_chain) {
		std::sort(level5.begin(), level5.end(),
			[](const PendingBatch5& a, const PendingBatch5& b) {
				return std::tie(a.client_id, a.batch_seq) < std::tie(b.client_id, b.batch_seq);
			});
	} else {
		shard.radix_sorter.sort_by_client_id(level5);
	}

	for (auto it = level5.begin(); it != level5.end(); ) {
		size_t client_id = it->client_id;
		auto group_end = it;
		while (group_end != level5.end() && group_end->client_id == client_id) ++group_end;

		if (true_client_chain) {
			ClientState5& state = shard.client_state[client_id];
			state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
			ClientEmitTracker& emitted = shard.client_emitted_tracker[client_id];
			if (state.next_expected == 0 && emitted.contiguous_max != UINT64_MAX) {
				state.next_expected = emitted.contiguous_max + 1;
				state.highest_sequenced = emitted.contiguous_max;
			}
				if (state.next_expected == 0 &&
				    state.highest_sequenced == 0 &&
				    emitted.contiguous_max == UINT64_MAX) {
					state.next_expected = 0;
				}

			for (auto jt = it; jt != group_end; ++jt) {
				size_t seq = jt->batch_seq;
				if (state.is_duplicate(seq) &&
				    emitted.IsEmitted(static_cast<uint64_t>(seq))) {
						terminalize_already_emitted(
							client_id, jt->broker_id, jt->cached_start_logical_offset,
							jt->num_msg, jt->cached_batch_id, seq, jt->cached_pbr_absolute_index);
					continue;
				}
				if (emitted.IsEmitted(static_cast<uint64_t>(seq))) {
					state.mark_sequenced(seq);
					if (seq >= state.next_expected) {
						state.next_expected = seq + 1;
					}
					terminalize_already_emitted(
						client_id, jt->broker_id, jt->cached_start_logical_offset,
						jt->num_msg, jt->cached_batch_id, seq, jt->cached_pbr_absolute_index);
					continue;
				}
				if (seq == state.next_expected) {
					state.mark_sequenced(seq);
					state.advance_next_expected();
					if (!CheckAndInsertBatchId(shard, jt->cached_batch_id)) {
						emitted.MarkEmitted(static_cast<uint64_t>(seq));
						ready.push_back(std::move(*jt));
					}
				} else if (seq < state.next_expected) {
					state.mark_sequenced(seq);
					accumulate_logical_only(
						jt->broker_id, jt->cached_start_logical_offset, jt->num_msg,
						jt->cached_pbr_absolute_index);
					record_late_drop(client_id);
					CheckAndInsertBatchId(shard, jt->cached_batch_id);
				} else {
					if (shard.hold_buffer_size >= kHoldBufferMaxEntries) {
						std::this_thread::sleep_for(std::chrono::microseconds(10));
						if (shard.deferred_level5.size() < kDeferredL5MaxEntries) {
							shard.deferred_level5.push_back(std::move(*jt));
							continue;
						}
						state.mark_sequenced(seq);
						state.next_expected = seq + 1;
						order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
						order5_hold_buffer_forced_skips_.fetch_add(1, std::memory_order_relaxed);
						record_forced_skip(client_id);
						emitted.MarkEmitted(static_cast<uint64_t>(seq));
						ready.push_back(std::move(*jt));
						continue;
					}

					auto& stream_map = shard.hold_buffer[client_id];
					if (stream_map.find(seq) != stream_map.end()) {
						continue;
					}

					HoldEntry5 he;
					he.batch = std::move(*jt);
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
					stream_map.emplace(seq, he);
					record_hold_insert(client_id, stream_map);
					shard.hold_buffer_size++;
					shard.clients_with_held_batches.insert(client_id);
					InvalidateOrder5HeldSlot(jt->hdr);

					PendingBatch5 marker;
					marker.broker_id = jt->broker_id;
					marker.slot_offset = jt->slot_offset;
					marker.is_held_marker = true;
					marker.hdr = nullptr;
					marker.num_msg = 0;
					ready.push_back(marker);
				}
			}
		} else {
			std::sort(it, group_end,
				[](const PendingBatch5& a, const PendingBatch5& b) {
					if (a.broker_id != b.broker_id) return a.broker_id < b.broker_id;
					return a.batch_seq < b.batch_seq;
				});

			for (auto bit = it; bit != group_end; ) {
				int stream_broker = bit->broker_id;
				auto broker_end = bit;
				while (broker_end != group_end && broker_end->broker_id == stream_broker) ++broker_end;

				const uint64_t stream_key = MakeClientBrokerStreamKey(client_id, stream_broker);
				ClientState5& state = shard.client_state[stream_key];
				state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
				ClientEmitTracker& emitted = shard.client_emitted_tracker[stream_key];
				if (state.next_expected == 0 && emitted.contiguous_max != UINT64_MAX) {
					state.next_expected = emitted.contiguous_max + 1;
					state.highest_sequenced = emitted.contiguous_max;
				}
				if (state.next_expected == 0 && state.highest_sequenced == 0 && emitted.contiguous_max == UINT64_MAX) {
					state.next_expected = bit->batch_seq;
				}

				for (auto jt = bit; jt != broker_end; ++jt) {
					size_t seq = jt->batch_seq;
					if (state.is_duplicate(seq) &&
					    emitted.IsEmitted(static_cast<uint64_t>(seq))) {
						continue;
					}
					if (emitted.IsEmitted(static_cast<uint64_t>(seq))) {
						state.mark_sequenced(seq);
						if (seq >= state.next_expected) {
							state.next_expected = seq + 1;
						}
						if (!CheckAndInsertBatchId(shard, jt->cached_batch_id)) {
							ready.push_back(std::move(*jt));
							emitted.MarkEmitted(static_cast<uint64_t>(seq));
							order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
							VLOG(1) << "[L5_REPLAY_EMIT] B" << ready.back().broker_id
							        << " seq=" << seq;
						}
						continue;
					}
					if (seq == state.next_expected) {
						state.mark_sequenced(seq);
						state.advance_next_expected();
						if (!CheckAndInsertBatchId(shard, jt->cached_batch_id)) {
							emitted.MarkEmitted(static_cast<uint64_t>(seq));
							ready.push_back(std::move(*jt));
						}
					} else if (seq < state.next_expected) {
						state.mark_sequenced(seq);
						if (!CheckAndInsertBatchId(shard, jt->cached_batch_id)) {
							order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
							ready.push_back(std::move(*jt));
							emitted.MarkEmitted(static_cast<uint64_t>(seq));
							VLOG(1) << "[L5_LATE_RESEQ] B" << ready.back().broker_id
							        << " seq=" << seq
							        << " next_expected=" << state.next_expected
							        << " (emit-late for ACK progress)";
						} else {
							VLOG(1) << "[L5_LATE_SKIP_DUP] B" << jt->broker_id
							        << " seq=" << seq;
						}
					} else {
						if (shard.hold_buffer_size >= kHoldBufferMaxEntries) {
							std::this_thread::sleep_for(std::chrono::microseconds(10));
							if (shard.deferred_level5.size() < kDeferredL5MaxEntries) {
								shard.deferred_level5.push_back(std::move(*jt));
								continue;
							}
							state.mark_sequenced(seq);
							state.next_expected = seq + 1;
							order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
							order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
							order5_hold_buffer_forced_skips_.fetch_add(1, std::memory_order_relaxed);
							emitted.MarkEmitted(static_cast<uint64_t>(seq));
							ready.push_back(std::move(*jt));
							continue;
						}

						auto& stream_map = shard.hold_buffer[stream_key];
						if (stream_map.find(seq) != stream_map.end()) {
							continue;
						}

						HoldEntry5 he;
						he.batch = std::move(*jt);
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
						stream_map.emplace(seq, he);
						shard.hold_buffer_size++;
						shard.clients_with_held_batches.insert(stream_key);
						InvalidateOrder5HeldSlot(jt->hdr);

						PendingBatch5 marker;
						marker.broker_id = jt->broker_id;
						marker.slot_offset = jt->slot_offset;
						marker.is_held_marker = true;
						marker.hdr = nullptr;
						marker.num_msg = 0;
						ready.push_back(marker);
					}
				}

				bit = broker_end;
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
		ClientEmitTracker& emitted = shard.client_emitted_tracker[cid];
		auto& cmap = map_it->second;
			while (true) {
				auto seq_it = cmap.find(state.next_expected);
				if (seq_it == cmap.end()) break;
				HoldEntry5& he = seq_it->second;
				if (emitted.IsEmitted(static_cast<uint64_t>(he.batch.batch_seq))) {
					state.mark_sequenced(he.batch.batch_seq);
					state.advance_next_expected();
					if (true_client_chain) {
						terminalize_already_emitted(
							cid, he.meta.broker_id, he.meta.start_logical_offset,
							he.meta.num_msg, he.meta.batch_id, he.batch.batch_seq,
							he.meta.pbr_absolute_index);
					} else {
						PendingBatch5 late = std::move(he.batch);
						late.from_hold = true;
						late.hold_meta = he.meta;
						if (!CheckAndInsertBatchId(shard, he.meta.batch_id)) {
							ready.push_back(std::move(late));
							emitted.MarkEmitted(static_cast<uint64_t>(ready.back().batch_seq));
							order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
						}
					}
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
				emitted.MarkEmitted(static_cast<uint64_t>(b.batch_seq));
				ready.push_back(std::move(b));
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
	const bool force_expire_all_frontiers =
		now_ns < force_expire_hold_until_ns_.load(std::memory_order_acquire);
	constexpr uint64_t kForceExpireMinAgeNs = 50ULL * 1000 * 1000;
	for (size_t cid : shard.clients_with_held_batches) {
		auto map_it = shard.hold_buffer.find(cid);
		if (map_it == shard.hold_buffer.end() || map_it->second.empty()) continue;
		if (force_expire_all_frontiers) {
			const auto& front = map_it->second.begin()->second;
			if (now_ns - front.hold_start_ns < kForceExpireMinAgeNs) continue;
			const size_t min_seq = map_it->second.begin()->first;
			shard.expired_hold_keys_buffer.emplace_back(cid, min_seq);
			continue;
		}
		const auto front_it = map_it->second.begin();  // ordered map: front is min sequence
		const size_t min_seq = front_it->first;
		const auto& front = front_it->second;
		if (now_ns - front.hold_start_ns >= GetOrder5HoldTimeoutNs(replication_factor_ > 0)) {
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
		record_expiry(cid);
		RecordOrder5FlightEvent(
			kOrder5FlightExpiry,
			static_cast<uint32_t>(ent.batch.broker_id),
			ent.client_id,
			ent.seq,
			shard.hold_buffer_size,
			force_expire_all_frontiers ? 1 : 0);
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
		ClientEmitTracker& emitted = shard.client_emitted_tracker[ent.client_id];
			if (ent.seq < state.next_expected || emitted.IsEmitted(static_cast<uint64_t>(ent.seq))) {
			state.mark_sequenced(ent.seq);
			// Advance next_expected so later batches (e.g. seq+1) can drain; otherwise
			// we'd leave next_expected behind and hold valid in-order batches forever.
			if (ent.seq >= state.next_expected)
				state.next_expected = ent.seq + 1;
			if (true_client_chain) {
				accumulate_logical_only(
					ent.batch.broker_id,
					ent.meta.start_logical_offset,
					ent.meta.num_msg,
					ent.meta.pbr_absolute_index);
				CheckAndInsertBatchId(shard, ent.meta.batch_id);
				record_late_drop(ent.client_id);
				VLOG(1) << "[L5_EXPIRED_LATE_DROP] B" << ent.batch.broker_id
				        << " seq=" << ent.seq;
			} else if (!CheckAndInsertBatchId(shard, ent.meta.batch_id)) {
				PendingBatch5 b = std::move(ent.batch);
				b.from_hold = true;
				b.hold_meta = ent.meta;
				emitted.MarkEmitted(static_cast<uint64_t>(b.batch_seq));
				ready.push_back(std::move(b));
				order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
				VLOG(1) << "[L5_EXPIRED_LATE_EMIT] B" << ready.back().broker_id
				        << " seq=" << ent.seq;
			} else {
				VLOG(1) << "[L5_EXPIRED_SKIP_DUP] B" << ent.batch.broker_id
				        << " seq=" << ent.seq;
			}
			continue;
		}
		// Sequence is beyond next_expected - skip the gap
		state.next_expected = ent.seq + 1;
		state.mark_sequenced(ent.seq);
		if (!true_client_chain) {
			order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
		}
		order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
		order5_hold_timeout_skips_.fetch_add(1, std::memory_order_relaxed);
		record_forced_skip(ent.client_id);
		if (!CheckAndInsertBatchId(shard, ent.meta.batch_id)) {
			PendingBatch5 b = std::move(ent.batch);
			b.from_hold = true;
			b.hold_meta = ent.meta;
			emitted.MarkEmitted(static_cast<uint64_t>(b.batch_seq));
			ready.push_back(std::move(b));
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
		ClientEmitTracker& emitted = shard.client_emitted_tracker[cid];
		auto& cmap = map_it->second;
			while (true) {
				auto seq_it = cmap.find(state.next_expected);
				if (seq_it == cmap.end()) break;
				HoldEntry5& he = seq_it->second;
				if (emitted.IsEmitted(static_cast<uint64_t>(he.batch.batch_seq))) {
					state.mark_sequenced(he.batch.batch_seq);
					state.advance_next_expected();
					terminalize_already_emitted(
						cid, he.meta.broker_id, he.meta.start_logical_offset,
						he.meta.num_msg, he.meta.batch_id, he.batch.batch_seq,
						he.meta.pbr_absolute_index);
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
				emitted.MarkEmitted(static_cast<uint64_t>(b.batch_seq));
				ready.push_back(std::move(b));
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

void Topic::WriteOrder5AnomalyCountersCsv() const {
	if (order_ != 5 || seq_type_ != heartbeat_system::EMBARCADERO) return;

	const std::string path = "order5_anomaly_counters_broker" + std::to_string(broker_id_) + ".csv";
	const bool write_header = !std::ifstream(path).good();
	std::ofstream out(path, std::ios::app);
	if (!out.is_open()) {
		LOG(ERROR) << "Failed to open anomaly counter CSV: " << path;
		return;
	}
	if (write_header) {
		out << "TimestampNs,Topic,BrokerId,Order,FifoViolations,AckOrderViolations,"
		    << "SkippedBatches,ScannerTimeoutSkips,HoldTimeoutSkips,"
		    << "HoldBufferForcedSkips,StaleEpochSkips\n";
	}
	out << SteadyNowNs() << ","
	    << topic_name_ << ","
	    << broker_id_ << ","
	    << order_ << ","
	    << order5_fifo_violations_.load(std::memory_order_relaxed) << ","
	    << order5_ack_order_violations_.load(std::memory_order_relaxed) << ","
	    << order5_skipped_batches_.load(std::memory_order_relaxed) << ","
	    << order5_scanner_timeout_skips_.load(std::memory_order_relaxed) << ","
	    << order5_hold_timeout_skips_.load(std::memory_order_relaxed) << ","
	    << order5_hold_buffer_forced_skips_.load(std::memory_order_relaxed) << ","
	    << order5_stale_epoch_skips_.load(std::memory_order_relaxed) << "\n";
}

void Topic::WriteOrder5ClientPressureSummary() const {
	if (order_ != kOrderStrong || seq_type_ != heartbeat_system::EMBARCADERO) return;

	absl::btree_map<size_t, ClientPressureStats> aggregate;
	for (const auto& shard_ptr : level5_shards_) {
		if (!shard_ptr) continue;
		for (const auto& [client_id, stats] : shard_ptr->client_pressure_stats) {
			ClientPressureStats& dst = aggregate[client_id];
			dst.hold_inserts += stats.hold_inserts;
			dst.expiries += stats.expiries;
			dst.forced_skips += stats.forced_skips;
			dst.late_drops += stats.late_drops;
			dst.max_held_depth = std::max(dst.max_held_depth, stats.max_held_depth);
		}
	}
	if (aggregate.empty()) return;

	struct ClientPressureRow {
		size_t client_id{0};
		ClientPressureStats stats;
	};
	std::vector<ClientPressureRow> rows;
	rows.reserve(aggregate.size());
	for (const auto& [client_id, stats] : aggregate) {
		rows.push_back(ClientPressureRow{client_id, stats});
	}
	std::sort(rows.begin(), rows.end(),
		[](const ClientPressureRow& a, const ClientPressureRow& b) {
			return std::tie(a.stats.expiries, a.stats.hold_inserts, a.stats.forced_skips,
			                a.stats.late_drops, a.stats.max_held_depth, a.client_id) >
			       std::tie(b.stats.expiries, b.stats.hold_inserts, b.stats.forced_skips,
			                b.stats.late_drops, b.stats.max_held_depth, b.client_id);
		});

	const size_t top_n = std::min<size_t>(rows.size(), 8);
	LOG(INFO) << "[ORDER5_CLIENT_PRESSURE_SUMMARY broker=" << broker_id_
	          << " topic=" << topic_name_
	          << "] tracked_clients=" << rows.size()
	          << " top_n=" << top_n;
	for (size_t i = 0; i < top_n; ++i) {
		const auto& row = rows[i];
		LOG(INFO) << "  client_id=" << row.client_id
		          << " holds=" << row.stats.hold_inserts
		          << " expiries=" << row.stats.expiries
		          << " forced_skips=" << row.stats.forced_skips
		          << " late_drops=" << row.stats.late_drops
		          << " max_held_depth=" << row.stats.max_held_depth;
	}

	const std::string path = "order5_client_pressure_broker" + std::to_string(broker_id_) + ".csv";
	std::ofstream out(path, std::ios::trunc);
	if (!out.is_open()) {
		LOG(ERROR) << "Failed to open client pressure CSV: " << path;
		return;
	}
	out << "ClientId,HoldInserts,Expiries,ForcedSkips,LateDrops,MaxHeldDepth\n";
	for (const auto& row : rows) {
		out << row.client_id << ","
		    << row.stats.hold_inserts << ","
		    << row.stats.expiries << ","
		    << row.stats.forced_skips << ","
		    << row.stats.late_drops << ","
		    << row.stats.max_held_depth << "\n";
	}
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
	scanner_shutdown_drained_[broker_id].store(false, std::memory_order_release);
	if (TopicDiagnosticsEnabled()) {
		LOG(INFO) << "BrokerScannerWorker5 starting for broker " << broker_id;
	}
	// Wait until tinode of the broker is initialized
	while(tinode_->offsets[broker_id].log_offset == 0){
		std::this_thread::yield();
	}
	if (TopicDiagnosticsEnabled()) {
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
	// Empty-slot skip without an authoritative producer tail can leap ahead of real writes
	// and permanently miss batches under startup skew.
	const uint64_t kMaxClaimedWaitUs = 10ULL * 1000 * 1000;       // 10 seconds
	uint64_t hole_wait_start_ns = 0;
	size_t holes_skipped = 0;  // Diagnostic: track how many slots were skipped after timeout

	// [[PEER_REVIEW #10]] Named constants for scanner timing
	constexpr size_t kIdleCyclesThreshold = 1024;    // Idle cycles before sleeping
	constexpr size_t kHeartbeatIterations = 1000000; // [[PERF_OPTIMIZATION]] ~1M iterations at 1M/sec = ~1s heartbeat
	// [[Phase 2.1]] Bounded CLAIMED wait: prevent permanent liveness failure if broker dies mid-write
	constexpr uint64_t kClaimedHealthCheckIntervalUs = 1ULL * 1000 * 1000;  // Check every 1s
	const size_t slot_count = BATCHHEADERS_SIZE / sizeof(BatchHeader);
	constexpr size_t kMaxScannerLeadSlots = 256;
	std::vector<uint64_t> last_pushed_pbr_index_per_slot(slot_count, std::numeric_limits<uint64_t>::max());


	while (!stop_threads_) {
		// If sequencer has already advanced the authoritative consumed frontier beyond our current
		// scanner cursor, jump forward. Otherwise the scanner can keep replaying an already-processed
		// prefix whose slots still look locally readable.
		{
			const void* consumed_addr = const_cast<const void*>(
				reinterpret_cast<const volatile void*>(
					&tinode_->offsets[broker_id].batch_headers_consumed_through));
			CXL::flush_cacheline(consumed_addr);
			CXL::load_fence();
			size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
			if (consumed == BATCHHEADERS_SIZE) consumed = 0;
			if (consumed < BATCHHEADERS_SIZE && (consumed % sizeof(BatchHeader)) == 0) {
				const size_t consumed_slot = consumed / sizeof(BatchHeader);
				const size_t current_slot = static_cast<size_t>(scanner_slot_seq % slot_count);
				const size_t forward = (consumed_slot + slot_count - current_slot) % slot_count;
				if (forward > 0 && forward < (slot_count / 2)) {
					current_batch_header = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(ring_start_default) + consumed);
					scanner_slot_seq += forward;
					hole_wait_start_ns = 0;
					idle_cycles = 0;
				}
			}
		}

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
		// [[RACE_FIX]] Read flags with ACQUIRE first to ensure we see producer's writes.
		uint32_t flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
		bool is_claimed = (flags & kBatchHeaderFlagClaimed) != 0;
		bool is_valid = (flags & kBatchHeaderFlagValid) != 0;

		// num_msg is uint32_t in BatchHeader, so read as volatile uint32_t for type safety
		// For non-coherent CXL: volatile prevents compiler caching; ACQUIRE doesn't help cache coherence
		volatile uint32_t num_msg_check = 0;
		if (is_valid) {
			num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;

			// [P2] Only invalidate second cacheline if batch is valid
			CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(current_batch_header) + 64);
			CXL::load_fence();
		}

		// Max reasonable: 2MB batch / 64B min message = ~32k messages, use 100k as safety limit
		constexpr uint32_t MAX_REASONABLE_NUM_MSG = 100000;
		// ORDER=5 only treats a slot as ready after the receiver has published the full header
		// and set batch_complete=1. This avoids exposing partially received payloads to the
		// epoch sequencer now that per-message paddedSize polling is gone from the hot path.
		volatile int batch_complete_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->batch_complete;
		bool batch_ready = is_valid &&
			(batch_complete_check == 1) &&
			(num_msg_check > 0 && num_msg_check <= MAX_REASONABLE_NUM_MSG);

		// [[PERF_OPTIMIZATION]] Lazy timestamp collection - only when needed for hole timing or heartbeat
		// Removed: std::chrono::steady_clock::now() call from every iteration (~25ns overhead)

		// Per-scanner heartbeat for observability; use iteration counter to avoid timestamp overhead
		if (TopicDiagnosticsEnabled() && ++heartbeat_iteration_count >= kHeartbeatIterations) {
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
				// Re-sync to the authoritative consumed frontier when the scanner lands on an
				// already-processed empty slot after wrap/skip/drain progress elsewhere.
				const void* consumed_addr = const_cast<const void*>(
					reinterpret_cast<const volatile void*>(
						&tinode_->offsets[broker_id].batch_headers_consumed_through));
				CXL::flush_cacheline(consumed_addr);
				CXL::load_fence();
				size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
				if (consumed == BATCHHEADERS_SIZE) consumed = 0;
				if (consumed < BATCHHEADERS_SIZE && (consumed % sizeof(BatchHeader)) == 0) {
					BatchHeader* consumed_header = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(ring_start_default) + consumed);
					if (consumed_header != current_batch_header) {
						current_batch_header = consumed_header;
						scanner_slot_seq = static_cast<uint64_t>(consumed / sizeof(BatchHeader));
						hole_wait_start_ns = 0;
						idle_cycles = 0;
						continue;
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
						volatile int re_complete =
							reinterpret_cast<volatile BatchHeader*>(current_batch_header)->batch_complete;
						if ((re_flags & kBatchHeaderFlagValid) &&
						    re_complete == 1 &&
						    re_num > 0 &&
						    re_num <= MAX_REASONABLE_NUM_MSG) {
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
				order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
				order5_scanner_timeout_skips_.fetch_add(1, std::memory_order_relaxed);

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

					uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
					EpochBuffer5& buf = epoch_buffers_[epoch % 3];
					if (buf.enter_collection(broker_id)) {
						{
							std::lock_guard<std::mutex> data_lock(buf.data_mu);
							buf.per_broker[broker_id].push_back(skip_marker);
						}
						buf.exit_collection(broker_id);
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
	// [PHASE-5] Cache metadata while in L1 (just invalidated both cache lines above)
	pending.cached_log_idx = current_batch_header->log_idx;
	pending.cached_total_size = current_batch_header->total_size;
	pending.cached_batch_id = current_batch_header->batch_id;
	pending.cached_pbr_absolute_index = current_batch_header->pbr_absolute_index;
	pending.cached_start_logical_offset = current_batch_header->start_logical_offset;

	// A valid slot stays readable until the sequencer clears it. Under backlog the scanner can
	// wrap and revisit that same header before consumed_through catches up, which re-enqueues the
	// exact same batch many times. Suppress scanner-side duplicates using the published
	// per-broker absolute PBR index recorded in the slot.
	const size_t slot_index = slot_offset / sizeof(BatchHeader);
	if (slot_index < last_pushed_pbr_index_per_slot.size() &&
	    last_pushed_pbr_index_per_slot[slot_index] == pending.cached_pbr_absolute_index) {
		const void* consumed_addr = const_cast<const void*>(
			reinterpret_cast<const volatile void*>(
				&tinode_->offsets[broker_id].batch_headers_consumed_through));
		CXL::flush_cacheline(consumed_addr);
		CXL::load_fence();
		size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
		if (consumed == BATCHHEADERS_SIZE) consumed = 0;
		if (consumed < BATCHHEADERS_SIZE && (consumed % sizeof(BatchHeader)) == 0) {
			current_batch_header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(ring_start_default) + consumed);
			scanner_slot_seq = static_cast<uint64_t>(consumed / sizeof(BatchHeader));
		}
		std::this_thread::yield();
		continue;
	}

	// [[COMPLETE_DATA_LOSS_FIX]] Once batch is ready, it MUST be pushed to epoch buffer
	// before advancing the scanner position. This ensures no batches are lost during shutdown.
	// Spin-wait until we successfully push to an available epoch buffer.
	{
		bool pushed = false;
		int retry_count = 0;
		constexpr int kMaxRetries = 10000;  // ~1 second of retries with cpu_pause

		while (!pushed && retry_count < kMaxRetries) {
			uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
			EpochBuffer5& cur_buf = epoch_buffers_[epoch % 3];
			if (cur_buf.enter_collection(broker_id)) {
				{
					std::lock_guard<std::mutex> data_lock(cur_buf.data_mu);
					cur_buf.per_broker[broker_id].push_back(pending);
				}
				cur_buf.exit_collection(broker_id);
				pushed = true;
			} else {
				// epoch_index_ can move between the scanner's load and enter_collection(). Allow a
				// successor collecting epoch, but do not spray ready batches arbitrarily two epochs ahead.
				EpochBuffer5& next_buf = epoch_buffers_[(epoch + 1) % 3];
				if (next_buf.enter_collection(broker_id)) {
					{
						std::lock_guard<std::mutex> data_lock(next_buf.data_mu);
						next_buf.per_broker[broker_id].push_back(pending);
					}
					next_buf.exit_collection(broker_id);
					pushed = true;
				}
			}
			if (pushed) break;
			// Recovery: if no buffer is collecting and current epoch is sealed/caught-up,
			// move epoch_index to an IDLE successor and start collection there.
			if ((retry_count % 256) == 0) {
				uint64_t cur_epoch = epoch_index_.load(std::memory_order_acquire);
				EpochBuffer5& cur_buf = epoch_buffers_[cur_epoch % 3];
				EpochBuffer5::State cur_state = cur_buf.state.load(std::memory_order_acquire);
				if (cur_state == EpochBuffer5::State::IDLE) {
					cur_buf.reset_and_start();
				} else if (cur_state == EpochBuffer5::State::SEALED) {
					uint64_t last_seq = last_sequenced_epoch_.load(std::memory_order_acquire);
					if (last_seq < cur_epoch) {
						// Sequencer has not yet consumed the sealed epoch; do not skip ahead.
						CXL::cpu_pause();
						++retry_count;
						if (retry_count % 100 == 0) {
							std::this_thread::sleep_for(std::chrono::microseconds(10));
						}
						continue;
					}
					for (int step = 1; step <= 2; ++step) {
						uint64_t cand_epoch = cur_epoch + static_cast<uint64_t>(step);
						EpochBuffer5& cand = epoch_buffers_[cand_epoch % 3];
						if (cand.state.load(std::memory_order_acquire) != EpochBuffer5::State::IDLE) continue;
						if (!cand.reset_and_start()) {
							continue;
						}
						uint64_t expected = cur_epoch;
						if (!epoch_index_.compare_exchange_strong(expected, cand_epoch, std::memory_order_release)) {
							cand.state.store(EpochBuffer5::State::IDLE, std::memory_order_release);
						}
						break;
					}
				}
			}
			// No buffer currently collecting; pause/retry.
			CXL::cpu_pause();
			++retry_count;
			// Occasional micro-sleep to prevent busy-wait during shutdown
			if (retry_count % 100 == 0) {
				std::this_thread::sleep_for(std::chrono::microseconds(10));
			}
		}

		if (!pushed) {
			// [PANEL C5] Do not advance: retry same slot next iteration (Property 5: Progress).
			// Advancing would lose the batch (PBR slot never sequenced). Sleep then retry.
			if (TopicDiagnosticsEnabled()) {
				uint64_t cur_epoch = epoch_index_.load(std::memory_order_acquire);
				uint64_t last_seq = last_sequenced_epoch_.load(std::memory_order_acquire);
				auto state_to_cstr = [](EpochBuffer5::State s) {
					switch (s) {
						case EpochBuffer5::State::IDLE: return "IDLE";
						case EpochBuffer5::State::RESETTING: return "RESETTING";
						case EpochBuffer5::State::COLLECTING: return "COLLECTING";
						case EpochBuffer5::State::SEALED: return "SEALED";
					}
					return "UNKNOWN";
				};
				EpochBuffer5::State s0 = epoch_buffers_[0].state.load(std::memory_order_acquire);
				EpochBuffer5::State s1 = epoch_buffers_[1].state.load(std::memory_order_acquire);
				EpochBuffer5::State s2 = epoch_buffers_[2].state.load(std::memory_order_acquire);
				LOG(ERROR) << "[PUSH_FAILURE] BrokerScannerWorker5 [B" << broker_id
				           << "] Failed to push batch after " << kMaxRetries << " retries; will retry slot (batch_seq="
				           << pending.batch_seq << "). Do not advancing scanner."
				           << " epoch_index=" << cur_epoch
				           << " last_sequenced_epoch=" << last_seq
				           << " states=[" << state_to_cstr(s0) << ","
				           << state_to_cstr(s1) << ","
				           << state_to_cstr(s2) << "]";
				static std::atomic<uint64_t> push_retry_count{0};
				push_retry_count.fetch_add(1, std::memory_order_relaxed);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
	}
	total_batches_processed++;
	scanner_pushed_batches_[broker_id].fetch_add(1, std::memory_order_relaxed);
	scanner_pushed_msgs_[broker_id].fetch_add(static_cast<uint64_t>(pending.num_msg), std::memory_order_relaxed);
	if (slot_index < last_pushed_pbr_index_per_slot.size()) {
		last_pushed_pbr_index_per_slot[slot_index] = pending.cached_pbr_absolute_index;
	}
	{
		const void* consumed_addr = const_cast<const void*>(
			reinterpret_cast<const volatile void*>(
				&tinode_->offsets[broker_id].batch_headers_consumed_through));
		CXL::flush_cacheline(consumed_addr);
		CXL::load_fence();
		size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
		if (consumed == BATCHHEADERS_SIZE) consumed = 0;
		if (consumed < BATCHHEADERS_SIZE && (consumed % sizeof(BatchHeader)) == 0) {
			const size_t consumed_slot = consumed / sizeof(BatchHeader);
			const size_t next_slot = static_cast<size_t>(scanner_slot_seq % slot_count);
			const size_t lead = (next_slot + slot_count - consumed_slot) % slot_count;
			if (lead > kMaxScannerLeadSlots) {
				std::this_thread::yield();
			}
		}
	}
	idle_cycles = 0;

	// Periodic status logging (VLOG to avoid hot-path overhead during throughput tests)
	auto now = std::chrono::steady_clock::now();
		if (TopicDiagnosticsEnabled() && std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
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

		// [[RACE_FIX]] Read flags with ACQUIRE first
		uint32_t flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
		bool is_valid = (flags & kBatchHeaderFlagValid) != 0;

		volatile uint32_t num_msg_check = 0;
		if (is_valid) {
			num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
		}

		volatile int batch_complete_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->batch_complete;
		bool batch_ready = is_valid &&
			(batch_complete_check == 1) &&
			(num_msg_check > 0 && num_msg_check <= kMaxReasonableNumMsg);

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
			pending.cached_log_idx = current_batch_header->log_idx;
			pending.cached_total_size = current_batch_header->total_size;
			pending.cached_batch_id = current_batch_header->batch_id;
			pending.cached_pbr_absolute_index = current_batch_header->pbr_absolute_index;
			pending.cached_start_logical_offset = current_batch_header->start_logical_offset;

			bool pushed = false;
			for (int r = 0; r < 5000 && !pushed; ++r) {
				uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
				EpochBuffer5& buf = epoch_buffers_[epoch % 3];
				if (buf.enter_collection(broker_id)) {
					{
						std::lock_guard<std::mutex> data_lock(buf.data_mu);
						buf.per_broker[broker_id].push_back(pending);
					}
					buf.exit_collection(broker_id);
					pushed = true;
					drain_pushed++;
					total_batches_processed++;
					scanner_pushed_batches_[broker_id].fetch_add(1, std::memory_order_relaxed);
					scanner_pushed_msgs_[broker_id].fetch_add(static_cast<uint64_t>(pending.num_msg), std::memory_order_relaxed);
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
	scanner_shutdown_drained_[broker_id].store(true, std::memory_order_release);
}

// Helper method to advance consumed_through for processed slots
// [[WRAP_FIX]] Handle ring wrap: when next_expected==BATCHHEADERS_SIZE, slot 0 is the next expected.
void Topic::AdvanceConsumedThroughForProcessedSlots(
    const std::vector<PendingBatch5>& batch_list,
    std::array<size_t, NUM_MAX_BROKERS>& contiguous_consumed_per_broker,
    const std::array<bool, NUM_MAX_BROKERS>& broker_seen_in_epoch,
    const std::array<uint64_t, NUM_MAX_BROKERS>* cv_max_cumulative,
    const std::array<uint64_t, NUM_MAX_BROKERS>* cv_max_pbr_index) {
	if (batch_list.empty()) {
		if (cv_max_cumulative && cv_max_pbr_index) {
			FlushAccumulatedCV(*cv_max_cumulative, *cv_max_pbr_index);
		}
		CXL::store_fence();
		return;
	}

	const size_t slot_size = sizeof(BatchHeader);
	const size_t slot_count = BATCHHEADERS_SIZE / slot_size;
	std::array<std::vector<uint8_t>, NUM_MAX_BROKERS> processed_slots;
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (broker_seen_in_epoch[b]) {
			processed_slots[b].assign(slot_count, 0);
		}
	}
	for (const PendingBatch5& p : batch_list) {
		int b = p.broker_id;
		if (b < 0 || b >= NUM_MAX_BROKERS || !broker_seen_in_epoch[b]) continue;
		if (p.slot_offset >= BATCHHEADERS_SIZE) continue;
		if ((p.slot_offset % slot_size) != 0) continue;
		processed_slots[b][p.slot_offset / slot_size] = 1;
	}
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (!broker_seen_in_epoch[b]) continue;
		size_t next_expected = contiguous_consumed_per_broker[b];
		if (next_expected == BATCHHEADERS_SIZE || next_expected >= BATCHHEADERS_SIZE) {
			next_expected = 0;
		}
		if ((next_expected % slot_size) != 0) continue;
		size_t next_slot = next_expected / slot_size;
		size_t advanced = 0;
		while (advanced < slot_count && processed_slots[b][next_slot]) {
			next_slot = (next_slot + 1) % slot_count;
			advanced++;
		}
		contiguous_consumed_per_broker[b] =
			(next_slot == 0) ? BATCHHEADERS_SIZE : (next_slot * slot_size);
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
