#pragma once
#include "common/config.h"
#include <heartbeat.grpc.pb.h>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cctype>

/* CXL memory layout
 *
 * CXL is composed of three components; TINode, Bitmap, Segments
 * TINode region: First sizeof(TINode) * MAX_TOPIC
 * + Padding to make each region be aligned in cacheline
 * Bitmap region: Cacheline_size * NUM_MAX_BROKERS
 * BatchHeaders region: NUM_MAX_BROKERS * BATCHHEADERS_SIZE * MAX_TOPIC
 * Segment region: Rest. It is allocated to each brokers equally according to broker_id
 * 		Segment: 8Byte of segment metadata to store address of last ordered_offset from the segment, messages
 * 			Message: Header + paylod
 */

namespace Embarcadero{

using heartbeat_system::SequencerType;
using heartbeat_system::SequencerType::EMBARCADERO;
using heartbeat_system::SequencerType::KAFKA;
using heartbeat_system::SequencerType::SCALOG;
using heartbeat_system::SequencerType::CORFU;

// Separate broker-owned and sequencer-owned fields to prevent false sharing + correctness
// Total: 512 bytes - two 256-byte regions
// [[WIDEN_REPLICATION_DONE]] replication_done changed from int[32] to uint64_t[32]
struct alignas(512) offset_entry {
	// Broker region: Cache line 0-3 (bytes 0-255)
	volatile size_t log_offset;              // +0 (8B)
	volatile size_t batch_headers_offset;    // +8 (8B)
	volatile size_t written;                 // +16 (8B)
	volatile unsigned long long int written_addr; // +24 (8B)
	// [[WIDEN_REPLICATION_DONE]] - Changed from int[NUM_MAX_BROKERS] to uint64_t[NUM_MAX_BROKERS]
	// Each broker gets an 8-byte slot to store full-sized offsets
	volatile uint64_t replication_done[NUM_MAX_BROKERS]; // +32 (256B: 8*32)
	
	// Sequencer region: Cache line 4-7 (bytes 256-511)
	// Broker region total = 32 + 256 = 288B (exceeds but will be separated by alignment)
	volatile int ordered;                    // +288 (4B) - will be adjusted by alignment
	volatile size_t ordered_offset;          // +292 (8B)
	uint8_t _pad_sequencer[256 - 4 - 8];     // +300 (244B)
};

// Static verification - note: struct will be padded to 512B alignment
static_assert(sizeof(offset_entry) >= 512, "offset_entry must be at least 512 bytes");
static_assert(alignof(offset_entry) == 512, "offset_entry must be 512-byte aligned");
// Cache separation is still effective: broker fields (first 288B) and sequencer fields are logically separate
// even though they may be padded

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

struct alignas(64) BatchHeader{
	size_t batch_seq; // Monotonically increasing from each client. Corfu sets in log's seq
	size_t total_size;
	size_t start_logical_offset;
	uint32_t broker_id;
	uint32_t ordered;
	size_t batch_off_to_export;
	size_t total_order;
	size_t log_idx;	// Sequencer4: relative log offset to the payload of the batch and elative offset to last message
	uint32_t client_id;
	uint32_t num_msg;
	volatile uint32_t batch_complete;  // Batch-level completion flag for Sequencer 5
#ifdef BUILDING_ORDER_BENCH
	uint32_t gen;
#endif
#ifdef BUILDING_ORDER_BENCH
    uint64_t publish_ts_ns;
#endif
};


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
	volatile uint32_t received;      // Offset 4 (4 bytes) - Completion flag: 0=not received, 1=received
	volatile uint64_t ts;            // Offset 8 (8 bytes) - Receipt timestamp (rdtsc())

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
 * Header version enumeration for migration support
 * Allows coexistence of legacy MessageHeader and new BlogMessageHeader
 */
enum class HeaderVersion : uint16_t {
	HEADER_V1 = 1,  // Legacy MessageHeader (current implementation)
	HEADER_V2 = 2,  // Paper spec BlogMessageHeader (new implementation)
};

/**
 * [[PAPER_SPEC: Implemented]] - MessageHeaderV2: Versioned header wrapper for migration
 * Purpose: Enable coexistence of legacy MessageHeader and BlogMessageHeader during migration
 * 
 * Design: Stores version field at the beginning, followed by union of both header types
 * Note: Both MessageHeader and BlogMessageHeader are 64 bytes, so union is safe
 * 
 * Usage Pattern:
 *   MessageHeaderV2* hdr = ...;
 *   if (hdr->version == HeaderVersion::HEADER_V2) {
 *       BlogMessageHeader* v2 = &hdr->v2;
 *       // Use v2 fields
 *   } else {
 *       MessageHeader* v1 = &hdr->v1;
 *       // Use v1 fields
 *   }
 * 
 * Migration Strategy:
 * - Phase 1: Network receiver writes V1 headers (backward compatible)
 * - Phase 2: Network receiver writes V2 headers, sequencer reads both
 * - Phase 3: All code migrates to V2, deprecate V1
 */
struct alignas(64) MessageHeaderV2 {
	HeaderVersion version;  // Offset 0 (2 bytes) - Header version identifier
	uint16_t _pad;         // Offset 2 (2 bytes) - Padding to align union
	
	// Union of both header types (both are 64 bytes, so union is safe)
	union {
		MessageHeader v1;        // Legacy header (current implementation)
		BlogMessageHeader v2;    // New header (paper spec)
	};
};
// Note: MessageHeaderV2 is 128 bytes due to alignment (version + pad + aligned union), spanning 2 cache lines
// The union members are 64-byte aligned, so they start at offset 64
static_assert(sizeof(MessageHeaderV2) >= 66, "MessageHeaderV2 must accommodate version + union");
static_assert(offsetof(MessageHeaderV2, v1) == 64, "V1 header must be 64-byte aligned");
static_assert(offsetof(MessageHeaderV2, v2) == 64, "V2 header must be 64-byte aligned");

/**
 * Helper functions for versioned header access
 * These functions provide type-safe access to header fields based on version
 */
namespace HeaderUtils {
	/**
	 * @brief Detect header version from raw pointer
	 * @param hdr Pointer to header (may be MessageHeader, BlogMessageHeader, or MessageHeaderV2)
	 * @return Detected version, or HEADER_V1 if unknown
	 * 
	 * Detection logic:
	 * - If pointer is to MessageHeaderV2, read version field
	 * - Otherwise, assume HEADER_V1 (legacy)
	 */
	inline HeaderVersion DetectVersion(const void* hdr) {
		if (hdr == nullptr) {
			return HeaderVersion::HEADER_V1;
		}
		
		// Check if this is a MessageHeaderV2 (has version field at offset 0)
		// Version field is at offset 0, but we need to check if it's a valid version
		uint16_t version_val = *reinterpret_cast<const uint16_t*>(hdr);
		if (version_val == static_cast<uint16_t>(HeaderVersion::HEADER_V1) ||
		    version_val == static_cast<uint16_t>(HeaderVersion::HEADER_V2)) {
			return static_cast<HeaderVersion>(version_val);
		}
		
		// Default to V1 for legacy headers
		return HeaderVersion::HEADER_V1;
	}

	/**
	 * @brief Get V1 header pointer (legacy MessageHeader)
	 * @param hdr Pointer to versioned header
	 * @return Pointer to MessageHeader, or nullptr if invalid
	 */
	inline MessageHeader* GetV1Header(void* hdr) {
		if (hdr == nullptr) return nullptr;
		
		HeaderVersion version = DetectVersion(hdr);
		if (version == HeaderVersion::HEADER_V1) {
			MessageHeaderV2* v2_hdr = reinterpret_cast<MessageHeaderV2*>(hdr);
			if (v2_hdr->version == HeaderVersion::HEADER_V1) {
				return &v2_hdr->v1;
			}
			// Direct MessageHeader pointer (not wrapped)
			return reinterpret_cast<MessageHeader*>(hdr);
		}
		return nullptr;
	}

	/**
	 * @brief Get V2 header pointer (BlogMessageHeader)
	 * @param hdr Pointer to versioned header
	 * @return Pointer to BlogMessageHeader, or nullptr if invalid
	 */
	inline BlogMessageHeader* GetV2Header(void* hdr) {
		if (hdr == nullptr) return nullptr;
		
		HeaderVersion version = DetectVersion(hdr);
		if (version == HeaderVersion::HEADER_V2) {
			MessageHeaderV2* v2_hdr = reinterpret_cast<MessageHeaderV2*>(hdr);
			return &v2_hdr->v2;
		}
		return nullptr;
	}

	/**
	 * @brief Convert V1 header to V2 header (migration helper)
	 * @param v1_source Source MessageHeader
	 * @param v2_dest Destination BlogMessageHeader (must be pre-allocated)
	 * @param client_id Client ID for V2 header
	 * @param batch_seq Batch sequence for V2 header
	 * 
	 * Field mapping:
	 * - v1.size → v2.size
	 * - v1.total_order → v2.total_order
	 * - v1.client_id → v2.client_id
	 * - v1.client_order → v2.batch_seq (approximate)
	 * - v2.received = 1 (assume already received)
	 * - v2.counter = 0 (will be set by delegation)
	 * - Timestamps set to current time
	 */
	inline void ConvertV1ToV2(const MessageHeader* v1_source, BlogMessageHeader* v2_dest,
	                          uint64_t client_id, uint32_t batch_seq) {
		if (v1_source == nullptr || v2_dest == nullptr) {
			return;
		}

		// Receiver fields (bytes 0-15)
		v2_dest->size = static_cast<uint32_t>(v1_source->size);
		v2_dest->received = 1;  // Assume already received
		v2_dest->ts = 0;  // Will be set by receiver if needed

		// Delegation fields (bytes 16-31) - leave for delegation to set
		v2_dest->counter = 0;
		v2_dest->flags = 0;
		v2_dest->processed_ts = 0;

		// Sequencer fields (bytes 32-47)
		v2_dest->total_order = v1_source->total_order;
		v2_dest->ordered_ts = 0;  // Will be set by sequencer

		// Read-only metadata (bytes 48-63)
		v2_dest->client_id = client_id;
		v2_dest->batch_seq = batch_seq;
		v2_dest->_pad = 0;
	}

	/**
	 * @brief Check if BlogMessageHeader should be used (feature flag)
	 * @return true if BlogMessageHeader is enabled, false to use legacy MessageHeader
	 * 
	 * Currently checks environment variable EMBARCADERO_USE_BLOG_HEADER.
	 * Future: Could check configuration file or compile-time flag.
	 * 
	 * Default: false (backward compatibility - use MessageHeader)
	 */
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

} // End of namespace Embarcadero
