#pragma once
#include "common/config.h"
#include <heartbeat.grpc.pb.h>

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

struct alignas(64) offset_entry {
	struct {
		volatile size_t log_offset;
		volatile size_t batch_headers_offset;
		volatile size_t written;
		volatile unsigned long long int written_addr;
		volatile int replication_done[NUM_MAX_BROKERS];
	}__attribute__((aligned(64)));
	struct {
		volatile int ordered;
		volatile size_t ordered_offset; //relative offset to last ordered message header
	}__attribute__((aligned(64)));
};

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

} // End of namespace Embarcadero
