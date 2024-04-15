#ifndef INCLUDE_DISK_MANGER_H_
#define INCLUDE_DISK_MANGER_H_

#include <thread>
#include <vector>
#include <optional>
#include "folly/MPMCQueue.h"
#include "common/config.h"
#include "../embarlet/req_queue.h"
#include "../embarlet/ack_queue.h"

namespace Embarcadero{

class DiskManager{
	public:
		DiskManager(std::shared_ptr<AckQueue> ack_queue, std::shared_ptr<ReqQueue> req_queue, int num_io_threads=NUM_DISK_IO_THREADS);
		~DiskManager();

	private:
		void DiskIOThread();

		std::vector<std::thread> threads_;
		std::shared_ptr<ReqQueue> reqQueue_;
		std::shared_ptr<AckQueue> ackQueue_;

		int log_fd_;
		std::atomic<int> offset_{0};
		bool stop_threads_ = false;
		std::atomic<int> thread_count_{0};
		int num_io_threads_;
};

} // End of namespace Embarcadero
#endif
