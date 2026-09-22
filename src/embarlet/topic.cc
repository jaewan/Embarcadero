#include "topic.h"
#include "topic_order5_internal.h"
#include "cxl_manager/scalog_local_sequencer.h"
#include "cxl_manager/lazylog_local_sequencer.h"
#include "common/ack_rf_policy.h"
#include "common/performance_utils.h"
#include "common/stage_trace.h"
#include "common/wire_formats.h"
#include "common/order_level.h"
#include "common/env_flags.h"
#include "common/fault_injection.h"
#include "order5_tr_trace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace Embarcadero {
using namespace topic_internal;

namespace {

std::vector<std::string> ResolveLazyLogMetadataEndpoints() {
	const char* value = std::getenv("EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS");
	if (value == nullptr || *value == '\0') return {};
	std::vector<std::string> endpoints;
	std::stringstream stream(value);
	std::string endpoint;
	while (std::getline(stream, endpoint, ',')) {
		const auto first = endpoint.find_first_not_of(" \t");
		const auto last = endpoint.find_last_not_of(" \t");
		if (first != std::string::npos) endpoints.push_back(endpoint.substr(first, last - first + 1));
	}
	return endpoints;
}

std::vector<Corfu::CorfuReplicaTarget> ResolveCorfuChainTargets(
		int source_broker_id, int replication_factor, int num_brokers) {
	const int expected = std::max(0, replication_factor - 1);
	if (expected == 0) return {};
	if (num_brokers < replication_factor) {
		LOG(ERROR) << "Corfu RF=" << replication_factor << " requires at least RF live brokers, got "
		           << num_brokers;
		return {};
	}
	// A complete membership map is deliberate: a single global RF-1 endpoint
	// list is wrong because every source broker has a different ordered chain.
	const char* value = std::getenv("EMBARCADERO_CORFU_REPLICA_ENDPOINTS");
	if (value == nullptr || *value == '\0') {
		LOG(ERROR) << "RF>1 Corfu requires EMBARCADERO_CORFU_REPLICA_ENDPOINTS as "
		           << "broker_id@endpoint entries for every participating broker";
		return {};
	}
	std::unordered_map<int, std::string> membership;
	std::unordered_map<std::string, int> endpoint_owner;
	std::stringstream stream(value); std::string item;
	while (std::getline(stream, item, ',')) {
		const auto first = item.find_first_not_of(" \t"), last = item.find_last_not_of(" \t");
		if (first == std::string::npos) continue;
		item = item.substr(first, last - first + 1);
		const auto at = item.find('@');
		if (at == std::string::npos || at == 0 || at + 1 == item.size()) {
			LOG(ERROR) << "invalid Corfu replica endpoint entry '" << item << "' (expected broker_id@endpoint)";
			return {};
		}
		char* end = nullptr;
		const long id = std::strtol(item.substr(0, at).c_str(), &end, 10);
		if (*end != '\0' || id < 0 || id >= num_brokers) {
			LOG(ERROR) << "invalid Corfu replica broker id in '" << item << "'";
			return {};
		}
		const std::string endpoint = item.substr(at + 1);
		if (!membership.emplace(static_cast<int>(id), endpoint).second ||
			!endpoint_owner.emplace(endpoint, static_cast<int>(id)).second) {
			LOG(ERROR) << "duplicate Corfu replica broker id or endpoint in membership map";
			return {};
		}
	}
	std::vector<Corfu::CorfuReplicaTarget> targets;
	for (int index = 1; index <= expected; ++index) {
		const int target_id = Embarcadero::GetReplicationSetBroker(
			source_broker_id, replication_factor, num_brokers, index);
		auto it = membership.find(target_id);
		if (it == membership.end() || target_id == source_broker_id) {
			LOG(ERROR) << "Corfu chain membership lacks expected target broker " << target_id;
			return {};
		}
		targets.push_back({index, target_id, it->second});
	}
	return targets;
}

}  // namespace

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
		SessionEntry* session_table,
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
	session_table_(session_table),
	logical_offset_(0),
	written_logical_offset_((size_t)-1),
	num_slots_(BATCHHEADERS_SIZE / sizeof(BatchHeader)),
	initial_segment_base_(reinterpret_cast<uintptr_t>(segment_metadata)),
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
        LOG(INFO) << "PBR reservation mode=" << (use_lock_free_pbr_ ? "atomic128" : "mutex")
                  << " slots=" << num_slots_ << " topic=" << topic_name_;
		if (num_slots_ == 0) {
			LOG(ERROR) << "PBR num_slots_ is 0 for topic " << topic_name_
				<< " (BATCHHEADERS_SIZE=" << BATCHHEADERS_SIZE
				<< ", sizeof(BatchHeader)=" << sizeof(BatchHeader) << "); PBR reservation will fail.";
		}

		ack_level_ = tinode_->ack_level;
		replication_factor_ = tinode_->replication_factor;
		ordered_offset_addr_ = nullptr;
		ordered_offset_ = 0;
		validated_written_byte_offset_ = tinode_->offsets[broker_id_].log_offset;
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
			if (replication_factor_ > 1) {
				const int num_brokers = get_num_brokers_callback_();
				const auto targets = ResolveCorfuChainTargets(broker_id_, replication_factor_, num_brokers);
				if (static_cast<int>(targets.size()) != replication_factor_ - 1) {
					LOG(FATAL) << "invalid Corfu ordered chain configuration";
				}
				corfu_replication_client_ = std::make_unique<Corfu::CorfuReplicationClient>(topic_name, replication_factor_, targets);
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
			const auto metadata_endpoints = ResolveLazyLogMetadataEndpoints();
			if (!metadata_endpoints.empty()) {
				if (replication_factor_ <= 0 ||
					static_cast<int>(metadata_endpoints.size()) != replication_factor_) {
					LOG(ERROR) << "LazyLog metadata replication disabled: expected exactly "
					           << replication_factor_ << " endpoints in "
					           << "EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS, got "
					           << metadata_endpoints.size();
				} else {
					lazylog_metadata_replica_client_ =
						std::make_unique<LazyLog::LazyLogMetadataReplicaClient>(metadata_endpoints);
					LOG(INFO) << "LazyLog metadata replication enabled with "
					          << metadata_endpoints.size() << " replicas";
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
				// [[FIX: stale-ring]] Zero the batch-header ring for SCALOG/LAZYLOG before
				// starting DelegationThread. zero_mode=metadata writes zeros to the CXL
				// region but CPU cache may have stale batch_complete=1 from a prior run.
				// An explicit memset+flush guarantees all slots start with batch_complete=0.
				if ((seq_type_ == SCALOG || seq_type_ == LAZYLOG) && cxl_addr_ != nullptr && tinode_ != nullptr) {
					uint8_t* ring_base = reinterpret_cast<uint8_t*>(cxl_addr_) +
					    tinode_->offsets[broker_id_].batch_headers_offset;
					std::memset(ring_base, 0, BATCHHEADERS_SIZE);
					CXL::store_fence();
					CXL::flush_cache_range(ring_base, BATCHHEADERS_SIZE);
					CXL::store_fence();
					LOG(INFO) << "DelegationThread: zeroed batch-header ring broker=" << broker_id_
					          << " size=" << BATCHHEADERS_SIZE << " bytes";
				}
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
					LOG(INFO) << "Creating Sequencer2 thread for Corfu ORDER=2";
					if (replication_factor_ > 0) {
						goi_recovery_thread_ = std::thread(&Topic::GOIRecoveryThread, this);
						LOG(INFO) << "Started GOIRecoveryThread for topic " << topic_name_;
					}
					sequencerThread_ = std::thread(&Topic::Sequencer2, this);
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
	Scalog::ScalogLocalSequencer scalog_local_sequencer(
		tinode_, broker_id_, cxl_addr_, topic_name_, batch_header, this);
	scalog_local_sequencer.SendLocalCut(topic_name_, stop_threads_volatile_);
}

void Topic::StartLazyLogLocalSequencer() {
	BatchHeader* batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	LazyLog::LazyLogLocalSequencer lazylog_local_sequencer(
		tinode_, broker_id_, cxl_addr_, topic_name_, batch_header, this);
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

inline void Topic::PublishValidatedWrittenRange(size_t start_offset, size_t total_size) {
	if (total_size == 0) {
		return;
	}

	size_t range_start = start_offset;
	size_t range_end = start_offset + total_size;
	auto next_it = validated_written_ranges_.lower_bound(range_start);
	if (next_it != validated_written_ranges_.begin()) {
		auto prev_it = std::prev(next_it);
		if (prev_it->second >= range_start) {
			range_start = prev_it->first;
			range_end = std::max(range_end, prev_it->second);
			validated_written_ranges_.erase(prev_it);
		}
	}
	while (next_it != validated_written_ranges_.end() && next_it->first <= range_end) {
		range_end = std::max(range_end, next_it->second);
		auto erase_it = next_it++;
		validated_written_ranges_.erase(erase_it);
	}
	validated_written_ranges_[range_start] = range_end;

	bool advanced = false;
	while (!validated_written_ranges_.empty()) {
		auto it = validated_written_ranges_.begin();
		if (it->first > validated_written_byte_offset_) {
			break;
		}
		if (it->second <= validated_written_byte_offset_) {
			validated_written_ranges_.erase(it);
			continue;
		}
		validated_written_byte_offset_ = it->second;
		validated_written_ranges_.erase(it);
		advanced = true;
	}

	if (!advanced) {
		return;
	}

	tinode_->offsets[broker_id_].validated_written_byte_offset = validated_written_byte_offset_;
	CXL::store_fence();
	CXL::flush_cacheline(CXL::ToFlushable(
		&tinode_->offsets[broker_id_].validated_written_byte_offset));
	CXL::store_fence();
	if (seq_type_ == LAZYLOG) {
		VLOG(1) << "LazyLog validated payload frontier broker=" << broker_id_
		        << " range_start=" << start_offset
		        << " range_end=" << start_offset + total_size
		        << " frontier=" << validated_written_byte_offset_;
	}
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

	// [[DELEGATION_STALL_DIAG]] Track when DelegationThread waits >5s for batch_complete=1.
	// This catches the tail-drain stall where the last batch's PBR slot never gets published.
	auto delegation_spin_start = std::chrono::steady_clock::now();
	bool delegation_stall_logged = false;
	while (!stop_threads_) {
		if (current_batch) {
			// batch_complete is in the first 64B of BatchHeader. Only flush the first
			// cache line while spinning; the second line is only needed after we know
			// batch_complete==1 (saves one CLFLUSHOPT per spin iteration).
			CXL::flush_cacheline(current_batch);
			CXL::load_fence();
		}
		if (current_batch && !__atomic_load_n(&current_batch->batch_complete, __ATOMIC_ACQUIRE)) {
			const auto spin_now = std::chrono::steady_clock::now();
			const auto spin_ms = std::chrono::duration_cast<std::chrono::milliseconds>(spin_now - delegation_spin_start).count();
			if (spin_ms > 5000 && !delegation_stall_logged) {
				delegation_stall_logged = true;
				const size_t slot_offset = reinterpret_cast<uint8_t*>(current_batch) -
					reinterpret_cast<uint8_t*>(delegation_ring_start);
				LOG(WARNING) << "[DelegationThread stall] broker=" << broker_id_
				             << " waiting >5s for batch_complete=1"
				             << " slot_offset=" << slot_offset
				             << " num_msg=" << current_batch->num_msg
				             << " log_idx=" << current_batch->log_idx
				             << " pbr=" << current_batch->pbr_absolute_index
				             << " publish_commit=" << current_batch->publish_commit
				             << " validated_written=" << validated_written_byte_offset_;
			}
		} else {
			delegation_spin_start = std::chrono::steady_clock::now();
			delegation_stall_logged = false;
		}
		if (current_batch && __atomic_load_n(&current_batch->batch_complete, __ATOMIC_ACQUIRE)) {
			CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(current_batch) + 64);
			CXL::load_fence();
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
					// Scalog replication pollers parse next_msg_diff directly from CXL-shared
					// MessageHeaders, so those cachelines must be flushed before advancing
					// validated_written_byte_offset. LazyLog's ApplyGlobalBinding also walks
					// next_msg_diff directly from CXL, so it needs the same per-message flush.
					// Embarcadero subscribers use the network export path and don't require
					// per-message CXL flushes here.
					const bool need_per_msg_flush = (seq_type_ == SCALOG || seq_type_ == LAZYLOG);
					bool touched_message_headers = false;
					for (size_t i = 0; i < current_batch->num_msg; ++i) {
						// [[FIX: NT ingest paddedSize visibility]] For SCALOG/LAZYLOG with
						// NT ingest, message payload is written to CXL via non-temporal stores
						// (bypassing CPU cache). Read paddedSize from CXL (not stale cache)
						// by flushing the cacheline before the plain load.
						if (need_per_msg_flush) {
							CXL::flush_cacheline(msg_ptr);
							CXL::load_fence();
						}
						// [[FIX: logical_offset race]] Use batch->start_logical_offset + i
						// instead of logical_offset_ (which is racy with ScalogGetCXLBuffer
						// concurrently incrementing it per-batch while we process per-message).
						msg_ptr->logical_offset = current_batch->start_logical_offset + i;
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
						// Do NOT dereference msg_ptr->segment_header here. For contiguously
						// packed v1 batch messages, msg_ptr - CACHELINE_SIZE is the PREVIOUS
						// message's payload tail; the old backlink store
						//   *(unsigned long long*)(msg_ptr->segment_header) = 64
						// corrupted 8 payload bytes of every non-last message in a batch.
						// The only readers of that word are dead #ifdef MULTISEGMENT blocks;
						// live consumers navigate via next_msg_diff / paddedSize.
						if (need_per_msg_flush) {
							CXL::flush_cacheline(msg_ptr);
							touched_message_headers = true;
						}

						if (i < current_batch->num_msg - 1) {
							msg_ptr = reinterpret_cast<MessageHeader*>(
								reinterpret_cast<uint8_t*>(msg_ptr) + current_padded_size);
						}

						// For SCALOG/LAZYLOG, logical_offset_ is already advanced by
						// ScalogGetCXLBuffer (by num_msg per batch). Do NOT double-count.
						if (!need_per_msg_flush) {
							logical_offset_++;
						}
					}
					if (touched_message_headers) {
						// [[FIX: CXL replica visibility]] MFENCE instead of SFENCE: ensures
						// CLFLUSHOPTs for next_msg_diff are globally visible before
						// validated_written_byte_offset is updated and seen by the
						// ReplicaPollingLoop on remote brokers (non-coherent CXL 2.0).
						CXL::full_fence();
					}

					if (order_ != 0) {
						const bool count_based_written =
							(seq_type_ == SCALOG || seq_type_ == LAZYLOG);
						// [[FIX: written_val race]] For SCALOG/LAZYLOG use the batch-relative
						// count (start + num_msg) instead of logical_offset_ (which is racy with
						// ScalogGetCXLBuffer concurrently advancing it). For other sequencer types,
						// logical_offset_ is the correct value since it is only incremented here.
						size_t written_val = count_based_written
							? (current_batch->start_logical_offset + current_batch->num_msg)
							: logical_offset_ - 1;
						UpdateTInodeWritten(
							written_val,
							static_cast<unsigned long long int>(
								reinterpret_cast<uint8_t*>(msg_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_)));
						if (seq_type_ == SCALOG) {
							PublishValidatedWrittenRange(current_batch->log_idx, current_batch->total_size);
						} else if (seq_type_ == LAZYLOG) {
							// LazyLog's CXL data-replica pollers consume this same
							// byte frontier.  Publish it with the contiguous-range
							// release protocol rather than an unfenced store: metadata
							// ACKs must not overtake payload visibility at replicas.
							PublishValidatedWrittenRange(current_batch->log_idx, current_batch->total_size);
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
						if (seq_type_ == SCALOG) {
							PublishValidatedWrittenRange(current_batch->log_idx, current_batch->total_size);
						} else if (seq_type_ == LAZYLOG) {
							PublishValidatedWrittenRange(current_batch->log_idx, current_batch->total_size);
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
		CXL::store_fence();
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
	CXL::store_fence();
	CXL::flush_cacheline(seq_region);
	CXL::store_fence();
}

// Segment addresses share the allocator's immutable SEGMENT_SIZE grid. This
// recovers a reservation's original segment after other threads roll forward.
void* Topic::SegmentForAddress(void* address) const {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  const size_t remainder = addr >= initial_segment_base_
      ? (addr - initial_segment_base_) % SEGMENT_SIZE
      : (SEGMENT_SIZE - (initial_segment_base_ - addr) % SEGMENT_SIZE) % SEGMENT_SIZE;
  return reinterpret_cast<void*>(addr - remainder);
}

bool Topic::CheckSegmentBoundary(void* log, size_t msgSize,
                                 unsigned long long segment_metadata) {
  (void)log;
  (void)segment_metadata;
  if (msgSize == 0 || SEGMENT_SIZE <= 64 || msgSize > SEGMENT_SIZE - 64)
    return false;
  absl::MutexLock lock(&segment_rollover_mu_);
  const bool available = TryRolloverSegment(current_segment_, log_addr_, SEGMENT_SIZE,
      msgSize, [this](void* old_segment, uintptr_t cursor) -> void* {
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
        uint64_t reject = 0;
        if (!fault::Pause("storage.before_rollover_allocate",
                {UINT64_MAX, UINT64_MAX, retired_segments_.size(),
                 reinterpret_cast<uintptr_t>(old_segment), cursor}, &stop_threads_, &reject) || reject)
            return nullptr;
#endif
        void* next = get_new_segment_callback_ ? get_new_segment_callback_() : nullptr;
        if (!next) return nullptr;
        const uintptr_t base = reinterpret_cast<uintptr_t>(old_segment);
        // Reservation high-water, not proof of receive completion. Active
        // receives/readers/replicas still own old bytes; never reclaim them.
        *static_cast<uint64_t*>(old_segment) = cursor - base;
        CXL::flush_cacheline(old_segment);
        auto* header = static_cast<uint64_t*>(next);
        header[0] = 0;
        header[1] = ++segment_id_counter_;
        header[2] = ++segment_generation_;
        header[3] = cxl_addr_ ? base - reinterpret_cast<uintptr_t>(cxl_addr_) : 0;
        CXL::flush_cacheline(next);
        CXL::store_fence();
        retired_segments_.push_back(old_segment);
        return next;
      });
  if (!available) blog_capacity_exhausted_.store(true, std::memory_order_release);
  return available;
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
		log = TryReserveBLogSpaceFailClosed(batch_header.total_size, true);
        if (!log) { batch_header_location = nullptr; return nullptr; }
		logical_offset = logical_offset_;
		segment_header = SegmentForAddress(log);
		start_logical_offset = logical_offset_;
		logical_offset_ += batch_header.num_msg;


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

                // Segment reservation high-water is sealed by rollover. A callback
                // may finish after rollover and must not write the new segment header.

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
	const size_t msg_size = batch_header.total_size;
	BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);
	const size_t slot = batch_header.batch_seq % num_slots_;
	BatchHeader* slot_header = &batch_header_log[slot];

	// Get log address with batch offset
    uintptr_t assigned_address;
    if (!LocateAssignedPayload(initial_segment_base_,
          reinterpret_cast<uintptr_t>(first_message_addr_), SEGMENT_SIZE,
          batch_header.log_idx, msg_size, assigned_address)) {
      log = nullptr;
      blog_capacity_exhausted_.store(true, std::memory_order_release);
      return nullptr;
    }
    log = reinterpret_cast<void*>(assigned_address);
    segment_header = reinterpret_cast<void*>(initial_segment_base_);

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
	CXL::store_fence();
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
		if (replication_factor_ > 1 && corfu_replication_client_) {
			const Corfu::CorfuAppendDescriptor descriptor{
				Corfu::CorfuSlotKey{topic_name_, static_cast<uint32_t>(broker_id_), batch_header.batch_seq},
				Corfu::CorfuValueId{batch_header.client_id, batch_header.original_client_batch_seq, batch_header.total_order,
					batch_header.num_msg, batch_header.total_size},
				batch_header.log_idx, log, batch_header.total_size};
			const bool replicated = corfu_replication_client_->AppendOrdered(descriptor);
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
		} // end: remote chain RF>=2

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
			CXL::store_fence();
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
		CXL::store_fence();
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
	if (seq_type_ != CORFU || order_ != Embarcadero::kOrderTotal || replication_factor_ <= 1) {
		return;
	}

	uint64_t messages_to_ack = 0;
	absl::flat_hash_map<uint32_t, uint64_t> per_client_delta;
	{
		absl::MutexLock lock(&corfu_order2_durable_mu_);
		std::vector<DurableBatch> newly_contiguous;
		if (!corfu_order2_durable_frontier_.Record(
				batch_seq, DurableBatch{num_msg, client_id}, &newly_contiguous)) {
			LOG(WARNING) << "Corfu ORDER=2 durable: duplicate completion for batch_seq=" << batch_seq;
			return;
		}
		for (const DurableBatch& batch : newly_contiguous) {
			messages_to_ack += batch.message_count;
			per_client_delta[batch.client_id] += batch.message_count;
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
			CXL::store_fence();
			CXL::flush_cacheline(const_cast<const void*>(
					reinterpret_cast<const volatile void*>(
							&replica_tinode_->offsets[b].replication_done[broker_id_])));
		}
		tinode_->offsets[b].replication_done[broker_id_] = last_offset;
		CXL::store_fence();
		CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(
				&tinode_->offsets[b].replication_done[broker_id_])));
	}
	CXL::store_fence();

	for (auto& [cid, cnt] : per_client_delta) {
		UpdatePerClientDurable(cid, cnt);
	}
}

void Topic::UpdatePerClientWritten(uint32_t client_id, uint64_t count) {
	absl::MutexLock lock(&per_client_mu_);
	per_client_written_[client_id] += count;
}

void Topic::RecordPerClientOrderedVisibility(uint32_t client_id, uint64_t count) {
	UpdatePerClientOrdered(client_id, count);
}

void Topic::RecordPerClientDurableVisibility(uint32_t client_id, uint64_t count) {
	UpdatePerClientDurable(client_id, count);
}

void Topic::RecordLazyLogMetadataReplicaAck(uint64_t start_logical_offset,
		uint32_t num_msg, uint32_t client_id) {
	if (!SupportsPerClientAppendAckLevel1() || num_msg == 0) return;
	absl::MutexLock lock(&lazylog_append_mu_);
	auto [it, inserted] = lazylog_metadata_ready_.emplace(
		start_logical_offset, std::make_pair(num_msg, client_id));
	if (!inserted && it->second != std::make_pair(num_msg, client_id)) {
		LOG(ERROR) << "LazyLog metadata descriptor conflicts at logical offset "
		           << start_logical_offset;
	}
}

void Topic::MaybeAdvanceLazyLogAppendVisibility() const {
	if (!SupportsPerClientAppendAckLevel1() || replication_factor_ <= 0) return;
	const int num_brokers = get_num_brokers_callback_();
	uint64_t data_frontier = std::numeric_limits<uint64_t>::max();
	int ready_replicas = 0;
	for (int i = 0; i < replication_factor_; ++i) {
		const int replica = Embarcadero::GetReplicationSetBroker(
			broker_id_, replication_factor_, num_brokers, i);
		volatile uint64_t* done = &tinode_->offsets[replica].replication_done[broker_id_];
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(done)));
		CXL::load_fence();
		const uint64_t value = *done;
		if (value == kReplicationNotStarted) continue;
		++ready_replicas;
		data_frontier = std::min(data_frontier, value + 1);
	}
	if (ready_replicas < replication_factor_) return;

	absl::MutexLock lock(&lazylog_append_mu_);
	while (true) {
		auto it = lazylog_metadata_ready_.find(lazylog_append_next_logical_offset_);
		if (it == lazylog_metadata_ready_.end()) break;
		const uint64_t end = lazylog_append_next_logical_offset_ + it->second.first;
		if (end > data_frontier) break;
		per_client_lazylog_append_[it->second.second] += it->second.first;
		lazylog_append_progress_.fetch_add(it->second.first, std::memory_order_release);
		lazylog_append_next_logical_offset_ = end;
		lazylog_metadata_ready_.erase(it);
	}
}

void Topic::RecordOrder0DurableBatch(uint64_t durable_sequence_key, uint32_t num_msg, uint32_t client_id) {
	// Only EMBARCADERO ORDER=0 uses this path; SCALOG and LAZYLOG credit
	// per_client_durable_ exclusively through their sequencer DrainDurableBatches
	// (Path B), which gates on ordered AND min(replication_done). Including them
	// here double-credits every message and allows clients to exit ACK-wait
	// when only ~50% of bytes are actually durable.
	const bool uses_replication_done_durability =
		(seq_type_ == EMBARCADERO && order_ == 0);
	if (!uses_replication_done_durability || replication_factor_ <= 0 || num_msg == 0) {
		return;
	}
	absl::MutexLock lock(&per_client_durable_mu_);
	auto [it, inserted] = order0_durable_pending_.emplace(durable_sequence_key,
	                                                      std::make_pair(num_msg, client_id));
	if (!inserted) {
		if (it->second != std::make_pair(num_msg, client_id)) {
			LOG(WARNING) << "ORDER0 durable pending duplicate/conflict key=" << durable_sequence_key
			             << " old_num_msg=" << it->second.first
			             << " old_client=" << it->second.second
			             << " new_num_msg=" << num_msg
			             << " new_client=" << client_id;
		}
	}
}

void Topic::MaybeAdvanceOrder0DurableVisibility() const {
	const bool uses_replication_done_durability =
		(seq_type_ == EMBARCADERO && order_ == 0);
	if (!uses_replication_done_durability || replication_factor_ <= 0) {
		return;
	}

	int num_brokers = get_num_brokers_callback_ ? get_num_brokers_callback_() : NUM_MAX_BROKERS_CONFIG;
	if (num_brokers <= 0) {
		num_brokers = NUM_MAX_BROKERS_CONFIG;
	}

	size_t min_replication_done = std::numeric_limits<size_t>::max();
	int ready_replicas = 0;
	for (int i = 0; i < replication_factor_; ++i) {
		int b = Embarcadero::GetReplicationSetBroker(broker_id_, replication_factor_, num_brokers, i);
		volatile uint64_t* rep_done_ptr = &tinode_->offsets[b].replication_done[broker_id_];
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(rep_done_ptr)));
		CXL::load_fence();
		const size_t val = *rep_done_ptr;
		if (val == kReplicationNotStarted) {
			continue;
		}
		ready_replicas++;
		if (val < min_replication_done) {
			min_replication_done = val;
		}
	}

	if (ready_replicas < replication_factor_ || min_replication_done == kReplicationNotStarted) {
		return;
	}

	const uint64_t durable_frontier = min_replication_done + 1;
	absl::MutexLock lock(&per_client_durable_mu_);
	while (order0_durable_logical_count_ < durable_frontier) {
		auto it = order0_durable_pending_.find(order0_durable_next_sequence_key_);
		if (it == order0_durable_pending_.end()) {
			break;
		}
		const uint32_t num_msg = it->second.first;
		const uint32_t client_id = it->second.second;
		const uint64_t batch_end = order0_durable_logical_count_ + num_msg;
		if (batch_end > durable_frontier) {
			break;
		}

		per_client_durable_[client_id] += num_msg;
		order0_durable_logical_count_ = batch_end;
		order0_durable_next_sequence_key_ += num_msg;
		order0_durable_pending_.erase(it);
	}
}

void Topic::UpdatePerClientOrdered(uint32_t client_id, uint64_t count) {
	absl::MutexLock lock(&per_client_mu_);
	per_client_ordered_[client_id] += count;
	per_client_ordered_epoch_[client_id] = CurrentControlEpoch();
	StageTrace::Record(
		StageTrace::Stage::OrderedFrontier,
		client_id,
		per_client_ordered_[client_id],
		SteadyNowNs(),
		count);
}

uint64_t Topic::GetClientWritten(uint32_t client_id) const {
	absl::MutexLock lock(&per_client_mu_);
	auto it = per_client_written_.find(client_id);
	return (it != per_client_written_.end()) ? it->second : 0;
}

uint64_t Topic::GetClientOrdered(uint32_t client_id) const {
	absl::MutexLock lock(&per_client_mu_);
	auto it = per_client_ordered_.find(client_id);
	return (it != per_client_ordered_.end()) ? it->second : 0;
}

uint64_t Topic::GetClientOrderedEpoch(uint32_t client_id) const {
	absl::MutexLock lock(&per_client_mu_);
	auto it = per_client_ordered_epoch_.find(client_id);
	return (it != per_client_ordered_epoch_.end()) ? it->second : 0;
}

void Topic::PublishSessionFenceNotification(const SessionFenceNotification& note) {
	absl::MutexLock lock(&session_fence_mu_);
	pending_session_fences_[note.client_id] = note;
}

bool Topic::TakeSessionFenceNotification(uint32_t client_id, SessionFenceNotification* out) {
	if (out == nullptr) return false;
	absl::MutexLock lock(&session_fence_mu_);
	auto it = pending_session_fences_.find(client_id);
	if (it == pending_session_fences_.end()) return false;
	*out = it->second;
	pending_session_fences_.erase(it);
	return true;
}

uint64_t Topic::CurrentControlEpoch() const {
	uint64_t epoch = cached_epoch_.load(std::memory_order_acquire);
	if (cxl_addr_ == nullptr) return epoch;
	const uint64_t now_ns = SteadyNowNs();
	uint64_t last_ns = last_control_epoch_refresh_ns_.load(std::memory_order_acquire);
	if (epoch != 0 && now_ns - last_ns < 1'000'000ULL) return epoch;
	if (!last_control_epoch_refresh_ns_.compare_exchange_strong(
			last_ns, now_ns, std::memory_order_acq_rel, std::memory_order_acquire)) {
		return cached_epoch_.load(std::memory_order_acquire);
	}
	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::invalidate_cacheline_for_read(control_block);
	CXL::load_fence();
	const uint64_t refreshed = control_block->epoch.load(std::memory_order_acquire);
	cached_epoch_.store(refreshed, std::memory_order_release);
	return refreshed;
}

void Topic::RestampRecoveredOrderedEpochs(uint64_t producing_epoch) {
	if (producing_epoch == 0) return;
	absl::MutexLock lock(&per_client_mu_);
	for (const auto& [session_key, unused_next_expected] : recovered_order5_next_expected_) {
		(void)unused_next_expected;
		const uint32_t client_id = static_cast<uint32_t>(session_key >> 32);
		per_client_ordered_epoch_[client_id] = producing_epoch;
	}
}

bool Topic::AckRelayEpochValid(
		uint32_t client_id,
		uint64_t* producing_epoch_out,
		uint64_t* control_epoch_out) const {
	const uint64_t control_epoch = CurrentControlEpoch();
	uint64_t producing_epoch = 0;
	{
		absl::MutexLock lock(&per_client_mu_);
		auto it = per_client_ordered_epoch_.find(client_id);
		if (it != per_client_ordered_epoch_.end()) {
			producing_epoch = it->second;
		}
	}
	if (producing_epoch_out) *producing_epoch_out = producing_epoch;
	if (control_epoch_out) *control_epoch_out = control_epoch;
	// producing_epoch==0 means this client has never been stamped by CommitEpoch.
	// Withholding then blocks the zero frontier forever whenever ControlBlock.epoch
	// was bumped at sequencer start (1→2). Only withhold once the client has a
	// real producing stamp that lags the control epoch (zombie / stale session).
	if (producing_epoch == 0) {
		return true;
	}
	return !ShouldWithholdAckRelay(producing_epoch, control_epoch);
}

uint64_t Topic::GetClientDurable(uint32_t client_id) const {
	MaybeAdvanceOrder0DurableVisibility();
	absl::MutexLock lock(&per_client_durable_mu_);
	auto it = per_client_durable_.find(client_id);
	return (it != per_client_durable_.end()) ? it->second : 0;
}

uint64_t Topic::GetClientAppend(uint32_t client_id) const {
	MaybeAdvanceLazyLogAppendVisibility();
	absl::MutexLock lock(&lazylog_append_mu_);
	auto it = per_client_lazylog_append_.find(client_id);
	return (it != per_client_lazylog_append_.end()) ? it->second : 0;
}

bool Topic::SupportsPerClientAppendAckLevel1() const {
	return seq_type_ == LAZYLOG && order_ == Embarcadero::kOrderTotal &&
	       lazylog_metadata_replica_client_ != nullptr;
}

uint64_t Topic::GetLazyLogAppendProgress() const {
	MaybeAdvanceLazyLogAppendVisibility();
	return lazylog_append_progress_.load(std::memory_order_acquire);
}

bool Topic::SupportsPerClientAckLevel1() const {
	// ACK level 1 is "ordered frontier" semantics.
	// We currently maintain per-client ordered frontier in:
	// - CORFU ORDER=2 via RecordCorfuOrder2BatchCompletion
	// - LAZYLOG ORDER=2 and SCALOG ORDER=1 via local sequencer export publication
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
	if (seq_type_ == LAZYLOG) {
		return order_ == Embarcadero::kOrderTotal;
	}
	if (seq_type_ == SCALOG) {
		return order_ == Embarcadero::kOrderPerBroker;
	}
	if (seq_type_ == EMBARCADERO) {
		return order_ == Embarcadero::kOrderPerBroker;
	}
	return false;
}

bool Topic::SupportsPerClientWrittenAckLevel1() const {
	return seq_type_ == EMBARCADERO &&
	       order_ == 0;
}

bool Topic::SupportsPerClientAckLevel2Durable() const {
	if (seq_type_ == EMBARCADERO) {
		// ACK2 requires RF>=2 (CXL primary + at least one media-durable replica).
		if (replication_factor_ < Embarcadero::kMinReplicationFactorForAck2) {
			return false;
		}
		return order_ == 0 || order_ == Embarcadero::kOrderStrong;
	}
	if (seq_type_ == CORFU) {
		// RF includes the already-published CXL primary.  The ordered driver
		// returns only after every RF-1 remote sidecar has acknowledged its
		// data+metadata sync, so RF2 and RF3 share this ACK2 contract.
		return order_ == Embarcadero::kOrderTotal &&
		       replication_factor_ >= Embarcadero::kMinReplicationFactorForAck2 &&
		       corfu_replication_client_ != nullptr;
	}
	if (seq_type_ == LAZYLOG) {
		return order_ == Embarcadero::kOrderTotal &&
		       replication_factor_ >= Embarcadero::kMinReplicationFactorForAck2 &&
		       lazylog_metadata_replica_client_ != nullptr;
	}
	if (seq_type_ == SCALOG) {
		return order_ == Embarcadero::kOrderPerBroker &&
		       replication_factor_ >= Embarcadero::kMinReplicationFactorForAck2;
	}
	return false;
}

void Topic::UpdatePerClientDurable(uint32_t client_id, uint64_t count) {
	absl::MutexLock lock(&per_client_durable_mu_);
	per_client_durable_[client_id] += count;
	// Canary: for EMBARCADERO ORDER=0 only — the only path where tinode->written
	// is actively maintained by UpdateWrittenForOrder0 on the network-receive thread.
	// SCALOG and LAZYLOG do not update tinode->written, so comparing against it would
	// always fire (false alarm): they use DrainDurableBatches (Path B) exclusively.
	if (tinode_ != nullptr && seq_type_ == EMBARCADERO && order_ == 0) {
		const uint64_t written = tinode_->offsets[broker_id_].written;
		const uint64_t new_client_durable = per_client_durable_[client_id];
		if (new_client_durable > written + 1) {
			LOG(WARNING) << "[ACK2 canary] per_client_durable_ EXCEEDS broker written"
			             << " broker=" << broker_id_
			             << " client=" << client_id
			             << " durable=" << new_client_durable
			             << " written=" << written
			             << " -- double-credit bug detected";
		}
	}
}

void Topic::MaybeAdvanceOrder5DurableFromCV() {
	if (order_ != Embarcadero::kOrderStrong || seq_type_ != EMBARCADERO) return;
	if (replication_factor_ < Embarcadero::kMinReplicationFactorForAck2) return;
	if (cxl_addr_ == nullptr) return;

	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	GOIEntry* goi = reinterpret_cast<GOIEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kGOIOffset);
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	CXL::invalidate_cacheline_for_read(control_block);
	CXL::load_fence();
	const uint64_t committed_seq = control_block->committed_seq.load(std::memory_order_acquire);
	if (committed_seq == UINT64_MAX) return;

	std::array<uint64_t, NUM_MAX_BROKERS> durable_logical{};
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		CXL::invalidate_cacheline_for_read(&cv[b]);
		CXL::load_fence();
		durable_logical[b] = cv[b].completed_logical_offset.load(std::memory_order_acquire);
	}

	const uint64_t now_ns = SteadyNowNs();
	// Bound/surface: warn when a disk-replica tail pins durable attribution.
	static constexpr uint64_t kDurablePinWarnNs = 30ULL * 1000ULL * 1000ULL * 1000ULL;
	absl::MutexLock lock(&per_client_durable_mu_);
	for (int owner = 0; owner < NUM_MAX_BROKERS; ++owner) {
		uint64_t& cursor = order5_durable_next_goi_by_owner_[static_cast<size_t>(owner)];
		bool pinned_this_pass = false;
		uint64_t pin_needed_cumulative = 0;
		while (cursor <= committed_seq) {
			GOIEntry* entry = &goi[cursor];
			ReadGOIEntryFresh(entry);
			if (entry->global_seq != cursor) {
				break;
			}
			const int entry_owner = static_cast<int>(entry->broker_id);
			if (entry_owner != owner) {
				++cursor;
				continue;
			}
			// Empty/skip GOI entries have nothing to wait for on disk.
			if (entry->message_count == 0) {
				++cursor;
				continue;
			}
			if (durable_logical[static_cast<size_t>(owner)] < entry->cumulative_message_count) {
				pinned_this_pass = true;
				pin_needed_cumulative = entry->cumulative_message_count;
				break;
			}
			if (entry->client_id != 0 || entry->message_count > 0) {
				per_client_durable_[static_cast<uint32_t>(entry->client_id)] += entry->message_count;
				StageTrace::Record(
					StageTrace::Stage::DurableFrontier,
					entry->client_id,
					cursor,
					SteadyNowNs(),
					per_client_durable_[static_cast<uint32_t>(entry->client_id)]);
			}
			++cursor;
		}
		uint64_t& pin_since = order5_durable_pin_since_ns_[static_cast<size_t>(owner)];
		if (pinned_this_pass) {
			if (pin_since == 0) {
				pin_since = now_ns;
			} else if (now_ns - pin_since >= kDurablePinWarnNs) {
				LOG_EVERY_N(ERROR, 64)
					<< "[ORDER5_DURABLE_PIN] owner=" << owner
					<< " pinned_ms=" << ((now_ns - pin_since) / 1'000'000ULL)
					<< " goi_cursor=" << cursor
					<< " durable_logical=" << durable_logical[static_cast<size_t>(owner)]
					<< " need_cumulative=" << pin_needed_cumulative
					<< " committed_seq=" << committed_seq
					<< " (disk-replica tail stall; durable ACK waits on CV completed_logical)";
			}
		} else {
			pin_since = 0;
		}
	}
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
		void* skipped_addr = TryReserveBLogSpaceFailClosed(batch_header.total_size, true);
        if (!skipped_addr) { log = nullptr; return nullptr; }

		// Store for later retrieval
		skipped_batch_[batch_header.client_id].emplace(client_seq, skipped_addr);

		// Move log address forward (assuming same batch size)


		// Update client sequence
		client_seq += num_brokers;
	}

	// Allocate space for this batch
	log = TryReserveBLogSpaceFailClosed(batch_header.total_size, true);
    if (!log) return nullptr;
    segment_header = SegmentForAddress(log);
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
	const size_t msg_size = batch_header.total_size;
	void* batch_headers_log;

	{
		absl::MutexLock lock(&mutex_);

		// Allocate space in log
		log = TryReserveBLogSpaceFailClosed(msg_size, true);
    if (!log) { batch_header_location = nullptr; return nullptr; }

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
	segment_header = SegmentForAddress(log);

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
	{
		absl::MutexLock lock(&mutex_);
		logical_offset = logical_offset_;
		logical_offset_ += batch_header.num_msg;
	}
	batch_header.start_logical_offset = logical_offset;
	batch_header.broker_id = broker_id_;
    batch_header.pbr_absolute_index = pbr_idx;
    batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) | pbr_idx;

    BatchHeader* batch_header_ring = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
    size_t num_slots = BATCHHEADERS_SIZE / sizeof(BatchHeader);
    size_t slot_idx = static_cast<size_t>(pbr_idx % num_slots);
    batch_header_location = &batch_header_ring[slot_idx];

	// Calculate addresses
	const size_t msg_size = batch_header.total_size;

	// Allocate space in log
	log = TryReserveBLogSpaceFailClosed(msg_size, true);
    if (!log) { batch_header_location = nullptr; return nullptr; }
    batch_header.log_idx = reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_);

	// Check for segment boundary
	segment_header = SegmentForAddress(log);

	// Install the same unpublished ownership claim used by the common
	// post-receive PBR path. Scalog reserves before recv so its replication
	// callback can retain the chosen payload address, but publication still
	// validates this identity after recv. Merely clearing batch_complete (the
	// old baseline path) is no longer sufficient once publication fails closed
	// on stale/ring-reused slots.
	InstallPBRClaim(batch_header, batch_header_location);

    size_t rep_offset = 0;
    if (!kCxlScalogMode) {
        rep_offset = scalog_batch_offset_.fetch_add(batch_header.total_size, std::memory_order_relaxed);
    }

	// Return replication callback
	// ACK2 credit for SCALOG goes through ScalogLocalSequencer::DrainDurableBatches
	// (Path B) gated on ordered AND min(replication_done). No direct call here.
	return [this, batch_header, log, rep_offset, kCxlScalogMode](void* log_ptr, size_t /*placeholder*/) {
		bool data_replication_submitted = kCxlScalogMode;
		// Handle replication if needed
		if (!kCxlScalogMode && replication_factor_ > 0 && scalog_replication_client_) {
			data_replication_submitted = scalog_replication_client_->ReplicateData(
						rep_offset,
						batch_header.total_size,
						batch_header.num_msg,
						log
				);
		}
		if (!data_replication_submitted) {
			LOG(ERROR) << "Scalog data replication failed; not recording durable batch";
			return;
		}
		// ACK2 credit handled by ScalogLocalSequencer::DrainDurableBatches (Path B).
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
	{
		absl::MutexLock lock(&mutex_);
		logical_offset = logical_offset_;
		logical_offset_ += batch_header.num_msg;
	}
	batch_header.start_logical_offset = logical_offset;
	batch_header.broker_id = broker_id_;
	batch_header.pbr_absolute_index = pbr_idx;
	batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) | pbr_idx;

	BatchHeader* batch_header_ring = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	size_t num_slots = BATCHHEADERS_SIZE / sizeof(BatchHeader);
	size_t slot_idx = static_cast<size_t>(pbr_idx % num_slots);
	batch_header_location = &batch_header_ring[slot_idx];

	const size_t msg_size = batch_header.total_size;

	log = TryReserveBLogSpaceFailClosed(msg_size, true);
    if (!log) { batch_header_location = nullptr; return nullptr; }
	batch_header.log_idx = reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_);
	segment_header = SegmentForAddress(log);

	// LazyLog shares Scalog's pre-receive reservation path and therefore needs
	// the same complete ownership claim before PublishPBRSlotDirect validates it.
	InstallPBRClaim(batch_header, batch_header_location);

	size_t rep_offset = 0;
	if (!kCxlLazyLogMode) {
		rep_offset = scalog_batch_offset_.fetch_add(batch_header.total_size, std::memory_order_relaxed);
	}
	return [this, batch_header, log, rep_offset, kCxlLazyLogMode](void* /*log_ptr*/, size_t /*placeholder*/) {
		bool data_replication_submitted = kCxlLazyLogMode;
		if (!kCxlLazyLogMode && replication_factor_ > 0 && scalog_replication_client_) {
			data_replication_submitted = scalog_replication_client_->ReplicateData(
				rep_offset, batch_header.total_size, batch_header.num_msg, log);
		}
		if (!lazylog_metadata_replica_client_) return;
		if (!data_replication_submitted) {
			LOG(ERROR) << "LazyLog data replication failed before metadata append";
			return;
		}
		lazylogmetadata::MetadataAppendRequest request;
		request.set_topic(topic_name_);
		request.set_source_broker_id(static_cast<uint32_t>(broker_id_));
		request.set_source_batch_seq(batch_header.pbr_absolute_index);
		request.set_client_id(batch_header.client_id);
		request.set_client_batch_seq(batch_header.batch_seq);
		request.set_payload_offset(batch_header.log_idx);
		request.set_payload_size(batch_header.total_size);
		request.set_num_messages(batch_header.num_msg);
		VLOG(1) << "LazyLog metadata append descriptor source_broker="
		        << request.source_broker_id()
		        << " source_batch_seq=" << request.source_batch_seq()
		        << " client=" << request.client_id()
		        << " num_messages=" << request.num_messages();
		std::string error;
		if (!lazylog_metadata_replica_client_->AppendToAll(request, &error)) {
			LOG(ERROR) << "LazyLog metadata replication failed for pbr="
			           << batch_header.pbr_absolute_index << ": " << error;
			return;
		}
		RecordLazyLogMetadataReplicaAck(batch_header.start_logical_offset,
			batch_header.num_msg, batch_header.client_id);
		// ACK2 credit for LAZYLOG goes through LazyLogLocalSequencer::
		// DrainDurableBatches -> RecordPerClientDurableVisibility (Path B).
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

	// Calculate message size for allocation. Segment base is re-read inside
	// TryReserveBLogSpaceFailClosed to avoid stale segment_metadata races.
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

	// [[PERF Phase 1.1]] Lock-free BLog allocation with fail-closed bounds.
	// Never return a pointer past the mapped segment end (SIGSEGV under CXL exhaustion).
	// PBR allocation is handled separately in ReservePBRSlotAfterRecv(); only log_addr_ is updated here.
	// [[P1.2]] When replication=0 no O_DIRECT pwrite; use cache-line alignment to reduce padding.
	constexpr size_t kODirectAlign = 4096;
	constexpr size_t kCacheLineAlign = 64;
	size_t align = (replication_factor_ > 0) ? kODirectAlign : kCacheLineAlign;
	size_t alloc_size = (msg_size + align - 1) & ~(align - 1);
	log = TryReserveBLogSpaceFailClosed(alloc_size, /*epoch_already_checked=*/true);
	if (log == nullptr) {
		batch_header_location = nullptr;
		return nullptr;
	}

#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
    if (!fault::Pause("ingress.after_blog_reserve",
            {batch_header.client_id, batch_header.session_epoch, batch_header.batch_seq,
             reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_), alloc_size},
            &stop_threads_)) {
        // The network allocation loop retries null while capacity is available.
        // Cancellation must not burn a fresh reservation on every retry.
        blog_capacity_exhausted_.store(true, std::memory_order_release);
        log = nullptr;
        return nullptr;
    }
#endif

	// [[DESIGN: PBR reserve after receive]] Do NOT generate metadata or write the BatchHeader here.
	// NetworkManager reserves a PBR slot after recv(payload) and generates metadata once.
	// For non-EMBARCADERO sequencers: ReservePBRSlotAndWriteEntry generates metadata.
	// Leave batch_header unchanged; caller will fill it.
	batch_header_location = nullptr;

	return nullptr;
}

bool Topic::IsIngestBatchAlreadySeen(uint32_t client_id, uint64_t batch_seq,
		uint32_t session_epoch) const {
	IngestDedupKey key{client_id, session_epoch, batch_seq};
	absl::MutexLock lock(&ingest_dedup_mu_);
	if (ingest_seen_.contains(key)) {
		ingest_dedup_hits_.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	return false;
}

void Topic::MarkIngestBatchSeen(uint32_t client_id, uint64_t batch_seq,
		uint32_t session_epoch) {
	IngestDedupKey key{client_id, session_epoch, batch_seq};
	absl::MutexLock lock(&ingest_dedup_mu_);
	if (!ingest_seen_.insert(key).second) {
		return;
	}
	ingest_seen_ring_.push_back(key);
	while (ingest_seen_ring_.size() > kIngestDedupCap) {
		ingest_seen_.erase(ingest_seen_ring_.front());
		ingest_seen_ring_.pop_front();
	}
}

// [[Issue #3]] Single epoch check per batch; call once at batch start, pass epoch_already_checked to ReserveBLogSpace/ReservePBRSlotAndWriteEntry.
bool Topic::CheckEpochOnce() {
	uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
	bool force_full_read = (n % kEpochCheckInterval == 0);
	auto [current_epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
	last_checked_epoch_.store(current_epoch, std::memory_order_release);
	return was_stale;
}

void* Topic::TryReserveBLogSpaceFailClosed(size_t size, bool epoch_already_checked) {
	if (!epoch_already_checked) {
		uint64_t n = epoch_check_counter_.fetch_add(1, std::memory_order_relaxed);
		bool force_full_read = (n % kEpochCheckInterval == 0);
		auto [current_epoch, was_stale] = RefreshBrokerEpochFromCXL(force_full_read);
		if (was_stale) return nullptr;
	}

	if (IsBLogCapacityExhausted() || size == 0 || SEGMENT_SIZE <= 64 ||
        size > SEGMENT_SIZE - 64) return nullptr;
    for (int retry = 0; retry < 64; ++retry) {
      const auto base = reinterpret_cast<uintptr_t>(
          current_segment_.load(std::memory_order_acquire));
      uintptr_t result;
      if (TryReserveSegmentBytes(log_addr_, base, SEGMENT_SIZE, size, result))
        return reinterpret_cast<void*>(result);
      if (!CheckSegmentBoundary(nullptr, size, base)) return nullptr;
    }
	return nullptr;
}

// [[RECV_DIRECT_TO_CXL]] Lock-free BLog allocation (~10ns).
// [[PHASE_1A_EPOCH_FENCING]] Design §4.2.1: refuse writes if broker epoch is stale (zombie broker).
// [[Issue #3]] When epoch_already_checked true, skip epoch check (caller did CheckEpochOnce at batch start).
void* Topic::ReserveBLogSpace(size_t size, bool epoch_already_checked) {
	return TryReserveBLogSpaceFailClosed(size, epoch_already_checked);
}

void Topic::RefreshPBRConsumedThroughCache() {
  // One observer at a time prevents a delayed modulo sample looking like a
  // second lap. Contending producers use the conservative existing cache.
  if (pbr_refresh_busy_.test_and_set(std::memory_order_acquire)) return;
  const void* addr = const_cast<const void*>(reinterpret_cast<const volatile void*>(
      &tinode_->offsets[broker_id_].batch_headers_consumed_through));
  CXL::flush_cacheline(addr);
  CXL::load_fence();
  const size_t consumed = tinode_->offsets[broker_id_].batch_headers_consumed_through;
  if (consumed <= BATCHHEADERS_SIZE && consumed % sizeof(BatchHeader) == 0 && num_slots_ >= 2) {
    cached_pbr_consumed_through_.store(consumed, std::memory_order_release);
    const uint64_t modulo = consumed == BATCHHEADERS_SIZE ? 0 : consumed / sizeof(BatchHeader);
    const auto previous = cached_consumed_seq_.load(std::memory_order_relaxed);
    cached_consumed_seq_.store(UnwrapPBRConsumed(previous, modulo, num_slots_),
                               std::memory_order_release);
  }
  pbr_refresh_busy_.clear(std::memory_order_release);
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

bool Topic::ReservePBRSlotLockFree(uint32_t num_msg, size_t& out_byte_offset,
                                    size_t& out_logical_offset, uint64_t& out_absolute) {
  uint64_t slot, logical;
  if (!TryReservePBR(pbr_state_, cached_consumed_seq_, num_slots_, num_msg,
                     slot, logical, [this] { RefreshPBRConsumedThroughCache(); }, &out_absolute))
    return false;
  out_byte_offset = slot * sizeof(BatchHeader);
  out_logical_offset = logical;
  return true;
}

bool Topic::ReservePBRSlotCore(BatchHeader& batch_header, void* log, bool epoch_already_checked,
		void*& batch_headers_log, size_t& logical_offset, void*& segment_header) {
	if (!first_batch_headers_addr_ || IsBLogCapacityExhausted()) return false;

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


    uint64_t absolute_index;
	if (use_lock_free_pbr_) {
		size_t byte_offset;
		if (!ReservePBRSlotLockFree(batch_header.num_msg, byte_offset, logical_offset, absolute_index))
			return false;
		batch_headers_log = reinterpret_cast<void*>(
			reinterpret_cast<uintptr_t>(first_batch_headers_addr_) + byte_offset);
    } else {
      // Platforms without the native 128-bit fast path retain the existing
      // producer mutex but share the same absolute-sequence admission logic.
      absl::MutexLock lock(&mutex_);
      size_t byte_offset;
      if (!ReservePBRSlotLockFree(batch_header.num_msg, byte_offset, logical_offset,
                                 absolute_index)) return false;
      batch_headers_log = reinterpret_cast<uint8_t*>(first_batch_headers_addr_) + byte_offset;
      const size_t next_offset = ((absolute_index + 1) % num_slots_) * sizeof(BatchHeader);
      batch_headers_ = reinterpret_cast<uintptr_t>(first_batch_headers_addr_) + next_offset;
      cached_next_slot_offset_.store(next_offset, std::memory_order_release);
      logical_offset_ = logical_offset + batch_header.num_msg;
    }

	// Receive owns its original reservation; a concurrent rollover is harmless.
	segment_header = SegmentForAddress(log);

	batch_header.start_logical_offset = logical_offset;
	batch_header.broker_id = broker_id_;
	batch_header.ordered = 0;
	batch_header.total_order = 0;
	batch_header.pbr_absolute_index = absolute_index;
	batch_header.batch_id = (static_cast<uint64_t>(broker_id_) << 48) | batch_header.pbr_absolute_index;
	batch_header.epoch_created = static_cast<uint16_t>(std::min(current_epoch, static_cast<uint64_t>(0xFFFF)));
	batch_header.log_idx = static_cast<size_t>(
		reinterpret_cast<uintptr_t>(log) - reinterpret_cast<uintptr_t>(cxl_addr_));
	return true;
}

void Topic::InstallPBRClaim(const BatchHeader& batch_header,
		BatchHeader* batch_header_location) {
	CHECK(batch_header_location != nullptr);
	BatchHeader claimed = batch_header;
	claimed.flags = kBatchHeaderFlagClaimed;
	claimed.batch_complete = 0;
	claimed.publish_commit = kBatchHeaderPublishUncommitted;

	absl::MutexLock lifecycle_lock(&pbr_slot_lifecycle_mu_);
	memcpy(batch_header_location, &claimed, sizeof(BatchHeader));
	CXL::store_fence();
	if (CXL::ExplicitFlushRequired()) {
		CXL::flush_cacheline(batch_header_location);
		CXL::flush_cacheline(
			reinterpret_cast<const uint8_t*>(batch_header_location) + 64);
		CXL::store_fence();
	}
}

bool Topic::ReservePBRSlotAfterRecv(BatchHeader& batch_header, void* log,
		void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
		bool epoch_already_checked) {
	void* batch_headers_log;
	if (!ReservePBRSlotCore(batch_header, log, epoch_already_checked, batch_headers_log, logical_offset, segment_header)) {
		batch_header_location = nullptr;
		return false;
	}
	BatchHeader* slot = reinterpret_cast<BatchHeader*>(batch_headers_log);
	// Serialize claim replacement against a delayed receiver's ownership check
	// and publication. The payload receive itself remains fully concurrent.
	// ORDER=5 scanners gate readiness on publish_commit + batch_complete, so
	// exposing this immutable claim is safe and gives timeout recovery enough
	// identity to retire a specific client-sequence gap.
	InstallPBRClaim(batch_header, slot);
	batch_header_location = slot;
	return true;
}

bool Topic::PublishPBRSlotDirect(const BatchHeader& batch_header, BatchHeader* batch_header_location) {
	if (!batch_header_location) return false;
	CHECK(batch_header.pbr_absolute_index != kBatchHeaderPublishUncommitted)
		<< "publish_commit sentinel collides with pbr_absolute_index";
	absl::MutexLock lifecycle_lock(&pbr_slot_lifecycle_mu_);
	if (!BatchHeaderClaimOwnedBy(*batch_header_location, batch_header)) {
		const uint64_t rejected =
			stale_pbr_publish_rejects_.fetch_add(1, std::memory_order_relaxed) + 1;
		if (rejected == 1 || (rejected & (rejected - 1)) == 0) {
			LOG(WARNING) << "Rejecting stale PBR publication: expected_abs="
			             << batch_header.pbr_absolute_index
			             << " expected_batch=" << batch_header.batch_id
			             << " slot_abs=" << batch_header_location->pbr_absolute_index
			             << " slot_batch=" << batch_header_location->batch_id
			             << " slot_flags=0x" << std::hex
			             << batch_header_location->flags << std::dec
			             << " slot_complete=" << batch_header_location->batch_complete
			             << " slot_publish_commit="
			             << batch_header_location->publish_commit
			             << " rejects=" << rejected;
		}
		return false;
	}
	BatchHeader published = batch_header;
	// Keep CLAIMED set after publish so scanners never misclassify a published slot as empty tail
	// if num_msg visibility lags on non-coherent CXL.
	published.flags |= kBatchHeaderFlagClaimed;
	published.flags |= kBatchHeaderFlagValid;
	published.publish_commit = kBatchHeaderPublishUncommitted;
	memcpy(batch_header_location, &published, sizeof(BatchHeader));
	__atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE);

	// BatchHeader is 128B; flush both cachelines for non-coherent CXL visibility.
	CXL::store_fence();
	if (CXL::ExplicitFlushRequired()) {
		CXL::flush_cacheline(batch_header_location);
		const void* batch_header_next_line = reinterpret_cast<const void*>(
			reinterpret_cast<const uint8_t*>(batch_header_location) + 64);
		CXL::flush_cacheline(batch_header_next_line);
		CXL::store_fence();

		// Publish the slot only after the full header is already durable in CXL.
		batch_header_location->publish_commit = batch_header.pbr_absolute_index;
		CXL::store_fence();
		CXL::flush_cacheline(batch_header_next_line);
		CXL::store_fence();
	} else {
		// Coherent mapping: release store of publish_commit is sufficient.
		batch_header_location->publish_commit = batch_header.pbr_absolute_index;
		CXL::store_fence();
	}
	return true;
}

bool Topic::PublishPBRSlotAfterRecv(const BatchHeader& batch_header, BatchHeader* batch_header_location) {
	if (!batch_header_location) return false;

	bool ok = false;
	if (seq_type_ == EMBARCADERO && Embarcadero::UsesEpochSequencerPath(order_)) {
		// ORDER=4/5 already tolerate out-of-arrival-order batches in the sequencer via hold/expiry
		// hold/expiry logic. Delaying slot publication until pbr_absolute_index becomes contiguous
		// can strand a large tail of fully received batches behind one missing index. Make each
		// completed batch visible immediately and let the sequencer/CV path recover contiguity.
		ok = PublishPBRSlotDirect(batch_header, batch_header_location);
	} else {
		ok = PublishPBRSlotDirect(batch_header, batch_header_location);
	}
	if (ok && seq_type_ == EMBARCADERO && order_ == Embarcadero::kOrderStrong) {
		const uint32_t session_epoch =
			batch_header.session_epoch32 != 0
				? batch_header.session_epoch32
				: static_cast<uint32_t>(batch_header.session_epoch);
		MarkIngestBatchSeen(static_cast<uint32_t>(batch_header.client_id),
		                    batch_header.batch_seq,
		                    session_epoch);
	}
	return ok;
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
	batch_header.publish_commit = kBatchHeaderPublishUncommitted;
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

/**
 * Get message address and size for topic subscribers
 *
 * Note: Current implementation depends on the subscriber knowing the physical
 * address of last fetched message. This is only true if messages were exported
 * from CXL. For disk cache optimization, we'd need to implement indexing.
 *
 * @return true if more messages are available
 */
void Topic::PushOrder0Batch(uint64_t log_idx, uint32_t total_size, uint32_t num_msg,
		uint64_t start_logical_offset, uint32_t client_id,
		uint16_t header_version) {
	uint64_t cursor = order0_batch_write_cursor_.fetch_add(1, std::memory_order_relaxed);
	auto& slot = order0_batch_ring_[cursor & (kOrder0BatchRingSize - 1)];
	slot.log_idx = log_idx;
	slot.start_logical_offset = start_logical_offset;
	slot.total_size = total_size;
	slot.num_msg = num_msg;
	slot.client_id = client_id;
	slot.header_version = header_version;
	// Release store: makes slot metadata visible to readers after sequence.
	slot.sequence.store(cursor + 1, std::memory_order_release);
}

bool Topic::ReadOrder0Batch(uint64_t& read_cursor,
		uint64_t& out_log_idx,
		uint32_t& out_total_size,
		uint32_t& out_num_msg,
		uint16_t& out_header_version) const {
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
	out_header_version = slot.header_version;
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

// Order 2: Total order (no per-client ordering). Same epoch pipeline as Sequencer5, but no Level 5
// partition or hold buffer; all batches go directly to ready in arrival order. Design §2.3 Order 2.
void Topic::Sequencer2() {
	LOG(INFO) << "Starting Sequencer2 (Order 2 total order, no per-client state) for topic: " << topic_name_;
	ResetCompletedRangeQueue();
	global_batch_seq_.store(0, std::memory_order_release);
	{
		ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
		control_block->committed_seq.store(UINT64_MAX, std::memory_order_release);
		CXL::store_fence();
		CXL::flush_cacheline(control_block);
		CXL::store_fence();
	}
	committed_seq_updater_stop_.store(false, std::memory_order_release);
	committed_seq_updater_thread_ = std::thread(&Topic::CommittedSeqUpdaterThread, this);
	// Same Level-5 shard state as Sequencer5: EpochSequencerThread always calls
	// ProcessLevel5Batches (hold-buffer tick + per-epoch partition). Without this,
	// level5_shards_ is empty and ProcessLevel5Batches dereferences level5_shards_[0].
	InitLevel5Shards();
	// [[ORDER2_RECOVERY_GATE]] ORDER=2 has no per-session ORDER=5 state to recover; the
	// EpochSequencerThread's ProcessLevel5Batches CHECK on order5_recovery_complete_ fires
	// unless we set it here. For ORDER=2, level5 paths are no-ops (all batches have
	// client_id==0 → level0), so setting the flag unconditionally is safe.
	order5_recovery_complete_.store(true, std::memory_order_release);
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
	CXL::store_fence();
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
	export_sequence_by_broker_.fill(0);
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
	// [[TR_TRACE]] All ORDER=5 producer threads have joined; write the trace CSV.
	Order5TrTrace::Instance().Flush();
}

} // namespace Embarcadero

/**
 * Start/Stop GOIRecoveryThread (added in Topic constructor/destructor)
 * This is a placeholder - actual thread start/stop will be added to constructor/destructor
 */
