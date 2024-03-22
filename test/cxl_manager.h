#ifndef INCLUDE_CXL_MANGER_H_
#define INCLUDE_CXL_MANGER_H_

#include "topic_manager.h"

#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace Embarcadero{

#define NUM_BROKERS 4

class TopicManager;

enum CXL_Type {Emul, Real};

struct publish_request{
	int client_id;
	int request_id;
	char topic[32];
	bool acknowledge;
	std::atomic<int> *counter;
	void* payload_address;
	size_t size;
};

struct TInode{
	char topic[32];
	// Align and pad to 64B cacheline
	struct alignas(64) offset_entry {
    size_t ordered;
    size_t written;
    void* log_addr;
    char _padding[64 - (sizeof(size_t) * 2 + sizeof(void*))]; 
	};
	offset_entry offsets[NUM_BROKERS];
};

struct MessageHeader{
	void* skip_idx[4];
	int order;
};

class CXLManager{
	public:
		CXLManager(int broker_id);
		~CXLManager();
		void* Get_tinode(const char* topic, int broker_num);
		void SetTopicManager(TopicManager *topic_manager){
			topic_manager_ = topic_manager;
		}
		void EnqueueRequest(struct publish_request &request){
			std::unique_lock<std::mutex> lock(queueMutex_);
			requestQueue_.push(request);
			queueCondVar_.notify_one(); 
		}
		void* GetNewSegment();
		void* GetTInode(const char* topic, int broker_num);

	private:
		int broker_id_;
		//TODO(Erika) Replace this queue, mutex, and condition variable with folly MPMC.
		// We may not even want this thread model and rely on folly IOThreadPoolExecutor
		std::queue<publish_request> requestQueue_;
		std::mutex queueMutex_;
		std::condition_variable queueCondVar_;
		std::vector<std::thread> threads_;

		TopicManager *topic_manager_;
		CXL_Type cxl_type_;
		int cxl_emul_fd_;
		void* cxl_addr_;
		void* bitmap_;
		void* segments_;
		void* current_log_addr_;
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};

		void CXL_io_thread();
};

} // End of namespace Embarcadero
#endif
