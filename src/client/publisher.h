#pragma once

#include <atomic>
#include "absl/container/flat_hash_set.h"
#include "common.h"
#include "buffer.h"

/**
 * Publisher class for publishing messages to the messaging system
 */
class Publisher {
	public:
		/**
		 * Constructor for Publisher
		 * @param topic Topic name
		 * @param head_addr Head broker address
		 * @param port Port
		 * @param num_threads_per_broker Number of threads per broker
		 * @param message_size Size of messages
		 * @param queueSize Queue size
		 * @param order Order level
		 * @param seq_type Sequencer type
		 */
		Publisher(char topic[TOPIC_NAME_SIZE], std::string head_addr, std::string port, 
				int num_threads_per_broker, size_t message_size, size_t queueSize, 
				int order, SequencerType seq_type = heartbeat_system::SequencerType::EMBARCADERO);

		/**
		 * Destructor - cleans up resources
		 */
		~Publisher();


		/**
		 * Initializes the publisher
		 * @param ack_level Acknowledgement level
		 */
		void Init(int ack_level);

		/**
		 * Publishes a message
		 * @param message Message data (not owned; must remain valid until Poll() completes)
		 * @param len Message length
		 * @threading Call from application thread; client_order_ is atomic for concurrent Publish if needed
		 */
		void Publish(char* message, size_t len);

		/**
		 * Polls until n messages have been published and (if ack_level>=1) acknowledged.
		 * @param n Number of messages to wait for
		 * @return true if all messages sent and ACKs received, false on timeout or error
		 * @threading Call from same thread as Publish(); joins publisher threads on first successful call
		 */
		bool Poll(size_t n);

		/**
		 * Debug method to check if sending is finished
		 */
		void DEBUG_check_send_finish();

		/**
		 * PERF OPTIMIZATION: Pre-touch all allocated hugepage buffers to reduce variance
		 * This ensures all virtual addresses are populated and hugepages are committed
		 * Should be called after Init() but before performance measurement starts
		 */
		void WarmupBuffers();

		/**
		 * Simulates broker failures during operation
		 * @param total_message_size Total size of all messages
		 * @param failure_percentage Percentage of messages after which to fail
		 * @param killbrokers Function to kill brokers
		 */
		void FailBrokers(size_t total_message_size, size_t message_size,
				double failure_percentage, std::function<bool()> killbrokers);

		//********* Fail Broker Record Functions
		// Call this *before* starting threads that need the common time
		void RecordStartTime() {
			start_time_ = std::chrono::steady_clock::now();
			// Clear previous events if reusing the Publisher instance
			{
				absl::MutexLock lock(&event_mutex_);
				failure_events_.clear();
			}
		}

		// Call this *after* test run / joining threads
		void WriteFailureEventsToFile(const std::string& filename) {
			std::ofstream outfile(filename);
			if (!outfile.is_open()) {
				LOG(ERROR) << "Failed to open failure event log file: " << filename;
				return;
			}
			// Write header
			outfile << "Timestamp(ms),EventDescription\n";
			{
				absl::MutexLock lock(&event_mutex_);
				// Sort events by timestamp for clarity in the log/plot
				std::sort(failure_events_.begin(), failure_events_.end());
				for (const auto& event : failure_events_) {
					// Basic CSV quoting for description
					outfile << event.first << ",\"" << event.second << "\"\n";
				}
			}
			outfile.close();
		}

		int GetClientId(){
			return client_id_;
		}

		/**
		 * Signals that writing is finished
		 */
		void WriteFinishedOrPuased();

	private:
		std::string head_addr_;
		std::string port_;
		int client_id_;
		size_t num_threads_per_broker_;
		std::atomic<int> num_threads_{0};
		size_t message_size_;
		size_t queueSize_;  // [[GUARDED_BY: mutex_]] when modified in SubscribeToClusterStatus
		Buffer pubQue_;
		SequencerType seq_type_;
		std::unique_ptr<CorfuSequencerClient> corfu_client_;

		// [[Atomic - written by destructor/Poll/DEBUG_check, read by PublishThread/SubscribeToCluster]]
		std::atomic<bool> shutdown_{false};
		std::atomic<bool> publish_finished_{false};
		std::atomic<bool> connected_{false};
		// [[Atomic - Publish() increments, Poll() reads; ensures visibility across call sites]]
		std::atomic<size_t> client_order_{0};
		// [[DIAGNOSTIC]] Batch stats (PublishThread writes, WaitForAcks logs on timeout)
		std::atomic<size_t> total_batches_sent_{0};
		std::atomic<size_t> total_batches_attempted_{0};
		std::atomic<size_t> total_batches_failed_{0};

		// Used to measure real-time throughput during failure benchmark
		std::atomic<size_t> total_sent_bytes_{0};
		std::vector<std::atomic<size_t>> sent_bytes_per_broker_;
		bool measure_real_time_throughput_ = false;
		std::thread real_time_throughput_measure_thread_;
		std::thread kill_brokers_thread_;
		std::atomic<bool> kill_brokers_{false};  // FailBrokers writes, Poll reads; atomic if they run on different threads
		std::chrono::steady_clock::time_point start_time_;
		

		absl::Mutex event_mutex_;
		std::vector<std::pair<long long, std::string>> failure_events_ ABSL_GUARDED_BY(event_mutex_);

		// Helper to record an event with timestamp relative to start_time_
		void RecordFailureEvent(const std::string& description) {
			// Ensure start_time_ is initialized before calling this
			if (start_time_ == std::chrono::steady_clock::time_point{}) {
				LOG(ERROR) << "RecordFailureEvent called before RecordStartTime!";
				return; // Or initialize start_time_ here if needed, though less accurate
			}
			auto now = std::chrono::steady_clock::now();
			auto duration = now - start_time_;
			long long timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
			{
				absl::MutexLock lock(&event_mutex_);
				failure_events_.emplace_back(timestamp_ms, description);
			}
			// Also log immediately for real-time info
			LOG(WARNING) << "Failure/Event @ " << timestamp_ms << " ms: " << description;
		}


		// Context for cluster probe
		grpc::ClientContext context_;
		std::unique_ptr<HeartBeat::Stub> stub_;
		std::thread cluster_probe_thread_;

		// Broker management
		absl::flat_hash_map<int, std::string> nodes_;
		absl::Mutex mutex_;
		std::vector<int> brokers_;
		// [[FIX: B2=0 ACKs]] Track which brokers have publisher threads to handle late registration
		absl::flat_hash_set<int> brokers_with_threads_ ABSL_GUARDED_BY(mutex_);
		char topic_[TOPIC_NAME_SIZE];

		// Acknowledgement
		int ack_level_;
		int ack_port_;
		// [[threading: EpollAckThread writes, Poll() reads]] — must be atomic for correctness
		std::atomic<size_t> ack_received_{0};
		std::vector<std::atomic<size_t>> acked_messages_per_broker_;
		std::vector<std::thread> threads_;
		std::thread ack_thread_;
		std::atomic<int> thread_count_{0};
		std::atomic<bool> threads_joined_{false};
		// [[FIX: B3=0 ACKs]] Track which brokers have established ACK connections
		// This allows us to detect when a broker's ACK connection is missing
		absl::flat_hash_set<int> brokers_with_ack_connection_ ABSL_GUARDED_BY(mutex_);
		std::atomic<int> expected_ack_brokers_{0};

		/**
		 * Thread for handling acknowledgements using epoll
		 */
		void EpollAckThread();

		/**
		 * Thread for handling acknowledgements
		 */
		void AckThread();

		/**
		 * Thread for publishing messages
		 * @param broker_id Broker ID
		 * @param pubQuesIdx Queue index
		 */
		void PublishThread(int broker_id, int pubQuesIdx);

		/**
		 * Subscribes to cluster status updates
		 */
		void SubscribeToClusterStatus();

		/**
		 * Polls cluster status periodically
		 */
		void ClusterProbeLoop();

		/**
		 * Adds publisher threads
		 * @param num_threads Number of threads to add
		 * @param broker_id Broker ID
		 * @param queue_size Queue size to use (caller must pass consistent value, e.g. under mutex)
		 * @return true if successful, false otherwise
		 */
		bool AddPublisherThreads(size_t num_threads, int broker_id, size_t queue_size);

		// Instance vars for SubscribeToClusterStatus error handling (was static)
		std::chrono::steady_clock::time_point last_read_warning_;
		size_t read_fail_count_{0};
};
