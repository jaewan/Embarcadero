#pragma once
#include "common/config.h"
#include "common/performance_utils.h"  // For CXL:: namespace (flush_cacheline, load_fence)
#include <heartbeat.grpc.pb.h>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cctype>
#include <atomic>

/* CXL memory layout (v2.0 - Phase 1a + Phase 2)
 *
 * Reference: docs/CXL_MEMORY_LAYOUT_v2.md
 *
 * [[PHASE_1A_EPOCH_FENCING]] ControlBlock at offset 0x0 (128 bytes)
 * [[PHASE_2_GOI_CV]]         CompletionVector at 0x1000 (4 KB), GOI at 0x2000 (16 GB)
 *
 * Offset Map (512 GB CXL module):
 * ┌──────────────┬──────────┬──────────────────────────────────────────┐
 * │ 0x0000_0000  │ 128 B    │ ControlBlock (Phase 1a: epoch fencing)   │
 * │ 0x0000_1000  │ 4 KB     │ CompletionVector (Phase 2: ACK path)     │
 * │ 0x0000_2000  │ 16 GB    │ GOI (Phase 2: chain replication)         │
 * │ 0x4_0000_2000│ 1 GB     │ PBR/BatchHeaders (existing)              │
 * │ 0x4_4000_2000│ ~495 GB  │ BLog (existing)                          │
 * │ (legacy)     │ ~24 KB   │ TInode (to be deprecated in Phase 3)     │
 * └──────────────┴──────────┴──────────────────────────────────────────┘
 *
 * Legacy regions (pre-Phase 2):
 * - TINode region: sizeof(TInode) * MAX_TOPIC + padding
 * - Bitmap region: Cacheline_size * NUM_MAX_BROKERS
 * - BatchHeaders region: NUM_MAX_BROKERS * BATCHHEADERS_SIZE * MAX_TOPIC
 * - Segment region: Rest
 */

namespace Embarcadero{

/**
 * Control Block (128 bytes) — Phase 1a/3: Epoch fencing + cluster coordination
 *
 * **Location**: CXL offset 0x0 (first 128 bytes of CXL memory)
 * **Purpose**: Cluster-wide coordination structure for epoch-based fencing, sequencer lease,
 *              broker health tracking, and global sequence progress.
 *
 * **Alignment**: 128 bytes (2 cache lines) to avoid false sharing with adjacent structures.
 *                Modern CPUs prefetch 128 bytes; 64-byte alignment risks invalidation conflicts
 *                when head broker updates epoch and brokers read it concurrently.
 *
 * **Phase 1a (Zombie Fencing — §4.2.1)**:
 *   - Head broker writes epoch=1 on initialization
 *   - All brokers read ControlBlock.epoch periodically (e.g., every 100 batches)
 *   - If broker's cached epoch < ControlBlock.epoch, broker is stale (partitioned)
 *   - Stale brokers stop accepting batches → prevents PBR slot burn DoS
 *
 * **Phase 3 Extensions**:
 *   - sequencer_lease: Heartbeat-based sequencer liveness (§4.2 Sequencer lease protocol)
 *   - committed_seq: Highest global_seq durably written to GOI (§3.3 scatter-gather updater)
 *   - broker_mask: Health bitmap for broker failure detection (§4.2 membership)
 *
 * **Threading Model**:
 *   epoch:           W: head broker (on init/failover); R: all brokers (periodic poll)
 *   sequencer_id:    W: membership service (on election); R: replicas, brokers (lease check)
 *   sequencer_lease: W: active sequencer (renewal every ~40ms); R: watchdog, replicas
 *   broker_mask:     W: membership service; R: sequencer (for recovery decisions)
 *   num_brokers:     W: membership service; R: sequencer, brokers
 *   committed_seq:   W: sequencer committed_seq updater thread (§3.3); R: replicas, subscribers
 *
 * **Ownership**:
 *   - CXL shared memory; no single owner
 *   - epoch: head broker owns initialization and failover updates
 *   - sequencer_lease: active sequencer owns renewal; membership service owns election
 *   - committed_seq: sequencer updater thread (single or scatter-gather mode)
 *
 * **Cache-line Safety**:
 *   - All fields are std::atomic<> for non-coherent CXL memory access
 *   - Writers must CXL::flush_cacheline() after writes (mandatory for CXL non-coherent)
 *   - Readers must invalidate cache before reads (handled by CXL load operations)
 *
 * @threading See field-level threading annotations above
 * @ownership CXL shared; head broker owns epoch init; sequencer owns lease/committed_seq
 * @paper_ref Design §2.4 Control Block, §4.2.1 Zombie slot burn, §4.2 Sequencer lease,
 *            §3.3 Scatter-gather committed_seq updater, §4.2.2 Sequencer-driven recovery
 */
struct alignas(128) ControlBlock {
	// ──────────────────────────────────────────────────────────────────────────
	// Epoch-based fencing (Phase 1a)
	// ──────────────────────────────────────────────────────────────────────────
	/**
	 * Epoch number (monotonic, never decreases).
	 * Incremented on: (1) initial head broker startup, (2) sequencer failover.
	 * Brokers poll this field periodically (e.g., every 100 batches or 10ms).
	 * If broker's cached epoch < ControlBlock.epoch, broker is a zombie (network partition).
	 * Zombie brokers MUST stop accepting batches and exit/reconnect.
	 *
	 * @threading W: head broker (init, failover); R: all brokers (periodic, relaxed)
	 * @invariant Monotonic: epoch[t+1] >= epoch[t]
	 */
	std::atomic<uint64_t> epoch{0};

	// ──────────────────────────────────────────────────────────────────────────
	// Sequencer lease protocol (Phase 3, §4.2)
	// ──────────────────────────────────────────────────────────────────────────
	/**
	 * Sequencer identity (node ID or UUID).
	 * Written by membership service when granting sequencer leadership.
	 * 0 = no active sequencer (election in progress).
	 *
	 * @threading W: membership service (election); R: replicas, brokers (lease check)
	 * @invariant Only one sequencer_id active per epoch
	 */
	std::atomic<uint64_t> sequencer_id{0};

	/**
	 * Sequencer lease expiry timestamp (nanoseconds since epoch, e.g., rdtsc or CLOCK_MONOTONIC).
	 * Active sequencer renews every ~40ms (writes now_ns() + 100ms).
	 * If now_ns() > sequencer_lease, sequencer is considered dead.
	 * Membership service elects new sequencer, increments epoch.
	 *
	 * @threading W: active sequencer (renewal, CXL flush required); R: watchdog, membership
	 * @invariant sequencer_lease[t+1] >= sequencer_lease[t] (monotonic during active tenure)
	 */
	std::atomic<uint64_t> sequencer_lease{0};

	// ──────────────────────────────────────────────────────────────────────────
	// Cluster membership (Phase 3, §4.2)
	// ──────────────────────────────────────────────────────────────────────────
	/**
	 * Broker health bitmap (32 bits for 32 brokers max).
	 * bit[i] = 1: broker i is healthy; bit[i] = 0: broker i failed or not registered.
	 * Used by sequencer for recovery decisions (e.g., skip gap if broker dead).
	 *
	 * @threading W: membership service (health updates); R: sequencer (gap timeout logic)
	 */
	std::atomic<uint32_t> broker_mask{0};

	/**
	 * Number of active brokers in cluster.
	 * Used for cluster size awareness (e.g., scatter-gather shard count, quota).
	 *
	 * @threading W: membership service; R: sequencer, brokers
	 */
	std::atomic<uint32_t> num_brokers{0};

	// ──────────────────────────────────────────────────────────────────────────
	// Global sequence progress (Phase 3, §3.3 scatter-gather)
	// ──────────────────────────────────────────────────────────────────────────
	/**
	 * Highest global_seq durably written to GOI (contiguous prefix).
	 * In single-sequencer mode (§3.2): updated after each epoch commit.
	 * In scatter-gather mode (§3.3): updated by committed_seq updater thread.
	 *
	 * **Scatter-gather invariant**: committed_seq = min(shard_high_water[0..S-1])
	 * where shard_high_water[s] = last GOI index written by logic thread s.
	 * Using min() ensures [0, committed_seq] is gap-free in GOI.
	 *
	 * Replicas and brokers read committed_seq to determine safe read boundaries.
	 * Subscribers can tail-read up to committed_seq without observing gaps.
	 *
	 * @threading W: sequencer committed_seq updater (single writer, periodic); R: replicas, subscribers
	 * @invariant Monotonic: committed_seq[t+1] >= committed_seq[t]
	 * @invariant [0, committed_seq] is fully written in GOI (no gaps)
	 */
	std::atomic<uint64_t> committed_seq{0};

	// ──────────────────────────────────────────────────────────────────────────
	// Padding to 128 bytes (prefetcher safety)
	// ──────────────────────────────────────────────────────────────────────────
	/**
	 * Padding to reach 128 bytes total structure size.
	 * Fields: 6 × 8 bytes + 2 × 4 bytes = 56 bytes (assuming std::atomic<T> == sizeof(T))
	 * However, actual size depends on std::atomic alignment. Padding = 128 - actual_used.
	 * static_assert below validates total size = 128 bytes.
	 */
	uint8_t _pad[80];
};
static_assert(sizeof(ControlBlock) == 128, "ControlBlock must be exactly 128 bytes");
static_assert(alignof(ControlBlock) == 128, "ControlBlock must be 128-byte aligned");

/**
 * CompletionVectorEntry (128 bytes) — Phase 2: Efficient ACK path
 *
 * At CXL offset 0x1000 (4 KB array: 32 brokers × 128 bytes).
 * Each broker has ONE 128-byte slot (avoids false sharing with 128B prefetching).
 * Tail replica updates completed_pbr_head; broker polls ONLY its own slot.
 *
 * Performance: 8-byte read (vs 256-byte replication_done array) → 32× bandwidth reduction.
 *
 * @threading completed_pbr_head: tail replica writes (monotonic); broker reads
 * @ownership CXL shared; tail replica owns writes per broker
 * @paper_ref Design §3.4 Completion Vector, Gap Analysis §3.4 ACK Path Efficiency
 *
 * Sentinel: Head initializes completed_pbr_head to (uint64_t)-1; 0 = first batch completed.
 */
struct alignas(128) CompletionVectorEntry {
	// Highest contiguous PBR index for this broker that is sequenced AND replicated
	// Head initializes to (uint64_t)-1 (no progress); tail writes 0, 1, 2, ...
	std::atomic<uint64_t> completed_pbr_head{0};  // [[writer: tail replica]]

	uint8_t _pad[120];  // Pad to 128 bytes (prefetcher safety: avoid false sharing)
};
static_assert(sizeof(CompletionVectorEntry) == 128, "CompletionVectorEntry must be exactly 128 bytes");
static_assert(alignof(CompletionVectorEntry) == 128, "CompletionVectorEntry must be 128-byte aligned");

/**
 * GOIEntry (64 bytes) — Phase 2: Global Order Index
 *
 * At CXL offset 0x2000 (16 GB array: 256M entries × 64 bytes).
 * Sequencer writes GOI[global_seq]; replicas read GOI, copy data, increment num_replicated.
 * Chain replication: replica R_i waits for num_replicated==i, increments to i+1 (token passing).
 *
 * @threading Sequencer writes all fields except num_replicated; replicas increment num_replicated
 * @ownership Sequencer owns writes; replicas coordinate via num_replicated token
 * @paper_ref Design §2.4 GOI Entry, §3.4 Chain Replication, Gap Analysis §3.3
 */
struct alignas(64) GOIEntry {
	// The definitive ordering
	uint64_t global_seq;                       // [[CRITICAL]] Sequential BATCH index (0, 1, 2, 3...) - used by replicas for polling
	uint64_t batch_id;                         // Globally unique (broker_id | timestamp | counter)
	uint64_t total_order;                      // Starting message sequence number (for message-level ordering)

	// Payload location
	uint16_t broker_id;                        // Which broker's BLog [0-31]
	uint16_t epoch_sequenced;                  // Epoch when sequenced (for staleness detection)
	uint64_t blog_offset;                      // Offset in BLog (64-bit: BLogs are ~15 GB each)
	uint32_t payload_size;                     // Batch size in bytes
	uint32_t message_count;                    // Number of messages in batch

	// Replication progress (chain protocol)
	std::atomic<uint32_t> num_replicated{0};   // 0..R (chain token: replica R_i waits for i, increments to i+1)

	// Per-client ordering info (Level 5) — Populated by Phase 1b sequencer when Level 5 enabled
	uint64_t client_id;                        // Source client (0 if Level 0)
	uint64_t client_seq;                       // Client's sequence number (if Level 5)

	// [[PHASE_2_FIX]] ACK path: Absolute PBR index for CV updater (CompletionVector[broker_id])
	uint64_t pbr_index;                        // [[CRITICAL]] Absolute index (never wraps, monotonic) from BatchHeader.pbr_absolute_index
};
static_assert(sizeof(GOIEntry) <= 128 && sizeof(GOIEntry) % 64 == 0, "GOIEntry must be cache-line multiple (64 or 128)");
static_assert(alignof(GOIEntry) == 64, "GOIEntry must be 64-byte aligned");

using heartbeat_system::SequencerType;
using heartbeat_system::SequencerType::EMBARCADERO;
using heartbeat_system::SequencerType::KAFKA;
using heartbeat_system::SequencerType::SCALOG;
using heartbeat_system::SequencerType::CORFU;

// Separate broker-owned, replication-owned, and sequencer-owned fields to prevent false sharing
// Total: 768 bytes - three independent 256-byte cache-line regions
// [[WIDEN_REPLICATION_DONE]] replication_done changed from int[32] to uint64_t[32]
// [[OFFSET_ENTRY_V2]] - Restructured to enforce strict cache-line separation per writer
// NOTE: Using flat layout (NOT nested structs) to avoid implicit alignment padding
// NOTE: alignas(64) ensures each element in TInode.offsets[] array is cache-line aligned
//       without inflating struct size (768 is already a multiple of 64)
struct alignas(64) offset_entry {
	// ============================================================================
	// BROKER REGION: Cache lines 0-1 (bytes 0-255)
	// [[WRITER: Broker thread only]] - Receives write stage
	// ============================================================================
	volatile size_t log_offset;              // +0 (8B)
	volatile size_t batch_headers_offset;    // +8 (8B)
	volatile size_t written;                 // +16 (8B)
	volatile unsigned long long int written_addr; // +24 (8B)
	uint8_t _pad_broker[256 - 32];           // +32 (224B) - pad first region to 256B
	
	// ============================================================================
	// REPLICATION_DONE REGION: Cache lines 2-3 (bytes 256-511)
	// [[WRITER: Replication thread only]] - Replication progress tracking
	// [[WIDEN_REPLICATION_DONE]] - Each broker gets an 8-byte uint64_t slot (was int[32])
	// ============================================================================
	volatile uint64_t replication_done[NUM_MAX_BROKERS]; // +256 (256B: 8*32 for 32 brokers)
	
	// ============================================================================
	// SEQUENCER REGION: Cache lines 4-5 (bytes 512-767)
	// [[WRITER: Sequencer thread only]] - Global ordering (Stage 3)
	// [[LOCKFREE_RING]] batch_headers_consumed_through: sequencer writes, broker reads (with
	// invalidate) before wrap; allows small ring without overwriting unconsumed slots.
	// ============================================================================
	volatile uint64_t ordered;                        // +512 (8B) - widened to avoid overflow on long runs
	volatile size_t ordered_offset;                   // +520 (8B)
	volatile size_t batch_headers_consumed_through;   // +528 (8B) - byte offset into batch header ring (exclusive)
	uint8_t _pad_sequencer[256 - 8 - 8 - 8];         // +536 (232B) - pad to 256B
};

// Static verification: enforce cache-line separation and size constraints
static_assert(sizeof(offset_entry) == 768, "offset_entry must be exactly 768 bytes (three 256B regions)");
static_assert(alignof(offset_entry) == 64, "offset_entry must be 64-byte aligned");
static_assert(offsetof(offset_entry, log_offset) == 0, "log_offset must be at offset 0");
static_assert(offsetof(offset_entry, replication_done) == 256, "replication_done must be at offset 256 (separate cache line)");
static_assert(offsetof(offset_entry, ordered) == 512, "ordered must be at offset 512 (separate cache line)");
static_assert(offsetof(offset_entry, ordered_offset) == 520, "ordered_offset must be at offset 520 (after 4B padding)");
static_assert(offsetof(offset_entry, batch_headers_consumed_through) == 528, "batch_headers_consumed_through at 528");

// CRITICAL: Each element in offset_entry[NUM_MAX_BROKERS] array in TInode is 768 bytes.
// With alignas(64), each element is guaranteed to be 64-byte aligned, which ensures:
// 1. Cache-line alignment for all three regions (broker @ 0, replication @ 256, sequencer @ 512)
// 2. No false sharing between different offset_entry elements in the array
// 3. Predictable alignment when CXL memory is pre-allocated by CXLManager
// 
// Note: 768 bytes = 12 * 64 bytes, so alignas(64) doesn't inflate struct size.
// Each region (256B) spans exactly 4 cache lines, providing natural isolation.
// TInode.offsets[] array size: 768 * NUM_MAX_BROKERS = 24KB (not inflated to 32KB).

struct alignas(64) TInode{
	struct {
		char topic[TOPIC_NAME_SIZE];
		volatile bool replicate_tinode = false;
		volatile int order;
		volatile int32_t replication_factor;
		volatile int32_t ack_level;
		SequencerType seq_type;
	}__attribute__((aligned(64)));

	volatile offset_entry offsets[NUM_MAX_BROKERS];
};

/**
 * [[PHASE_2_BATCH_LIFECYCLE]] - BatchHeader lifecycle contract
 * 
 * Writer ownership and flush requirements (non-coherent CXL):
 * 
 * Stage 1 (Receiver/NetworkManager):
 *   - Reserve BLog, recv payload into CXL, then reserve PBR slot (post-recv).
 *   - Slot is zeroed on reserve to avoid stale reads (num_msg=0 until publish).
 *   - Writes full BatchHeader once after recv: batch_seq, client_id, num_msg, broker_id, total_size,
 *     start_logical_offset, log_idx, epoch_created, batch_id, pbr_absolute_index.
 *   - Readiness = num_msg>0 for blocking path (no batch_complete needed).
 *   - MUST flush: both cachelines of BatchHeader
 *   - MUST fence: CXL::store_fence() after flush
 * 
 * Stage 3 (Sequencer):
 *   - Reads: num_msg>0 as readiness (blocking path); batch_complete still used by async paths.
 *   - Writes: total_order, ordered=1, batch_off_to_export
 *   - Clears: batch_complete=0, num_msg=0 (prevents duplicate processing / ABA reuse)
 *   - MUST flush: cacheline containing ordered/batch_off_to_export (Stage-4 polls this)
 *   - MUST fence: CXL::store_fence() after flush
 * 
 * Stage 4 (Replication):
 *   - Reads: ordered (polls until == 1), batch_off_to_export, total_size, log_idx
 *   - MUST invalidate: periodic cacheline invalidation + load_fence (every N misses)
 *   - Writes: replication_done (via DiskManager, not directly to BatchHeader)
 * 
 * Stage 5 (ACK/Export):
 *   - Reads: ordered (for export), replication_done (for ack_level=2)
 *   - MUST invalidate: periodic cacheline invalidation for replication_done polling
 * 
 * Field semantics:
 *   - batch_off_to_export==0: This slot IS the export record (simplified ORDER=5 design)
 *   - batch_off_to_export!=0: Points to actual batch header (legacy export chain)
 */
struct alignas(64) BatchHeader{
	// [[LIFECYCLE]]
	// 1) NetworkManager reserves PBR after recv, zeroes slot, writes full header, flush+fence.
	// 2) Sequencer processes batch and clears batch_complete=0 and num_msg=0 (with flush+fence).
	// This prevents duplicate processing of ring slots under ABA reuse.
	// Keep readiness-critical fields in the first cache line for CXL visibility
	size_t batch_seq; // [[WRITER: NetworkManager]] Monotonically increasing from each client. Corfu sets in log's seq
	uint32_t client_id; // [[WRITER: NetworkManager]]
	uint32_t num_msg; // [[WRITER: NetworkManager]] Set by receiver, cleared by sequencer
	volatile uint32_t batch_complete;  // [[WRITER: NetworkManager (set=1), Sequencer (clear=0)]] Batch-level completion flag for Sequencer 5
	uint32_t broker_id; // [[WRITER: NetworkManager]]
	uint32_t ordered; // [[WRITER: Sequencer]] Set to 1 when batch is globally ordered (Stage-4 polls this)
	uint16_t epoch_created; // [[WRITER: NetworkManager]] Epoch when batch was accepted (Phase 1a zombie fencing; sequencer can reject if stale)
	uint16_t _pad0;

	// [[PHASE_2]] Globally unique batch identifier for GOI deduplication and tracking
	uint64_t batch_id; // [[WRITER: NetworkManager]] Format: (broker_id << 48) | (timestamp << 16) | counter

	// [[PHASE_2_FIX]] Absolute PBR index (never wraps, monotonic) for CV tracking
	uint64_t pbr_absolute_index; // [[WRITER: NetworkManager]] Monotonic per-broker batch counter

	size_t total_size; // [[WRITER: NetworkManager]]
	size_t start_logical_offset; // [[WRITER: NetworkManager]]
	size_t log_idx;	// [[WRITER: NetworkManager]] Sequencer4: relative log offset to the payload of the batch and relative offset to last message
	size_t total_order; // [[WRITER: Sequencer]] Global sequence number assigned by sequencer
	size_t batch_off_to_export; // [[WRITER: Sequencer]] Export chain offset (0=in-place, !=0=points to actual batch)
#ifdef BUILDING_ORDER_BENCH
	uint32_t gen;
#endif
#ifdef BUILDING_ORDER_BENCH
    uint64_t publish_ts_ns;
#endif
};
static_assert(sizeof(BatchHeader) % 64 == 0, "BatchHeader must be cache-line sized");
static_assert(sizeof(BatchHeader) == 128, "BatchHeader must be exactly 128 bytes (two cache lines); CompleteBatchInCXL flushes both");
static_assert(offsetof(BatchHeader, client_id) < 64, "client_id must be in first cache line");
static_assert(offsetof(BatchHeader, num_msg) < 64, "num_msg must be in first cache line");
static_assert(offsetof(BatchHeader, batch_complete) < 64, "batch_complete must be in first cache line");
// Note: BATCHHEADERS_SIZE is runtime config, alignment verified at runtime in BrokerScannerWorker5


// Orders are very important to avoid race conditions.
// If you change orders of elements, change how sequencers and combiner check written messages
struct alignas(64) MessageHeader{
	volatile size_t paddedSize; // This include message+padding+header size
	void* segment_header;
	size_t logical_offset;
	volatile unsigned long long int next_msg_diff; // Relative to message_header, not cxl_addr_
	volatile size_t total_order;
	size_t client_order;
	size_t client_id;
	size_t size;
};

/**
 * [[PAPER_SPEC: Implemented]] - BlogMessageHeader: Cache-line partitioned message header
 * Reference: Paper §2.B Table 4 - Broker Log (Blog) message header
 * Purpose: Eliminate false sharing by partitioning header into writer-specific regions
 * 
 * Design Principle: Single Writer per Cache-Line Region
 * - Bytes 0-15:   Receiver writes only (Stage 1: Ingest)
 * - Bytes 16-31:  Delegation writes only (Stage 2: Local Ordering)
 * - Bytes 32-47:  Sequencer writes only (Stage 3: Global Ordering)
 * - Bytes 48-63:  Read-only metadata (set at creation)
 * 
 * Migration Note: Coexists with MessageHeader during migration
 * See docs/memory-bank/activeContext.md Task 3.2 for versioned header pattern
 */
struct alignas(64) BlogMessageHeader {
	// --- Bytes 0-15: Receiver Writes (Stage 1: Ingest) ---
	// [[WRITER: Receiver Thread]] - All fields in bytes 0-15 written by receiver only
	volatile uint32_t size;          // Offset 0 (4 bytes) - Payload size in bytes
	volatile uint32_t received;      // Offset 4 (4 bytes) - Reserved for future: completion flag (not used now; gating uses BatchHeader::batch_complete). Can be used for latency tracing later.
	volatile uint64_t ts;            // Offset 8 (8 bytes) - Reserved for future: receipt timestamp (rdtsc()). Can be used for latency tracing later; not written on hot path.

	// --- Bytes 16-31: Delegation Writes (Stage 2: Local Ordering) ---
	// [[WRITER: Delegation Thread]] - All fields in bytes 16-31 written by delegation only
	volatile uint32_t counter;       // Offset 16 (4 bytes) - Local per-broker sequence number
	volatile uint32_t flags;         // Offset 20 (4 bytes) - Status flags (reserved for future use)
	volatile uint64_t processed_ts;  // Offset 24 (8 bytes) - Processing timestamp (rdtsc())

	// --- Bytes 32-47: Sequencer Writes (Stage 3: Global Ordering) ---
	// [[WRITER: Sequencer Thread]] - All fields in bytes 32-47 written by sequencer only
	volatile uint64_t total_order;   // Offset 32 (8 bytes) - Global sequence number
	volatile uint64_t ordered_ts;    // Offset 40 (8 bytes) - Ordering timestamp (rdtsc())

	// --- Bytes 48-63: Read-Only Metadata ---
	// [[WRITER: None]] - Set at message creation, never modified
	uint64_t client_id;              // Offset 48 (8 bytes) - Client identifier
	uint32_t batch_seq;              // Offset 56 (4 bytes) - Batch sequence number within client
	uint32_t _pad;                   // Offset 60 (4 bytes) - Padding to align to 64 bytes
};
static_assert(sizeof(BlogMessageHeader) == 64, "BlogMessageHeader must be exactly 64 bytes");
static_assert(alignof(BlogMessageHeader) == 64, "BlogMessageHeader must be 64-byte aligned");
static_assert(offsetof(BlogMessageHeader, counter) == 16, "Delegation region must start at byte 16");
static_assert(offsetof(BlogMessageHeader, total_order) == 32, "Sequencer region must start at byte 32");
static_assert(offsetof(BlogMessageHeader, client_id) == 48, "Read-only metadata must start at byte 48");

/**
	 * @brief Check if BlogMessageHeader should be used (feature flag)
	 * @return true if BlogMessageHeader is enabled, false to use legacy MessageHeader
	 * 
	 * Currently checks environment variable EMBARCADERO_USE_BLOG_HEADER.
	 * Future: Could check configuration file or compile-time flag.
	 * 
	 * Default: false (backward compatibility - use MessageHeader)
	 */
namespace HeaderUtils {
	inline bool ShouldUseBlogHeader() {
		static bool cached = false;
		static bool value = false;
		if (!cached) {
			const char* env_val = std::getenv("EMBARCADERO_USE_BLOG_HEADER");
			if (env_val != nullptr) {
				std::string val(env_val);
				std::transform(val.begin(), val.end(), val.begin(), ::tolower);
				value = (val == "1" || val == "true" || val == "yes" || val == "on");
			}
			cached = true;
		}
		return value;
	}

	/**
	 * @brief Initialize BlogMessageHeader receiver fields (bytes 0-15)
	 * @param hdr Pointer to BlogMessageHeader
	 * @param size Payload size in bytes
	 * @param client_id Client identifier
	 * @param batch_seq Batch sequence number
	 * 
	 * Sets receiver region fields and flushes cache line.
	 * Must be called by receiver stage after allocating space and receiving payload.
	 */
	inline void InitBlogReceiverFields(BlogMessageHeader* hdr, uint32_t size,
	                                   uint64_t client_id, uint32_t batch_seq) {
		if (hdr == nullptr) return;

		// Receiver fields (bytes 0-15)
		hdr->size = size;
		hdr->received = 1;  // Mark as received
		// Note: ts will be set by receiver if timestamping is needed
		// For now, leave as 0 (can be optimized later)

		// Read-only metadata (bytes 48-63) - set at creation
		hdr->client_id = client_id;
		hdr->batch_seq = batch_seq;
		hdr->_pad = 0;

		// Delegation and sequencer fields remain 0 (will be set by respective stages)
	}
} // namespace HeaderUtils

/**
 * New cache-line aligned data structures for the disaggregated memory design
 * These structures implement the PBR (Pending Batch Ring) and GOI (Global Order Index)
 * as described in the migration strategy Phase 1.1
 */

/**
 * PendingBatchEntry: Cache-line aligned entry in the Pending Batch Ring (PBR)
 * Each broker has its own PBR containing metadata about batches awaiting sequencing
 */
struct alignas(64) PendingBatchEntry {
    volatile uint64_t batch_id;           // Unique batch identifier (0 = empty slot)
    volatile uint32_t broker_id;          // Source broker ID
    volatile uint32_t client_id;          // Client identifier
    volatile uint32_t batch_seq;          // Client batch sequence number
    volatile uint32_t num_messages;       // Number of messages in this batch
    volatile uint64_t data_offset;        // Offset in BrokerLog where data is stored
    volatile uint32_t data_size;          // Total size of batch data in bytes
    volatile uint32_t ready;              // Status: 0=not ready, 1=ready for sequencing
    uint8_t padding[16];                  // Pad to exactly 64 bytes
};

/**
 * GlobalOrderEntry: Cache-line aligned entry in the Global Order Index (GOI)
 * Central array that records the definitive global order of all batches
 */
struct alignas(64) GlobalOrderEntry {
    volatile uint64_t global_seq_start;   // Starting global sequence number for this batch
    volatile uint32_t num_messages;       // Number of messages in this batch
    volatile uint32_t broker_id;          // Source broker ID
    volatile uint64_t data_offset;        // Offset in BrokerLog where data is stored
    volatile uint32_t data_size;          // Total size of batch data in bytes
    volatile uint32_t status;             // Status: 0=empty, 1=assigned, 2=replicated
    volatile uint64_t epoch;              // Epoch when this entry was created (for fault tolerance)
    uint8_t padding[16];                  // Pad to exactly 64 bytes
};

// Compile-time assertions to ensure proper alignment and size
static_assert(sizeof(PendingBatchEntry) == 64, "PendingBatchEntry must be exactly 64 bytes");
static_assert(alignof(PendingBatchEntry) == 64, "PendingBatchEntry must be 64-byte aligned");
static_assert(sizeof(GlobalOrderEntry) == 64, "GlobalOrderEntry must be exactly 64 bytes");
static_assert(alignof(GlobalOrderEntry) == 64, "GlobalOrderEntry must be 64-byte aligned");

/**
 * BrokerMetadata (Bmeta) structures
 * Purpose: Per-broker coordination metadata with strict cache-line separation
 * 
 * Design Principle: Single Writer Principle
 * - BrokerLocalMeta: ONLY owner broker writes (cache line 0)
 * - BrokerSequencerMeta: ONLY sequencer writes (cache line 1)
 * - Prevents false sharing by ensuring different writers use different cache lines
 * 
 * Migration Note: These structures will coexist with offset_entry during migration
 * See docs/memory-bank/activeContext.md Task 2.3 for dual-write pattern
 */

/**
 * @brief Broker Local Metadata - Cache Line 0 (Broker-Written Only)
 * 
 * @threading Written by owner broker thread only (single writer)
 * @ownership Part of CXL shared memory, owned by CXLManager
 * @alignment Cache-line aligned (64 bytes) to prevent false sharing
 * @paper_ref Paper §2.A Table 5 - Local Struct
 * 
 * Field Mapping from offset_entry:
 * - log_ptr: Maps from offset_entry.log_offset
 * - processed_ptr: Maps from offset_entry.written_addr
 * - replication_done: Maps from offset_entry.replication_done (simplified to counter)
 * - log_start: New field (start of broker's log region)
 * - batch_headers_ptr: Maps from offset_entry.batch_headers_offset
 */
struct alignas(64) BrokerLocalMeta {
    // [[WRITER: Owner Broker]] - All fields in this struct written by broker only
    volatile uint64_t log_ptr;              // Pointer to start of Blog (was: log_offset)
    volatile uint64_t processed_ptr;        // Pointer to last locally-ordered message (was: written_addr)
    volatile uint32_t replication_done;     // Replication status counter (was: replication_done[NUM_MAX_BROKERS])
    volatile uint32_t _reserved1;           // Reserved for future use
    volatile uint64_t log_start;            // Start address of broker's log region
    volatile uint64_t batch_headers_ptr;    // Pointer to batch headers region (was: batch_headers_offset)
    uint8_t padding[24];                    // Explicit padding to ensure exactly 64 bytes
};
static_assert(sizeof(BrokerLocalMeta) == 64, "BrokerLocalMeta must fit in one cache line");
static_assert(alignof(BrokerLocalMeta) == 64, "BrokerLocalMeta must be 64-byte aligned");

/**
 * @brief Broker Sequencer Metadata - Cache Line 1 (Sequencer-Written Only)
 * 
 * @threading Written by sequencer thread only (single writer)
 * @ownership Part of CXL shared memory, owned by CXLManager
 * @alignment Cache-line aligned (64 bytes) to prevent false sharing
 * @paper_ref Paper §2.A Table 5 - Sequencer Struct
 * 
 * Field Mapping from offset_entry:
 * - ordered_seq: Maps from offset_entry.ordered (logical offset becomes sequence number)
 * - ordered_ptr: Maps from offset_entry.ordered_offset
 * - epoch: New field for fault tolerance
 * - status: New field for broker state tracking
 */
struct alignas(64) BrokerSequencerMeta {
    // [[WRITER: Sequencer Thread]] - All fields in this struct written by sequencer only
    volatile uint64_t ordered_seq;         // Global seqno of last ordered msg (was: ordered)
    volatile uint64_t ordered_ptr;          // Pointer to end of last ordered msg (was: ordered_offset)
    volatile uint64_t epoch;                // Epoch for fault tolerance (new field)
    volatile uint32_t status;                // Broker status (0=active, 1=inactive, etc.)
    volatile uint32_t _reserved2;           // Reserved for future use
    uint8_t padding[32];                    // Explicit padding to ensure exactly 64 bytes
};
static_assert(sizeof(BrokerSequencerMeta) == 64, "BrokerSequencerMeta must fit in one cache line");
static_assert(alignof(BrokerSequencerMeta) == 64, "BrokerSequencerMeta must be 64-byte aligned");

/**
 * @brief Broker Metadata - Combined structure (2 cache lines)
 * 
 * @threading Split ownership: local (broker), seq (sequencer)
 * @ownership Part of CXL shared memory, owned by CXLManager
 * @alignment Cache-line aligned (128 bytes = 2 cache lines)
 * @paper_ref Paper §2.A Table 5 - BrokerMetadata (Bmeta)
 * 
 * Memory Layout:
 * - Bytes 0-63:   BrokerLocalMeta (broker writes)
 * - Bytes 64-127: BrokerSequencerMeta (sequencer writes)
 * 
 * Usage:
 *   BrokerMetadata bmeta[NUM_MAX_BROKERS];  // Array in CXL
 *   bmeta[broker_id].local.processed_ptr = addr;  // Broker writes
 *   bmeta[broker_id].seq.ordered_seq = seq;        // Sequencer writes
 * 
 * Migration Strategy:
 * - Phase 1: Coexist with offset_entry (dual-write)
 * - Phase 2: Migrate readers to poll Bmeta instead of TInode
 * - Phase 3: Deprecate offset_entry after validation
 */
struct BrokerMetadata {
    BrokerLocalMeta local;      // Cache line 0: Broker writes only
    BrokerSequencerMeta seq;    // Cache line 1: Sequencer writes only
};
static_assert(sizeof(BrokerMetadata) == 128, "BrokerMetadata must be exactly 2 cache lines");
static_assert(alignof(BrokerMetadata) == 64, "BrokerMetadata must be 64-byte aligned");
static_assert(offsetof(BrokerMetadata, seq) == 64, "Sequencer struct must start at cache line boundary");

// [[PHASE_2]] CXL Memory Layout Constants
constexpr size_t kCompletionVectorOffset = 0x1000;  // CompletionVector at 4 KB from CXL base
constexpr size_t kGOIOffset = 0x2000;               // GOI at 8 KB from CXL base

/**
 * [[PHASE_2_ACK_HELPER]] Get replicated last offset from CompletionVector
 *
 * Reads CV for the given broker, translates PBR index → ring position → BatchHeader,
 * and returns the last logical offset that has been fully replicated.
 *
 * @param cxl_addr Base CXL address
 * @param tinode TInode for this topic
 * @param broker_id Broker ID
 * @param replicated_last_offset Output: Last replicated message offset (0-based), or (size_t)-1 if no progress
 * @return true if replication has progressed, false otherwise
 *
 * @note Caller must ensure ack_level==2, replication_factor>0, seq_type==EMBARCADERO
 * @note Performs CXL cache invalidation (non-coherent CXL)
 */
inline bool GetReplicatedLastOffsetFromCV(void* cxl_addr, TInode* tinode, int broker_id,
                                           size_t& replicated_last_offset) {
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr) + kCompletionVectorOffset);
	CompletionVectorEntry* my_cv_entry = &cv[broker_id];

	// Read CV (tail replica updates this after replication completes)
	CXL::flush_cacheline(my_cv_entry);
	CXL::load_fence();
	uint64_t completed_pbr_index = my_cv_entry->completed_pbr_head.load(std::memory_order_acquire);

	// Sentinel (uint64_t)-1 = no progress; 0 is valid (first batch completed)
	if (completed_pbr_index == static_cast<uint64_t>(-1)) {
		replicated_last_offset = static_cast<size_t>(-1);
		return false;  // No replication progress yet
	}

	// Map absolute PBR index to ring position
	BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr) + tinode->offsets[broker_id].batch_headers_offset);

	size_t ring_size_bytes = BATCHHEADERS_SIZE;
	size_t ring_size = ring_size_bytes / sizeof(BatchHeader);
	size_t ring_position = completed_pbr_index % ring_size;
	BatchHeader* completed_batch = ring_start + ring_position;

	// Read batch header to get replicated logical offset range
	CXL::flush_cacheline(completed_batch);
	CXL::load_fence();
	size_t start_offset = completed_batch->start_logical_offset;
	uint32_t num_msg = completed_batch->num_msg;

	// [[EDGE_CASE]] Handle num_msg == 0 (empty batch, should not happen in replication but be defensive)
	if (num_msg == 0) {
		replicated_last_offset = static_cast<size_t>(-1);
		return false;  // Treat as no progress
	}

	replicated_last_offset = start_offset + num_msg - 1;  // Last message offset (0-based)
	return true;
}

} // End of namespace Embarcadero
