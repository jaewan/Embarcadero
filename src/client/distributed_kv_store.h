#ifndef DISTRIBUTED_KV_STORE_H_
#define DISTRIBUTED_KV_STORE_H_

#include "absl/synchronization/mutex.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/flat_hash_map.h"

#include "common.h"
#include "publisher.h"
#include "subscriber.h"

#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>

// Unique identifier for operations
struct OperationId {
	size_t clientId; // Use message header's client_id
	size_t requestId;// Use message header's client_order

	bool operator==(const OperationId& other) const;
};

using OPID = size_t;

// Custom hash function for OperationId
namespace std {
	template<>
		struct hash<OperationId> {
			size_t operator()(const OperationId& id) const;
		};
}

// Operation types
enum class OpType {
	PUT,
	DELETE,
	MULTI_PUT,
	MULTI_GET,
	BEGIN_TX,
	COMMIT_TX,
	ABORT_TX
};

// Structure for a key-value pair
struct KeyValue {
	std::string key;
	std::string value;
};

// Structure for log entries
struct LogEntry {
	OperationId opId; // This is already in message header.
	OpType type;
	std::vector<KeyValue> kvPairs;  // Multiple pairs for multi-operations
	uint64_t transactionId;  // 0 if not part of a transaction

	// Serialize the log entry to a byte array
	std::vector<char> serialize() const;

	// Deserialize from a byte array
	static LogEntry deserialize(const void* data, size_t client_id, size_t client_order);
};

// Transaction state
struct Transaction {
	std::vector<KeyValue> writes;  // Pending writes
	absl::flat_hash_map<std::string, bool> readSet;  // Keys read
	absl::flat_hash_map<std::string, std::string> writeSet;  // Keys written
};

class DistributedKVStore {
	private:
		// Last request ID
		std::atomic<uint64_t> last_request_id_;

		// Index tracking - where in the log we've processed up to
		std::atomic<uint64_t> last_applied_total_order_;
		absl::Mutex apply_mutex_;

		// Thread pool for handling read operations
		//ThreadPool thread_pool_;

		// Server ID which should be the client_id from Embarcadero
		uint64_t server_id_;


		// Local key-value store
		absl::flat_hash_map<std::string, std::string> kv_store_;
		std::shared_mutex kv_store_mutex_;  // Read-write lock for better read concurrency

		// Pending write operations waiting for results
		absl::Mutex pending_ops_mutex_;
		absl::flat_hash_set<OPID> pending_ops_ ABSL_GUARDED_BY(pending_ops_mutex_);

		// Active transactions
		absl::Mutex transactions_mutex_;
		absl::flat_hash_map<uint64_t, Transaction> transactions_ ABSL_GUARDED_BY(transactions_mutex_);
		std::atomic<uint64_t> last_transaction_id_;

		// Log consumer thread
		std::vector<std::thread> log_consumer_threads_;
		std::atomic<bool> running_;

		std::unique_ptr<HeartBeat::Stub> stub_;
		std::unique_ptr<Publisher> publisher_;
		std::unique_ptr<Subscriber> subscriber_;

		// Process a log entry against the local state
		void processLogEntry(const LogEntry& entry, uint64_t logPosition);
		void processLogEntryFromRawBuffer(const void* data, size_t size,
				uint32_t client_id, size_t client_order,
				size_t total_order);

		// Complete a pending operation
		void completeOperation(OPID opId);

		// Consumer thread function to process log entries
		void logConsumer(int fd, std::shared_ptr<ConnectionBuffers> conn_buffers);

		// Wait until the local state has applied up to at least the given log position
		void waitUntilApplied(uint64_t position);

	public:
		// Constructor - now initializes the thread pool with a configurable number of threads
		explicit DistributedKVStore(uint64_t id, SequencerType seq_type, size_t numThreads = 8);

		// Destructor
		~DistributedKVStore();

		// Put a value for a key (through the log). Return client_order from MessageHeader
		size_t put(const std::string& key, const std::string& value);

		// Delete a key (through the log)
		//std::future<OperationResult> remove(const std::string& key);

		// Multi-key put operation (through the log)
		size_t multiPut(const std::vector<KeyValue>& kvPairs);

		// Get the current state of the key-value store (for debugging)
		std::unordered_map<std::string, std::string> getState();

		// Get the last applied index
		uint64_t getLastAppliedIndex() const;

		bool opFinished(OPID opId){
			return last_applied_total_order_ >= opId;
		}
};

#endif  // DISTRIBUTED_KV_STORE_H_
