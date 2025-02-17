#ifndef INCLUDE_TOPIC_MANGER_H_
#define INCLUDE_TOPIC_MANGER_H_

#include "../cxl_manager/cxl_manager.h"
#include "../disk_manager/disk_manager.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/btree_set.h"
#include "absl/synchronization/mutex.h"
#include <glog/logging.h>

#include <bits/stdc++.h>

#define CACHELINE_SIZE 64
namespace Embarcadero{

class CXLManager;
class DiskManager;

using GetNewSegmentCallback = std::function<void*()>;

class Topic{
	public:
		Topic(GetNewSegmentCallback get_new_segment_callback, 
				void* TInode_addr, TInode* replica_tinode, const char* topic_name, int broker_id, int order,
				heartbeat_system::SequencerType, void* cxl_addr, void* segment_metadata);
		~Topic(){
			stop_threads_ = true;
			for(std::thread& thread : combiningThreads_){
				if(thread.joinable()){
					thread.join();
				}
			}
			VLOG(3) << "[Topic]: \tDestructed";
		}
		// Delete copy contstructor and copy assignment operator
		Topic(const Topic &) = delete;
		Topic& operator=(const Topic &) = delete;

		bool GetMessageAddr(size_t &last_offset,
				void* &last_addr, void* &messages, size_t &messages_size);
		void Combiner();
		std::function<void(void*, size_t)> GetCXLBuffer(struct BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset){
			return (this->*GetCXLBufferFunc)(batch_header, topic, log, segment_header, logical_offset);
		}

	private:
		inline void UpdateTInodeWritten(size_t written, size_t written_addr);
		void CombinerThread();
		std::function<void(void*, size_t)>(Topic::*GetCXLBufferFunc)(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset);
		std::function<void(void*, size_t)> KafkaGetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset);
		std::function<void(void*, size_t)> CorfuGetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset);
		std::function<void(void*, size_t)> Order3GetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset);
		std::function<void(void*, size_t)> Order4GetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset);
		std::function<void(void*, size_t)> EmbarcaderoGetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset);
		const GetNewSegmentCallback get_new_segment_callback_;
		struct TInode *tinode_;
		struct TInode *replica_tinode_;
		std::string topic_name_;
		int broker_id_;
		struct MessageHeader *last_message_header_;
		int order_;
		heartbeat_system::SequencerType seq_type_;
		void* cxl_addr_;

		size_t logical_offset_;
		size_t written_logical_offset_;
		void* written_physical_addr_;
		std::atomic<unsigned long long int> log_addr_;
		unsigned long long int batch_headers_;
		//TODO(Jae) set this to nullptr if the sement is GCed
		void* first_message_addr_;
		void* first_batch_headers_addr_;
		absl::flat_hash_map<size_t, absl::flat_hash_map<size_t, void*>> skipped_batch_ ABSL_GUARDED_BY(mutex_);
		absl::flat_hash_map<size_t, size_t> order3_client_batch_ ABSL_GUARDED_BY(mutex_);
		absl::Mutex mutex_;
		absl::Mutex written_mutex_;
		std::atomic<size_t> kafka_logical_offset_{0};
		absl::flat_hash_map<size_t, size_t> written_messages_range_;

		//TInode cache
		void* ordered_offset_addr_;
		void* current_segment_;
		size_t ordered_offset_;
		bool stop_threads_ = false;

		std::vector<std::thread> combiningThreads_;
};

class TopicManager{
	public:
		TopicManager(CXLManager &cxl_manager, DiskManager &disk_manager, int broker_id):
			cxl_manager_(cxl_manager),
			disk_manager_(disk_manager),
			broker_id_(broker_id),
			num_topics_(0){
				VLOG(3) << "\t[TopicManager]\t\tConstructed";
			}
		~TopicManager(){
			VLOG(3) << "\t[TopicManager]\tDestructed";
		}
		bool CreateNewTopic(char topic[TOPIC_NAME_SIZE], int order, int replication_factor, bool replicate_tinode, heartbeat_system::SequencerType);
		void DeleteTopic(char topic[TOPIC_NAME_SIZE]);
		std::function<void(void*, size_t)> GetCXLBuffer(BatchHeader &batch_header, char topic[TOPIC_NAME_SIZE], void* &log, void* &segment_header, size_t &logical_offset);
		bool GetMessageAddr(const char* topic, size_t &last_offset,
				void* &last_addr, void* &messages, size_t &messages_size);

	private:
		struct TInode* CreateNewTopicInternal(char topic[TOPIC_NAME_SIZE]);
		struct TInode* CreateNewTopicInternal(char topic[TOPIC_NAME_SIZE], int order, int replication_factor, bool replicate_tinode, heartbeat_system::SequencerType);
		int GetTopicIdx(char topic[TOPIC_NAME_SIZE]){
			return topic_to_idx_(topic) % MAX_TOPIC_SIZE;
		}

		inline bool IsHeadNode(){
			return broker_id_ == 0;
		}

		CXLManager &cxl_manager_;
		DiskManager &disk_manager_;
		static const std::hash<std::string> topic_to_idx_;
		absl::flat_hash_map<std::string, std::unique_ptr<Topic> > topics_;
		absl::Mutex mutex_;
		int broker_id_;
		size_t num_topics_;
};

} // End of namespace Embarcadero
#endif
