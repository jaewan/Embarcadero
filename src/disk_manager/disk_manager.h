#ifndef INCLUDE_DISK_MANGER_H_
#define INCLUDE_DISK_MANGER_H_

#include <filesystem>
#include <thread>
#include <vector>
#include <optional>
#include <mutex>
#include <chrono>
#include "folly/MPMCQueue.h"
#include "common/config.h"

// Forward Declarations
namespace Corfu{
	class CorfuReplicationManager;
}
namespace Scalog{
	class ScalogReplicationManager;
}
namespace Embarcadero {
	class ChainReplicationManager;
}

// Forward declarations for CXL data structures
namespace Embarcadero {
	struct BatchHeader;
	struct TInode;
}

namespace Embarcadero{

namespace fs = std::filesystem;

struct ReplicationRequest{
	TInode* tinode;
	TInode* replica_tinode;
	int fd;
	int broker_id;
};

struct MemcpyRequest{
    void* addr;
    void* buf;
    size_t len;
		int fd;
		size_t offset;
};

class DiskManager{
	public:
		DiskManager(int broker_id, void* cxl_manager, bool log_to_memory,
							heartbeat_system::SequencerType sequencerType, size_t queueCapacity = 64);
		~DiskManager();
		// Current Implementation strictly requires the active brokers to be MAX_BROKER_NUM
		// Change this to get real-time num brokers
		void Replicate(TInode* TInode_addr, TInode* replica_tinode, int replication_factor);
		void StartScalogReplicaLocalSequencer();

	private:
		void ReplicateThread();
		void CopyThread();
		bool GetMessageAddr(TInode* tinode, int order, int broker_id, size_t &last_offset,
			void* &last_addr, void* &messages, size_t &messages_size);
		
		// [[EXPLICIT_REPLICATION_STAGE4]] - Batch-based replication for EMBARCADERO sequencers
		// Polls ordered batches from BatchHeader ring instead of message-based cursor
		bool GetNextReplicationBatch(TInode* tinode, int broker_id, 
			BatchHeader* &batch_ring_start, BatchHeader* &batch_ring_end,
			BatchHeader* &current_batch, size_t &disk_offset,
			void* &batch_payload, size_t &batch_payload_size, 
			size_t &batch_start_logical_offset, size_t &batch_last_logical_offset);

		std::vector<std::thread> threads_;
		folly::MPMCQueue<std::optional<struct ReplicationRequest>> requestQueue_;
		folly::MPMCQueue<std::optional<MemcpyRequest>> copyQueue_;
		int broker_id_;
		void* cxl_addr_;
		bool log_to_memory_;
		heartbeat_system::SequencerType sequencerType_;

		std::unique_ptr<Corfu::CorfuReplicationManager> corfu_replication_manager_;
		std::unique_ptr<Scalog::ScalogReplicationManager> scalog_replication_manager_;
		std::unique_ptr<Embarcadero::ChainReplicationManager> chain_replication_manager_;

		std::atomic<int> offset_{0};
		bool stop_threads_ = false;
		std::atomic<size_t> thread_count_{0};
		std::atomic<size_t> num_io_threads_{0};
		std::atomic<size_t> num_active_threads_{0};
		fs::path prefix_path_;
		
		// [[OBSERVABILITY]] - Replication metrics for monitoring and debugging
		// Per-broker counters (indexed by broker_id)
		struct ReplicationMetrics {
			std::atomic<uint64_t> batches_scanned{0};      // Total batches scanned in GetNextReplicationBatch
			std::atomic<uint64_t> batches_replicated{0};   // Total batches successfully replicated
			std::atomic<uint64_t> pwrite_retries{0};       // Total pwrite retries (EINTR/EAGAIN)
			std::atomic<uint64_t> pwrite_errors{0};        // Total permanent pwrite errors
			std::atomic<uint64_t> last_replication_done{0}; // Last replication_done value written
			std::chrono::steady_clock::time_point last_advance_time; // When replication_done last advanced
			std::mutex metrics_mutex; // Protects last_advance_time (non-atomic)
		};
		ReplicationMetrics replication_metrics_[NUM_MAX_BROKERS];
};

} // End of namespace Embarcadero
#endif
