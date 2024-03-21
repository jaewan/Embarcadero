#ifndef INCLUDE_TOPIC_MANGER_H_
#define INCLUDE_TOPIC_MANGER_H_

#include "cxl_manager.h"

#include <bits/stdc++.h>
#include <queue>

#define SKIP_SIZE 4
#define MAX_TOPIC_SIZE 4
#define SEGMENT_SIZE (1UL<<30)

namespace Embarcadero{

class CXLManager;

class Topic{
	public:
		Topic(CXLManager &cxl_manager, char topic_name[32], int broker_id);
		void PublishToCXL(void* message, size_t size);

	private:
		CXLManager &cxl_manager_;
		char* topic_name_;
		int broker_id_;
		struct TInode *tinode_;
		
		//TInode cache
		int logical_offset_;
		int written_logical_offset_;
		size_t remaining_size_;
		void* log_addr_;
		std::set<int> writing_offsets_;
		std::priority_queue<int, std::vector<int>, std::greater<int>> not_contigous_;

		//absl::mutex mu_;
		std::mutex mu_;
};

class TopicManager{
	public:
		void CreateNewTopic(char topic[32]);
		void DeleteTopic(char topic[32]);
		void PublishToCXL(char topic[32], void* message, size_t size);

	private:
		int GetTopicIdx(char topic[32]){
			return topic_to_idx_(topic) % MAX_TOPIC_SIZE;
		}

		static const std::hash<std::string> topic_to_idx_;
		std::map<std::string, Topic> topics_;
		//absl::flat_hash_set<std::string, Topic> topics_;
};

} // End of namespace Embarcadero
#endif
