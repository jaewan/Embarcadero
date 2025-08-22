#include "test_utils.h"
#include <chrono>
#include <iomanip>
#include <random>
#include <thread>
#include <fstream>
#include <numeric>
#include <algorithm>

// Helper function to generate random message content
void FillRandomData(char* buffer, size_t size) {
	static thread_local std::mt19937 gen(std::random_device{}());
	static thread_local std::uniform_int_distribution<char> dist(32, 126); // Printable ASCII chars

	for (size_t i = 0; i < size; i++) {
		buffer[i] = dist(gen);
	}
}

// Helper function to log test parameters
void LogTestParameters(const std::string& test_name, const cxxopts::ParseResult& result) {
	LOG(INFO) << "\n\n===== " << test_name << " ====="
		<< "\n\t  Message size: " << result["size"].as<size_t>() << " bytes"
		<< "\n\t  Total message size: " << result["total_message_size"].as<size_t>() << " bytes"
		<< "\n\t  Threads per broker: " << result["num_threads_per_broker"].as<size_t>()
		<< "\n\t  ACK level: " << result["ack_level"].as<int>()
		<< "\n\t  Order level: " << result["order_level"].as<int>()
		<< "\n\t  Sequencer: " << result["sequencer"].as<std::string>();
}

// Helper class to track and report test progress
class ProgressTracker {
	public:
		ProgressTracker(size_t total_operations, size_t log_interval = 5000)
			: total_ops_(total_operations), log_interval_(log_interval) {
				start_time_ = std::chrono::high_resolution_clock::now();
				last_log_time_ = start_time_;
			}

		void Update(size_t current_operations) {
			auto now = std::chrono::high_resolution_clock::now();
			auto elapsed_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time_).count();

			if (elapsed_since_last >= log_interval_ || current_operations >= total_ops_) {
				double progress_pct = (100.0 * current_operations) / total_ops_;
				auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

				// Calculate rate and ETA
				double rate = current_operations / (total_elapsed > 0 ? total_elapsed : 1);
				double eta = (total_ops_ - current_operations) / (rate > 0 ? rate : 1);

				LOG(INFO) << "Progress: " << std::fixed << std::setprecision(1) << progress_pct << "% "
					<< "(" << current_operations << "/" << total_ops_ << ") "
					<< "Rate: " << std::setprecision(2) << rate << " ops/sec, "
					<< "ETA: " << std::setprecision(0) << eta << " sec";

				last_log_time_ = now;
			}
		}

		double GetElapsedSeconds() const {
			auto now = std::chrono::high_resolution_clock::now();
			return std::chrono::duration<double>(now - start_time_).count();
		}

	private:
		size_t total_ops_;
		size_t log_interval_;
		std::chrono::high_resolution_clock::time_point start_time_;
		std::chrono::high_resolution_clock::time_point last_log_time_;
};

double FailurePublishThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE], 
		std::function<bool()> killbrokers) {
	LogTestParameters("Failure Publish Throughput Test", result);

	// Extract test parameters
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	double failure_percentage = result["failure_percentage"].as<double>();

	// Calculate number of messages
	size_t n = total_message_size / message_size;

	LOG(INFO) << "Starting failure publish throughput test with " << n << " messages"
		<< " (" << total_message_size << " bytes total)"
		<< ", failure at " << (failure_percentage * 100) << "% of data sent";

	// Allocate and prepare message buffer
	char* message = nullptr;
	try {
		message = new char[message_size];
		FillRandomData(message, message_size);
	} catch (const std::bad_alloc& e) {
		LOG(ERROR) << "Failed to allocate message buffer: " << e.what();
		return 0.0;
	}

	// Calculate queue size with buffer
	size_t q_size = total_message_size + (total_message_size / message_size) * 64 + 2097152;
	q_size = std::max(q_size, static_cast<size_t>(1024));

	// Create publisher
	Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
			num_threads_per_broker, message_size, q_size, order);

	try {
		p.RecordStartTime(); // For failure event timestamp across threads
												 // Initialize publisher
		p.Init(ack_level);

		// Set up broker failure simulation
		p.FailBrokers(total_message_size, message_size, failure_percentage, killbrokers);

		// Create progress tracker
		//ProgressTracker progress(n, 1000);

		// Start timing
		auto start = std::chrono::high_resolution_clock::now();

		// Publish messages
		for (size_t i = 0; i < n; i++) {
			p.Publish(message, message_size);

			/*
			// Update progress periodically
			if (i % 1000 == 0) {
			progress.Update(i);
			}
			*/
		}

		// Finalize publishing
		p.DEBUG_check_send_finish();
		p.Poll(n);

		p.WriteFailureEventsToFile("/home/domin/Embarcadero/data/failure/failure_events.csv");
		// Calculate elapsed time and bandwidth
		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> elapsed = end - start;
		double seconds = elapsed.count();
		double bandwidthMbps = ((message_size * n) / seconds) / (1024 * 1024);

		LOG(INFO) << "Failure publish test completed in " << std::fixed << std::setprecision(2) 
			<< seconds << " seconds";
		LOG(INFO) << "Bandwidth: " << std::fixed << std::setprecision(2) << bandwidthMbps << " MB/s";

		// Clean up
		delete[] message;
		return bandwidthMbps;

	} catch (const std::exception& e) {
		LOG(ERROR) << "Exception during failure publish test: " << e.what();
		delete[] message;
		return 0.0;
	}
}

double PublishThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE], 
		std::atomic<int>& synchronizer) {
	LogTestParameters("Publish Throughput Test", result);

	// Extract test parameters
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	int num_clients = result["parallel_client"].as<int>();
	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());

	// Adjust total message size for parallel clients
	total_message_size = total_message_size / num_clients;

	// Calculate number of messages
	size_t n = total_message_size / message_size;

	LOG(INFO) << "Starting publish throughput test with " << n << " messages"
		<< " (" << total_message_size << " bytes total)"
		<< ", client " << (num_clients - synchronizer.load() + 1) << " of " << num_clients;

	// Allocate and prepare message buffer
	char* message = nullptr;
	try {
		message = new char[message_size];
		FillRandomData(message, message_size);
	} catch (const std::bad_alloc& e) {
		LOG(ERROR) << "Failed to allocate message buffer: " << e.what();
		return 0.0;
	}

	// Calculate queue size with buffer
	size_t q_size = total_message_size + (total_message_size / message_size) * 64 + 2097152;
	q_size = std::max(q_size, static_cast<size_t>(1024));

	// Create publisher
	Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
			num_threads_per_broker, message_size, q_size, order, seq_type);

	try {
		// Initialize publisher
		p.Init(ack_level);

		// Synchronize with other clients
		synchronizer.fetch_sub(1);

		while (synchronizer.load() != 0) {
			std::this_thread::yield();
		}

		VLOG(5) << "All clients ready, starting publish test";

		// Start timing
		auto start = std::chrono::high_resolution_clock::now();

		// Publish messages
		for (size_t i = 0; i < n; i++) {
			p.Publish(message, message_size);
		}

		// Finalize publishing
		VLOG(5) << "Finished publishing from client";
		p.DEBUG_check_send_finish();
		p.Poll(n);

		// Calculate elapsed time and bandwidth
		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> elapsed = end - start;
		double seconds = elapsed.count();
		double bandwidthMbps = ((message_size * n) / seconds) / (1024 * 1024);

		LOG(INFO) << "Publish test completed in " << std::fixed << std::setprecision(2) 
			<< seconds << " seconds";
		LOG(INFO) << "Bandwidth: " << std::fixed << std::setprecision(2) << bandwidthMbps << " MB/s";

		// Clean up
		delete[] message;
		return bandwidthMbps;

	} catch (const std::exception& e) {
		LOG(ERROR) << "Exception during publish test: " << e.what();
		delete[] message;
		return 0.0;
	}
}

double SubscribeThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]) {
	LogTestParameters("Subscribe Throughput Test", result);

	// Extract test parameters
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	int order = result["order_level"].as<int>();

	LOG(INFO) << "Starting subscribe throughput test for " << total_message_size << " bytes of data";

	try {
		// Start timing
		auto start = std::chrono::high_resolution_clock::now();

		// Create subscriber
		Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic);

		// Track start of the actual receiving process
		auto receive_start = std::chrono::high_resolution_clock::now();

		// Wait for all messages to be received
		VLOG(5) << "Waiting to receive " << total_message_size << " bytes of data";

		// Wait for all data to be received
		s.Poll(total_message_size, message_size);

		// Calculate elapsed time and bandwidth
		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> elapsed = end - start;
		std::chrono::duration<double> receive_elapsed = end - receive_start;

		double seconds = elapsed.count();
		double receive_seconds = receive_elapsed.count();
		double bandwidthMbps = (total_message_size / (1024 * 1024)) / seconds;
		double receive_bandwidthMbps = (total_message_size / (1024 * 1024)) / receive_seconds;

		LOG(INFO) << "Subscribe test completed in " << std::fixed << std::setprecision(2) 
			<< seconds << " seconds (connection: " 
			<< std::setprecision(2) << (seconds - receive_seconds) 
			<< "s, receiving: " << std::setprecision(2) << receive_seconds << "s)";
		LOG(INFO) << "Bandwidth: " << std::fixed << std::setprecision(2) << bandwidthMbps 
			<< " MB/s (receiving only: " << receive_bandwidthMbps << " MB/s)";

		// Check message ordering if requested
		if (!s.DEBUG_check_order(order)) {
			LOG(ERROR) << "Order check failed for order level " << order;
		}

		return bandwidthMbps;

	} catch (const std::exception& e) {
		LOG(ERROR) << "Exception during subscribe test: " << e.what();
		return 0.0;
	}
}

double ConsumeThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]) {
	LogTestParameters("Consume Throughput Test", result);

	// Extract test parameters
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t n = total_message_size/message_size;
	int order = result["order_level"].as<int>();

	LOG(INFO) << "Starting consume throughput test for " << total_message_size << " bytes of data.\n"
		<< "This only works with " << NUM_MAX_BROKERS << " brokers";

	try {
		// Start timing
		auto start = std::chrono::high_resolution_clock::now();

		// Create subscriber
		Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic);
		s.WaitUntilAllConnected(); // Assume there exists NUM_MAX_BROKERS

		// Track start of the actual receiving process
		auto receive_start = std::chrono::high_resolution_clock::now();

		// Wait for all messages to be received
		VLOG(5) << "Waiting to receive " << total_message_size << " bytes of data";

		for(size_t i=0; i< n; i++){
			while(s.Consume() == nullptr){
				std::this_thread::yield();
			}
		}

		// Calculate elapsed time and bandwidth
		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> elapsed = end - start;
		std::chrono::duration<double> receive_elapsed = end - receive_start;


		double seconds = elapsed.count();
		double receive_seconds = receive_elapsed.count();
		double bandwidthMbps = (total_message_size / (1024 * 1024)) / seconds;
		double receive_bandwidthMbps = (total_message_size / (1024 * 1024)) / receive_seconds;

		LOG(INFO) << "Consume test completed in " << std::fixed << std::setprecision(2) 
			<< seconds << " seconds (connection: " 
			<< std::setprecision(2) << (seconds - receive_seconds) 
			<< "s, receiving: " << std::setprecision(2) << receive_seconds << "s)";
		LOG(INFO) << "Bandwidth: " << std::fixed << std::setprecision(2) << bandwidthMbps 
			<< " MB/s (receiving only: " << receive_bandwidthMbps << " MB/s)";

		// Check message ordering if requested
		if (!s.DEBUG_check_order(order)) {
			LOG(ERROR) << "Order check failed for order level " << order;
		}

		return bandwidthMbps;

	} catch (const std::exception& e) {
		LOG(ERROR) << "Exception during subscribe test: " << e.what();
		return 0.0;
	}
}

std::pair<double, double> E2EThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]) {
	LogTestParameters("End-to-End Throughput Test", result);

	// Extract test parameters
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());

	// Calculate number of messages
	size_t n = total_message_size / message_size;

	LOG(INFO) << "Starting end-to-end throughput test with " << n << " messages"
		<< " (" << total_message_size << " bytes total)";

	// Allocate and prepare message buffer
	char* message = nullptr;
	try {
		message = new char[message_size];
		FillRandomData(message, message_size);
	} catch (const std::bad_alloc& e) {
		LOG(ERROR) << "Failed to allocate message buffer: " << e.what();
		return std::make_pair(0.0, 0.0);
	}

	// Calculate queue size with buffer
	size_t q_size = total_message_size + (total_message_size / message_size) * 64 + 2097152;
	q_size = std::max(q_size, static_cast<size_t>(1024));

	try {
		// Create publisher and subscriber
		Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
				num_threads_per_broker, message_size, q_size, order, seq_type);
		Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, false);

		// Initialize publisher
		p.Init(ack_level);

		// Start timing for publishing
		auto start = std::chrono::high_resolution_clock::now();

		// Publish messages
		for (size_t i = 0; i < n; i++) {
			p.Publish(message, message_size);
		}

		// Finalize publishing
		p.DEBUG_check_send_finish();
		p.Poll(n);

		// Record publish end time
		auto pub_end = std::chrono::high_resolution_clock::now();

		// Wait for all messages to be received by subscriber
		LOG(INFO) << "Publishing complete, waiting for subscriber to receive all data...";
		s.Poll(total_message_size, message_size);

		// Record end-to-end end time
		auto end = std::chrono::high_resolution_clock::now();

		// Calculate publish bandwidth
		double pub_seconds = std::chrono::duration<double>(pub_end - start).count();
		double pubBandwidthMbps = ((message_size * n) / pub_seconds) / (1024 * 1024);

		// Calculate end-to-end bandwidth
		double e2e_seconds = std::chrono::duration<double>(end - start).count();
		double e2eBandwidthMbps = ((message_size * n) / e2e_seconds) / (1024 * 1024);

		// Check message ordering
		s.DEBUG_check_order(order);

		LOG(INFO) << "Publish completed in " << std::fixed << std::setprecision(2) 
			<< pub_seconds << " seconds, " << pubBandwidthMbps << " MB/s";
		LOG(INFO) << "End-to-end completed in " << std::fixed << std::setprecision(2) 
			<< e2e_seconds << " seconds, " << e2eBandwidthMbps << " MB/s";

		// Clean up
		delete[] message;
		return std::make_pair(pubBandwidthMbps, e2eBandwidthMbps);

	} catch (const std::exception& e) {
		LOG(ERROR) << "Exception during end-to-end test: " << e.what();
		delete[] message;
		return std::make_pair(0.0, 0.0);
	}
}

std::pair<double, double> LatencyTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]) {
	LogTestParameters("Latency Test", result);

	// Extract test parameters
	size_t message_size = result["size"].as<size_t>();
	size_t total_message_size = result["total_message_size"].as<size_t>();
	size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
	int ack_level = result["ack_level"].as<int>();
	int order = result["order_level"].as<int>();
	bool steady_rate = result.count("steady_rate");
	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());

	if (steady_rate) {
		LOG(WARNING) << "Using steady rate mode, this works best with 4 brokers";
	}

	// Calculate send interval for rate limiting
	size_t padded = message_size % 64;
	if (padded) {
		padded = 64 - padded;
	}

	size_t paddedMsgSizeWithHeader = message_size + padded + sizeof(Embarcadero::MessageHeader);

	// Calculate number of messages
	size_t n = total_message_size / message_size;

	LOG(INFO) << "Starting latency test with " << n << " messages"
		<< " (" << total_message_size << " bytes total)"
		<< (steady_rate ? ", using steady rate" : "");

	// Allocate message buffer
	char message[message_size];

	// Calculate queue size with buffer
	size_t q_size = total_message_size + (total_message_size / message_size) * 64 + 2097152;
	q_size = std::max(q_size, static_cast<size_t>(1024));

	try {
		// Create publisher and subscriber
		Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
				num_threads_per_broker, message_size, q_size, order, seq_type);
		Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, true);

		// Initialize publisher
		p.Init(ack_level);

		// Set up progress tracking
		//ProgressTracker progress(n, 1000);

		// Start timing
		auto start = std::chrono::high_resolution_clock::now();

		// Publish messages with timestamps
		size_t sent_bytes = 0;
		for (size_t i = 0; i < n; i++) {

			// If using steady rate, pause periodically
			if (steady_rate && (sent_bytes >= (BATCH_SIZE*4))) {
				p.WriteFinishedOrPuased();
				std::this_thread::sleep_for(std::chrono::microseconds(1500));
				sent_bytes = 0;
				// Capture current timestamp and embed it in the message
				auto timestamp = std::chrono::steady_clock::now();
				long long nanoseconds_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
						timestamp.time_since_epoch()).count();

				// First part of message contains the timestamp
				memcpy(message, &nanoseconds_since_epoch, sizeof(long long));
			}else{
				// Capture current timestamp and embed it in the message
				auto timestamp = std::chrono::steady_clock::now();
				long long nanoseconds_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
						timestamp.time_since_epoch()).count();

				// First part of message contains the timestamp
				memcpy(message, &nanoseconds_since_epoch, sizeof(long long));
			}

			// Send the message
			p.Publish(message, message_size);

			sent_bytes += paddedMsgSizeWithHeader;
		}

		// Finalize publishing
		p.DEBUG_check_send_finish();
		p.Poll(n);

		// Record publish end time
		auto pub_end = std::chrono::high_resolution_clock::now();

		// Wait for all messages to be received
		LOG(INFO) << "Publishing complete, waiting for subscriber to receive all data...";
		s.Poll(total_message_size, message_size);

		// Record end-to-end end time
		auto end = std::chrono::high_resolution_clock::now();

		// Calculate bandwidths
		double pub_seconds = std::chrono::duration<double>(pub_end - start).count();
		double e2e_seconds = std::chrono::duration<double>(end - start).count();

		double pubBandwidthMbps = (total_message_size / (1024 * 1024)) / pub_seconds;
		double e2eBandwidthMbps = (total_message_size / (1024 * 1024)) / e2e_seconds;

		LOG(INFO) << "Publish completed in " << std::fixed << std::setprecision(2) 
			<< pub_seconds << " seconds, " << pubBandwidthMbps << " MB/s";
		LOG(INFO) << "End-to-end completed in " << std::fixed << std::setprecision(2) 
			<< e2e_seconds << " seconds, " << e2eBandwidthMbps << " MB/s";

		// Process latency data
		s.DEBUG_check_order(order);
		s.StoreLatency();

		return std::make_pair(pubBandwidthMbps, e2eBandwidthMbps);

	} catch (const std::exception& e) {
		LOG(ERROR) << "Exception during latency test: " << e.what();
		return std::make_pair(0.0, 0.0);
	}
}

bool KillBrokers(std::unique_ptr<HeartBeat::Stub>& stub, int num_brokers) {
	LOG(INFO) << "Requesting to kill " << num_brokers << " brokers";

	// Prepare request
	grpc::ClientContext context;
	heartbeat_system::KillBrokersRequest req;
	heartbeat_system::KillBrokersResponse reply;

	// Set number of brokers to kill
	req.set_num_brokers(num_brokers);

	// Send request
	grpc::Status status = stub->KillBrokers(&context, req, &reply);

	if (!status.ok()) {
		LOG(ERROR) << "Failed to kill brokers: " << status.error_message();
		return false;
	}

	if (!reply.success()) {
		LOG(ERROR) << "Server returned failure when killing brokers";
		return false;
	}

	LOG(INFO) << "Successfully killed " << num_brokers << " brokers";
	return true;
}
