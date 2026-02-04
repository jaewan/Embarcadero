#pragma once

// Standard library includes
#include <bits/stdc++.h>

// External library includes
#include <absl/container/flat_hash_map.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <absl/container/btree_set.h>

#include "common/config.h"
#include "common/performance_utils.h"
#include "common/fine_grained_lock.h"
#include "topic.h"
#include "cxl_manager/cxl_manager.h"
#include "disk_manager/disk_manager.h"

namespace Embarcadero {

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

// Forward declarations
class CXLManager;
class DiskManager;
//class Topic;

/**
 * Class for managing multiple topics
 */
class TopicManager {
	public:
		/**
		 * Constructor
		 *
		 * @param cxl_manager Reference to CXL memory manager
		 * @param disk_manager Reference to disk storage manager
		 * @param broker_id ID of the broker
		 * @param is_sequencer_node If true, this node runs only the sequencer (no data path)
		 *        When is_sequencer_node && broker_id==0, skip allocating segment/batch header ring
		 */
		TopicManager(CXLManager& cxl_manager, DiskManager& disk_manager, int broker_id,
		             bool is_sequencer_node = false) :
			cxl_manager_(cxl_manager),
			disk_manager_(disk_manager),
			broker_id_(broker_id),
			is_sequencer_node_(is_sequencer_node),
			num_topics_(0) {
				VLOG(3) << "\t[TopicManager]\t\tConstructed (is_sequencer_node="
				        << is_sequencer_node_ << ")";
			}

		/**
		 * Destructor
		 */
		~TopicManager() {
			VLOG(3) << "\t[TopicManager]\tDestructed";
		}

		/**
		 * Create a new topic with specified parameters
		 *
		 * @param topic Topic name
		 * @param order Ordering level
		 * @param replication_factor Number of replicas
		 * @param replicate_tinode Whether to replicate the TInode
		 * @param seq_type Type of sequencer to use
		 * @return true if topic creation succeeded, false otherwise
		 */
		bool CreateNewTopic(
				const char topic[TOPIC_NAME_SIZE],
				int order,
				int replication_factor,
				bool replicate_tinode,
				int ack_level,
				heartbeat_system::SequencerType seq_type);

		/**
		 * Delete a topic
		 *
		 * @param topic Topic name to delete
		 */
		void DeleteTopic(const char topic[TOPIC_NAME_SIZE]);

		/**
		 * Get a buffer in CXL memory for a new batch of messages
		 *
		 * @param batch_header Reference to batch header
		 * @param topic Topic name
		 * @param log Reference to store log pointer
		 * @param segment_header Reference to store segment header
		 * @param logical_offset Reference to store logical offset
		 * @param seq_type Reference to store sequencer type
		 * @return Callback function to execute after writing to the buffer
		 */
		std::function<void(void*, size_t)> GetCXLBuffer(
				BatchHeader& batch_header,
				const char topic[TOPIC_NAME_SIZE],
				void*& log,
				void*& segment_header,
				size_t& logical_offset,
				heartbeat_system::SequencerType& seq_type,
				BatchHeader*& batch_header_location);

		// [[RECV_DIRECT_TO_CXL]] Split allocation for zero-copy receive path
		bool ReserveBLogSpace(const char* topic, size_t size, void*& log);
		bool IsPBRAboveHighWatermark(const char* topic, int high_pct);
		bool IsPBRBelowLowWatermark(const char* topic, int low_pct);
		bool ReservePBRSlotAndWriteEntry(const char* topic, BatchHeader& batch_header, void* log,
				void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location);
		bool ReservePBRSlotAfterRecv(const char* topic, BatchHeader& batch_header, void* log,
				void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location);
		// [[PERF]] Topic* overloads to avoid repeated topics_mutex_ lookup (use with cached Topic* from GetTopic)
		bool IsPBRAboveHighWatermark(Topic* topic_ptr, int high_pct);
		bool ReserveBLogSpace(Topic* topic_ptr, size_t size, void*& log, bool epoch_already_checked = false);
		bool ReservePBRSlotAndWriteEntry(Topic* topic_ptr, BatchHeader& batch_header, void* log,
				void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);
		bool ReservePBRSlotAfterRecv(Topic* topic_ptr, BatchHeader& batch_header, void* log,
				void*& segment_header, size_t& logical_offset, BatchHeader*& batch_header_location,
				bool epoch_already_checked = false);

		bool GetBatchToExport(
				const char* topic,
				size_t &expected_batch_offset,
				void* &batch_addr,
				size_t &batch_size);
		bool GetBatchToExportWithMetadata(
				const char* topic,
				size_t &expected_batch_offset,
				void* &batch_addr,
				size_t &batch_size,
				size_t &batch_total_order,
				uint32_t &num_messages);
		/**
		 * Get message address and size for a topic
		 *
		 * @param topic Topic name
		 * @param last_offset Reference to the last message offset seen
		 * @param last_addr Reference to the last message address seen
		 * @param messages Reference to store messages pointer
		 * @param messages_size Reference to store messages size
		 * @return true if new messages were found, false otherwise
		 */
		bool GetMessageAddr(
				const char* topic,
				size_t& last_offset,
				void*& last_addr,
				void*& messages,
				size_t& messages_size);

		int GetTopicOrder(const char* topic);

		void RegisterGetNumBrokersCallback(GetNumBrokersCallback callback){
			get_num_brokers_callback_ = callback;
		}

		void RegisterGetRegisteredBrokersCallback(GetRegisteredBrokersCallback callback){
			get_registered_brokers_callback_ = callback;
		}

		// Get a pointer to a topic object
		Topic* GetTopic(const std::string& topic_name);

	private:
		/**
		 * Internal implementation of topic creation
		 *
		 * @param topic Topic name
		 * @return Pointer to the created TInode or nullptr on failure
		 */
		struct TInode* CreateNewTopicInternal(const char topic[TOPIC_NAME_SIZE]);

		/**
		 * Internal implementation of topic creation with parameters
		 *
		 * @param topic Topic name
		 * @param order Ordering level
		 * @param replication_factor Number of replicas
		 * @param replicate_tinode Whether to replicate the TInode
		 * @param seq_type Type of sequencer to use
		 * @return Pointer to the created TInode or nullptr on failure
		 */
		struct TInode* CreateNewTopicInternal(
				const char topic[TOPIC_NAME_SIZE],
				int order,
				int replication_factor,
				bool replicate_tinode,
				int ack_level,
				heartbeat_system::SequencerType seq_type);

		/**
		 * Helper to initialize TInode offsets
		 */
		void InitializeTInodeOffsets(
				TInode* tinode,
				void* segment_metadata,
				void* batch_headers_region,
				void* cxl_addr);

		/**
		 * Get topic index from name
		 *
		 * @param topic Topic name
		 * @return Topic index
		 */
		int GetTopicIdx(const char topic[TOPIC_NAME_SIZE]) {
			return topic_to_idx_(topic) % MAX_TOPIC_SIZE;
		}

		/**
		 * Check if this is the head node
		 *
		 * @return true if this is the head node (broker_id == 0)
		 */
		inline bool IsHeadNode() const {
			return broker_id_ == 0;
		}

		// Core members
		CXLManager& cxl_manager_;
		DiskManager& disk_manager_;
		static const std::hash<std::string> topic_to_idx_;
		absl::flat_hash_map<std::string, std::unique_ptr<Topic>> topics_;

		// Replace single mutex with striped locking for better concurrency
		StripedLock<std::string, 128> topic_locks_;  // 128 stripes for fine-grained locking
		absl::Mutex topics_mutex_;  // Only for global operations like iteration

		int broker_id_;
		bool is_sequencer_node_;  // If true, skip B0 allocation (sequencer-only head node)
		size_t num_topics_;
		GetNumBrokersCallback get_num_brokers_callback_;
		GetRegisteredBrokersCallback get_registered_brokers_callback_;
}; // TopicManager

} // End of namespace Embarcadero
