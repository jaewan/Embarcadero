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
	uint32_t client_id;
	volatile uint32_t complete;
	size_t size;
};

} // End of namespace Embarcadero
