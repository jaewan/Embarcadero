#pragma once

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
		 * @param message Message data
		 * @param len Message length
		 */
		void Publish(char* message, size_t len);

		/**
		 * Polls until n messages have been published
		 * @param n Number of messages to wait for
		 */
		void Poll(size_t n);

		/**
		 * Debug method to check if sending is finished
		 */
		void DEBUG_check_send_finish();

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

		void Flush();

		/**
		 * Signals that writing is finished
		 */
		void WriteFinished();

	private:
		std::string head_addr_;
		std::string port_;
		int client_id_;
		size_t num_threads_per_broker_;
		std::atomic<int> num_threads_{0};
		size_t message_size_;
		size_t queueSize_;
		Buffer pubQue_;
		SequencerType seq_type_;
		std::unique_ptr<CorfuSequencerClient> corfu_client_;

		bool shutdown_{false};
		bool publish_finished_{false};
		bool connected_{false};
		size_t client_order_ = 0;

		// Used to measure real-time throughput during failure benchmark
		std::atomic<size_t> total_sent_bytes_{0};
		std::vector<std::atomic<size_t>> sent_bytes_per_broker_;
		bool measure_real_time_throughput_ = false;
		std::thread real_time_throughput_measure_thread_;
		std::thread kill_brokers_thread_;
		bool kill_brokers_ = false;
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
		char topic_[TOPIC_NAME_SIZE];

		// Acknowledgement
		int ack_level_;
		int ack_port_;
		size_t ack_received_;
		std::vector<size_t> acked_messages_per_broker_;
		std::vector<std::thread> threads_;
		std::thread ack_thread_;
		std::atomic<int> thread_count_{0};

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
		 * @return true if successful, false otherwise
		 */
		bool AddPublisherThreads(size_t num_threads, int broker_id);
};
