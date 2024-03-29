#ifndef INCLUDE_DISK_MANGER_H_
#define INCLUDE_DISK_MANGER_H_

#include "common/config.h"
#include <atomic>
#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"

namespace Embarcadero{

struct PublishRequest{
	int client_id;
	int request_id;
	char topic[32];
	bool acknowledge;
	std::atomic<int> *counter;
	void* payload_address;
	size_t size;
};

class DiskManager{
	public:
		DiskManager(size_t queueCapacity);
		~DiskManager();
		void EnqueueRequest(struct PublishRequest);

	private:
		void Disk_io_thread();

		std::vector<std::thread> threads_;
		folly::MPMCQueue<std::optional<struct PublishRequest>> requestQueue_;

		int log_fd_;
		std::atomic<int> offset_{0};
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};
};

} // End of namespace Embarcadero
#endif
