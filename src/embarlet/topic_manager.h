#ifndef INCLUDE_TOPIC_MANGER_H_
#define INCLUDE_TOPIC_MANGER_H_

#include "../cxl_manager/cxl_manager.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/btree_set.h"
#include "absl/synchronization/mutex.h"
#include <glog/logging.h>

#include <bits/stdc++.h>

#define CACHELINE_SIZE 64
namespace Embarcadero{

class CXLManager;

using GetNewSegmentCallback = std::function<void*()>;

class Topic{
	public:
		Topic(GetNewSegmentCallback get_new_segment_callback, 
				void* TInode_addr, const char* topic_name, int broker_id, int order,
				void* cxl_addr, void* segment_metadata);
		~Topic(){
			stop_threads_ = true;
			for(std::thread& thread : combiningThreads_){
				if(thread.joinable()){
					thread.join();
				}
			}
			LOG(INFO) << "[Topic]: \tDestructed";
		}
		// Delete copy contstructor and copy assignment operator
		Topic(const Topic &) = delete;
		Topic& operator=(const Topic &) = delete;

		
		void PublishToCXL(PublishRequest &req);
		bool GetMessageAddr(size_t &last_offset,
							void* &last_addr, void* messages, size_t &messages_size);
		void Combiner();

	private:
		void CombinerThread();
		const GetNewSegmentCallback get_new_segment_callback_;
		struct TInode *tinode_;
		std::string topic_name_;
		int broker_id_;
		struct MessageHeader *last_message_header_;
		int order_;
		void* cxl_addr_;
		
		size_t logical_offset_;
		size_t written_logical_offset_;
		void* written_physical_addr_;
		std::atomic<unsigned long long int> log_addr_;
		//TODO(Jae) set this to nullptr if the sement is GCed
		void* first_message_addr_;

		//TInode cache
		void* ordered_offset_addr_;
		void* current_segment_;
		size_t ordered_offset_;
		bool stop_threads_ = false;

		std::vector<std::thread> combiningThreads_;
};

class TopicManager{
	public:
		TopicManager(CXLManager &cxl_manager, int broker_id):
									cxl_manager_(cxl_manager),
									broker_id_(broker_id),
									num_topics_(0){
			LOG(INFO) << "[TopicManager]\tConstructed";
		}
		~TopicManager(){
			LOG(INFO) << "[TopicManager]\tDestructed";
		}
		bool CreateNewTopic(char topic[TOPIC_NAME_SIZE], int order);
		void DeleteTopic(char topic[TOPIC_NAME_SIZE]);
		void PublishToCXL(PublishRequest &req);
		bool GetMessageAddr(const char* topic, size_t &last_offset,
												void* &last_addr, void* messages, size_t &messages_size);

	private:
		struct TInode* CreateNewTopicInternal(char topic[TOPIC_NAME_SIZE]);
		int GetTopicIdx(char topic[TOPIC_NAME_SIZE]){
			return topic_to_idx_(topic) % MAX_TOPIC_SIZE;
		}

		inline bool IsHeadNode(){
			return broker_id_ == 0;
		}

		CXLManager &cxl_manager_;
		static const std::hash<std::string> topic_to_idx_;
		absl::flat_hash_map<std::string, std::unique_ptr<Topic> > topics_;
		absl::Mutex mutex_;
		int broker_id_;
		size_t num_topics_;
};

} // End of namespace Embarcadero
#endif
