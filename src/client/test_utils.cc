#include "test_utils.h"
#include "../common/configuration.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <random>
#include <thread>
#include <fstream>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <vector>

// Helper function to generate random message content
void FillRandomData(char* buffer, size_t size) {
	static thread_local std::mt19937 gen(std::random_device{}());
	static thread_local std::uniform_int_distribution<char> dist(32, 126); // Printable ASCII chars

	for (size_t i = 0; i < size; i++) {
		buffer[i] = dist(gen);
	}
}

static bool ShouldValidateOrder() {
	const char* env = std::getenv("EMBAR_VALIDATE_ORDER");
	if (!env) {
		return false;
	}
	return std::strcmp(env, "0") != 0;
}

namespace {

struct StageMetricSummary {
	double average = 0.0;
	double min = 0.0;
	double p50 = 0.0;
	double p99 = 0.0;
	double p999 = 0.0;
	double max = 0.0;
	size_t count = 0;
	bool valid = false;
};

std::vector<std::string> SplitCsv(const std::string& line) {
	std::vector<std::string> out;
	std::stringstream ss(line);
	std::string cell;
	while (std::getline(ss, cell, ',')) out.push_back(cell);
	return out;
}

bool TryLoadMetricSummary(const std::string& path, const std::string& metric, StageMetricSummary* out) {
	if (out == nullptr) return false;
	std::ifstream f(path);
	if (!f.is_open()) return false;
	std::string header_line;
	if (!std::getline(f, header_line)) return false;
	const auto headers = SplitCsv(header_line);
	std::unordered_map<std::string, size_t> col;
	for (size_t i = 0; i < headers.size(); ++i) col[headers[i]] = i;
	if (!col.count("Metric")) return false;
	auto parse_double = [&](const std::vector<std::string>& row, const std::string& name, double* v) -> bool {
		auto it = col.find(name);
		if (it == col.end() || it->second >= row.size()) return false;
		try { *v = std::stod(row[it->second]); return true; } catch (...) { return false; }
	};
	auto parse_size = [&](const std::vector<std::string>& row, const std::string& name, size_t* v) -> bool {
		auto it = col.find(name);
		if (it == col.end() || it->second >= row.size()) return false;
		try { *v = static_cast<size_t>(std::stoull(row[it->second])); return true; } catch (...) { return false; }
	};
	std::string line;
	while (std::getline(f, line)) {
		if (line.empty()) continue;
		const auto row = SplitCsv(line);
		size_t metric_col = col["Metric"];
		if (metric_col >= row.size() || row[metric_col] != metric) continue;
		StageMetricSummary s;
		if (!parse_double(row, "Average", &s.average)) return false;
		if (!parse_double(row, "Min", &s.min)) return false;
		if (!parse_double(row, "Median", &s.p50)) return false;
		if (!parse_double(row, "p99", &s.p99)) return false;
		if (!parse_double(row, "p999", &s.p999)) return false;
		if (!parse_double(row, "Max", &s.max)) return false;
		if (!parse_size(row, "Count", &s.count)) return false;
		s.valid = true;
		*out = s;
		return true;
	}
	return false;
}

void WriteStageLatencySummary(const StageMetricSummary& ordered,
		const StageMetricSummary& ack,
		const StageMetricSummary& deliver,
		bool monotonic_ok) {
	std::ofstream out("stage_latency_summary.csv");
	if (!out.is_open()) {
		LOG(ERROR) << "Failed to open stage_latency_summary.csv";
		return;
	}
	out << "Stage,Average,Min,p50,p99,p999,Max,Count\n";
	if (ordered.valid) {
		out << "append_send_to_ordered," << ordered.average << "," << ordered.min << ","
		    << ordered.p50 << "," << ordered.p99 << "," << ordered.p999 << ","
		    << ordered.max << "," << ordered.count << "\n";
	}
	if (ack.valid) {
		out << "append_send_to_ack," << ack.average << "," << ack.min << ","
		    << ack.p50 << "," << ack.p99 << "," << ack.p999 << ","
		    << ack.max << "," << ack.count << "\n";
	}
	if (deliver.valid) {
		out << "append_send_to_deliver," << deliver.average << "," << deliver.min << ","
		    << deliver.p50 << "," << deliver.p99 << "," << deliver.p999 << ","
		    << deliver.max << "," << deliver.count << "\n";
	}
	out << "monotonic_ordered_le_ack_le_deliver,,,,,,," << (monotonic_ok ? 1 : 0) << "\n";
}

void CheckStageLatencyMonotonicity(int ack_level) {
	StageMetricSummary ordered{};
	StageMetricSummary ack{};
	StageMetricSummary deliver{};
	const bool have_ack = TryLoadMetricSummary("pub_latency_stats.csv", "append_send_to_ack_batch_latency", &ack);
	bool have_ordered = TryLoadMetricSummary("pub_latency_stats.csv", "append_send_to_ordered_batch_latency", &ordered);
	const bool have_deliver =
		TryLoadMetricSummary("latency_stats.csv", "publish_to_deliver_latency", &deliver) ||
		TryLoadMetricSummary("latency_stats.csv", "append_send_to_deliver_message_latency", &deliver);
	if (!have_ordered && have_ack && ack_level == 1) {
		ordered = ack;
		ordered.valid = true;
		have_ordered = true;
		LOG(INFO) << "Stage latency ordered metric inferred from ACK metric (ack_level=1).";
	}
	bool monotonic_ok = false;
	if (have_ordered && have_ack && have_deliver) {
		monotonic_ok =
			(ordered.p50 <= ack.p50 && ack.p50 <= deliver.p50) &&
			(ordered.p99 <= ack.p99 && ack.p99 <= deliver.p99) &&
			(ordered.p999 <= ack.p999 && ack.p999 <= deliver.p999) &&
			(ordered.max <= ack.max && ack.max <= deliver.max);
		if (monotonic_ok) {
			LOG(INFO) << "Stage latency monotonicity check passed: ordered <= ack <= deliver (p50/p99/p999/max).";
		} else {
			LOG(WARNING) << "Stage latency monotonicity check failed (p50/p99/p999/max).";
		}
	} else {
		LOG(WARNING) << "Stage latency monotonicity check skipped: missing metrics "
		             << "(ordered=" << have_ordered << ", ack=" << have_ack << ", deliver=" << have_deliver << ")";
	}
	WriteStageLatencySummary(ordered, ack, deliver, monotonic_ok);
}

}  // namespace

// Helper function to calculate optimal queue size based on configuration
size_t CalculateOptimalQueueSize(size_t num_threads_per_broker, size_t total_message_size, size_t message_size) {
	const Embarcadero::Configuration& config = Embarcadero::Configuration::getInstance();
	
	// OPTIMIZED: Use 256MB constant per thread as determined from previous buffer optimization tests
	// This eliminates buffer wrapping issues and provides optimal performance across all message sizes
	const size_t OPTIMAL_BUFFER_SIZE_MB = 256;
	size_t buffer_size_per_thread_bytes = OPTIMAL_BUFFER_SIZE_MB * 1024 * 1024; // 256MB per thread
	
	// Total buffer size = threads_per_broker * brokers * 256MB_per_thread
	size_t num_brokers = config.config().broker.max_brokers.get();
	size_t total_buffer_size = num_threads_per_broker * num_brokers * buffer_size_per_thread_bytes;
	
	// For small messages that require more total buffer space, ensure minimum capacity
	size_t header_overhead = (total_message_size / message_size) * 64; // 64 bytes per message header
	size_t required_size = total_message_size + header_overhead + (2 * 1024 * 1024); // 2MB safety margin
	
	// Always use the optimized 256MB per thread, but ensure it's sufficient for the dataset
	size_t queue_size = std::max(total_buffer_size, required_size);
	
	VLOG(1) << "Using optimized 256MB per thread: " << (total_buffer_size / (1024 * 1024)) << " MB total "
	          << "(required for dataset: " << (required_size / (1024 * 1024)) << " MB, "
	          << "final queue: " << (queue_size / (1024 * 1024)) << " MB)";
	
	return std::max(queue_size, static_cast<size_t>(1024)); // Minimum 1KB
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

				VLOG(1) << "Progress: " << std::fixed << std::setprecision(1) << progress_pct << "% "
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

	// Calculate optimal queue size based on configuration
	// Use fixed small queue size to throttle producer so it stays alive throughout test
	// We use 8MB total queue size to force the producer to keep running alongside the network sends.
	size_t q_size = 1024 * 1024 * 8; // Very small queue to force producer to block
	q_size = std::max(q_size, static_cast<size_t>(1024));

	// Initialize subscriber BEFORE publisher to make sure they're ready to receive
	LOG(INFO) << "Setting up dummy subscriber to keep connections open if needed";
	// Note: the test currently focuses on Publisher throughput so we might not use the subscriber directly here 
	// But it might be necessary if we wanted end-to-end failure testing.

	// Create publisher
	Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
		num_threads_per_broker, message_size, q_size, order);
#ifdef COLLECT_LATENCY_STATS
	p.SetRecordResults(result.count("record_results") > 0);
#endif

	try {
		p.RecordStartTime();
		p.Init(ack_level);

		auto warmup_start = std::chrono::high_resolution_clock::now();
		p.WarmupBuffers();
		auto warmup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::high_resolution_clock::now() - warmup_start).count();
		LOG(INFO) << "Failure test buffer warmup completed in " << warmup_ms << " ms";

		p.FailBrokers(total_message_size, message_size, failure_percentage, killbrokers);

		// Create progress tracker
		//ProgressTracker progress(n, 1000);

		// Start timing
		auto start = std::chrono::high_resolution_clock::now();

		size_t batch_size = 10000;
		size_t max_in_flight_bytes = 100 * 1024 * 1024;
		size_t max_in_flight_msgs = max_in_flight_bytes / message_size;

		for (size_t i = 0; i < n; i++) {
			p.Publish(message, message_size);

			if (i > 0 && i % batch_size == 0) {
				if (p.GetShutdown()) {
					LOG(WARNING) << "Publishing loop interrupted due to shutdown at message " << i;
					break;
				}

				if (!p.IsThrottleRelaxed() && i > p.GetAckReceived() + max_in_flight_msgs) {
					auto throttle_start = std::chrono::steady_clock::now();
					int spin_count = 0;
					while (!p.GetShutdown() && !p.IsThrottleRelaxed() &&
					       i > p.GetAckReceived() + max_in_flight_msgs) {
						if (++spin_count % 100 == 0) {
							std::this_thread::sleep_for(std::chrono::microseconds(500));
						} else {
							Embarcadero::CXL::cpu_pause();
						}
						auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::steady_clock::now() - throttle_start).count();
						if (elapsed_ms >= 2000) {
							size_t acked = p.GetAckReceived();
							LOG(WARNING) << "Throttle stall: published=" << i << " acked=" << acked
							             << " gap=" << (i - acked) << " limit=" << max_in_flight_msgs
							             << " kill_brokers=" << p.IsKillBrokersActive()
							             << ". Relaxing in-flight limit to continue.";
							max_in_flight_msgs = i - acked + batch_size;
							break;
						}
					}
				}
			}
		}

		// Finalize publishing (Poll() seals, sets shutdown, joins threads, waits for ACKs)
		if (!p.Poll(n)) {
			LOG(ERROR) << "Publish test failed: not all messages acknowledged (ACK timeout or shortfall). See logs above for per-broker details.";
			delete[] message;
			exit(1);
		}

		const char* failure_dir = getenv("EMBARCADERO_FAILURE_DATA_DIR");
		std::string events_dir = (failure_dir && failure_dir[0]) ? failure_dir : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/Embarcadero/data/failure";
		std::string events_file = events_dir + "/failure_events.csv";
		p.WriteFailureEventsToFile(events_file);
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

	VLOG(1) << "Starting publish throughput test with " << n << " messages"
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

	// Calculate optimal queue size based on configuration
	size_t q_size = CalculateOptimalQueueSize(num_threads_per_broker, total_message_size, message_size);

		// Create publisher
		Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
			num_threads_per_broker, message_size, q_size, order, seq_type);
	#ifdef COLLECT_LATENCY_STATS
	p.SetRecordResults(result.count("record_results") > 0);
#endif

			try {
				// Initialize publisher
				p.Init(ack_level);
				// Warmup buffers to eliminate page-fault variance (same as other throughput tests).
				auto warmup_start = std::chrono::steady_clock::now();
				p.WarmupBuffers();
				auto warmup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - warmup_start).count();
				VLOG(1) << "Publish warmup completed in " << warmup_ms << " ms";

				// Synchronize with other clients
				int prev = synchronizer.fetch_sub(1, std::memory_order_acq_rel);
				VLOG(2) << "Publish sync decrement: prev=" << prev
				        << " now=" << synchronizer.load(std::memory_order_acquire);
			const char* sync_timeout_env = std::getenv("EMBAR_PUBLISH_SYNC_TIMEOUT_SEC");
			int sync_timeout_sec = sync_timeout_env ? std::atoi(sync_timeout_env) : 30;
			auto sync_start = std::chrono::steady_clock::now();
			auto last_sync_log = sync_start;

				while (synchronizer.load(std::memory_order_acquire) != 0) {
					auto now = std::chrono::steady_clock::now();
					auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - sync_start).count();
					if (now - last_sync_log >= std::chrono::seconds(2)) {
						VLOG(1) << "Publish sync waiting: remaining="
						        << synchronizer.load(std::memory_order_relaxed)
						        << " elapsed_sec=" << elapsed_sec;
						last_sync_log = now;
					}
					if (sync_timeout_sec > 0 && elapsed_sec >= sync_timeout_sec) {
						LOG(ERROR) << "Publish sync wait timeout after " << sync_timeout_sec
						           << "s remaining=" << synchronizer.load(std::memory_order_relaxed);
						delete[] message;
						return 0.0;
					}
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
				if (!p.Poll(n, false)) {
					LOG(ERROR) << "Publish test failed: not all messages acknowledged (ACK timeout or shortfall). See logs above for per-broker details.";
					delete[] message;
					exit(1);
				}

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

		// Create subscriber with order level for batch-aware processing
		Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, false, order);

		// Track start of the actual receiving process
		auto receive_start = std::chrono::high_resolution_clock::now();

		// Wait for all messages to be received
		VLOG(5) << "Waiting to receive " << total_message_size << " bytes of data";

		// All order levels use efficient passive polling
		VLOG(3) << "Using passive polling for order level " << order;
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
		if (ShouldValidateOrder() && !s.DEBUG_check_order(order)) {
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

		// Create subscriber with order level for batch-aware processing  
		Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, false, order);
		s.WaitUntilAllConnected(); // Assume there exists NUM_MAX_BROKERS

		// Track start of the actual receiving process
		auto receive_start = std::chrono::high_resolution_clock::now();

		// Wait for all messages to be received
		VLOG(5) << "Waiting to receive " << total_message_size << " bytes of data";

		for(size_t i=0; i< n; i++){
			void* msg = nullptr;
			size_t retry_count = 0;
			const size_t max_retries = 10; // Allow up to 10 seconds of retries
			
			while((msg = s.Consume(1000)) == nullptr){
				retry_count++;
				if (retry_count >= max_retries) {
					LOG(ERROR) << "ConsumeThroughputTest: Failed to consume message " << i << " after " << max_retries << " seconds. Aborting test.";
					return 0.0; // Return 0 bandwidth to indicate failure
				}
				VLOG(3) << "ConsumeThroughputTest: Retry " << retry_count << " for message " << i;
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
		if (ShouldValidateOrder() && !s.DEBUG_check_order(order)) {
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

	// Calculate optimal queue size based on configuration
	size_t q_size = CalculateOptimalQueueSize(num_threads_per_broker, total_message_size, message_size);

	try {
		// PERF OPTIMIZATION: Move all initialization out of timing measurement
		// This eliminates variance from buffer allocation, network setup, and thread creation
		
		LOG(INFO) << "Initializing publisher and subscriber (not measured)...";
		auto init_start = std::chrono::high_resolution_clock::now();
		
		// Create publisher and subscriber
		Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
				num_threads_per_broker, message_size, q_size, order, seq_type);
		Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, false, order);
		
		// Wait for subscriber connections (network setup - not measured)
		s.WaitUntilAllConnected();

		// Initialize publisher (buffer allocation + network threads - not measured)
		p.Init(ack_level);
		
		// Warmup buffers to eliminate page fault variance (not measured)
		p.WarmupBuffers();
		
		auto init_end = std::chrono::high_resolution_clock::now();
		double init_seconds = std::chrono::duration<double>(init_end - init_start).count();
		LOG(INFO) << "Initialization completed in " << std::fixed << std::setprecision(3) 
		          << init_seconds << "s (excluded from performance measurement)";

		// NOW start timing for pure critical path performance
		LOG(INFO) << "Starting critical path measurement...";
		auto start = std::chrono::high_resolution_clock::now();

		// Publish messages
		for (size_t i = 0; i < n; i++) {
			p.Publish(message, message_size);
		}

		// Finalize publishing (Poll() seals, sets shutdown, joins threads, waits for ACKs)
		if (!p.Poll(n, false)) {
			LOG(ERROR) << "End-to-end test failed: not all messages acknowledged (ACK timeout or shortfall). See logs above for per-broker details.";
			delete[] message;
			exit(1);
		}

		// Record publish end time
		auto pub_end = std::chrono::high_resolution_clock::now();

		// Wait for all messages to be received by subscriber
		LOG(INFO) << "Publishing complete, waiting for subscriber to receive all data...";
		
		// All order levels now use efficient passive polling
		// Sequencer 5 logical reconstruction happens in receiver threads
		VLOG(3) << "Using passive polling for order level " << order;
		s.Poll(total_message_size, message_size);

		// Record end-to-end end time
		auto end = std::chrono::high_resolution_clock::now();

		// Calculate publish bandwidth
		double pub_seconds = std::chrono::duration<double>(pub_end - start).count();
		double pubBandwidthMbps = ((message_size * n) / pub_seconds) / (1024 * 1024);

		// Calculate end-to-end bandwidth
		double e2e_seconds = std::chrono::duration<double>(end - start).count();
		double e2eBandwidthMbps = ((message_size * n) / e2e_seconds) / (1024 * 1024);

		// Check message ordering (add small delay to ensure buffers are stable)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (ShouldValidateOrder()) {
			s.DEBUG_check_order(order);
		}

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
	double target_mbps = 0.0;
	if (result.count("target_mbps")) {
		target_mbps = result["target_mbps"].as<double>();
	}
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
		<< (steady_rate ? ", using steady rate" : "")
		<< (target_mbps > 0.0 ? ", target_offered_load=" + std::to_string(target_mbps) + " MB/s" : "");

	// Allocate message buffer on heap-backed vector to avoid large stack allocations.
	std::vector<char> message(message_size);

	// Calculate queue size with buffer
	size_t q_size = total_message_size + (total_message_size / message_size) * 64 + 2097152;
	q_size = std::max(q_size, static_cast<size_t>(1024));

		try {
			// Create publisher and subscriber
			Publisher p(topic, "127.0.0.1", std::to_string(BROKER_PORT), 
					num_threads_per_broker, message_size, q_size, order, seq_type);
#ifdef COLLECT_LATENCY_STATS
			p.SetRecordResults(result.count("record_results") > 0);
#endif
			Subscriber s("127.0.0.1", std::to_string(BROKER_PORT), topic, true, order);
			s.WaitUntilAllConnected();

			// Initialize publisher
			p.Init(ack_level);

		// Set up progress tracking
		//ProgressTracker progress(n, 1000);

		// Start timing
		auto start = std::chrono::high_resolution_clock::now();
		const auto pace_start = std::chrono::steady_clock::now();
		const double target_bytes_per_sec = (target_mbps > 0.0) ? (target_mbps * 1024.0 * 1024.0) : 0.0;
		uint64_t offered_bytes = 0;

		// Publish messages with timestamps
		size_t sent_bytes = 0;
		for (size_t i = 0; i < n; i++) {

			// If using steady rate, pause periodically
			if (steady_rate && (sent_bytes >= (BATCH_SIZE*4))) {
				p.WriteFinishedOrPaused();
				std::this_thread::sleep_for(std::chrono::microseconds(1500));
				sent_bytes = 0;
				// Capture current timestamp and embed it in the message
				auto timestamp = std::chrono::steady_clock::now();
				long long nanoseconds_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
						timestamp.time_since_epoch()).count();

				// First part of message contains the timestamp
				memcpy(message.data(), &nanoseconds_since_epoch, sizeof(long long));
			}else{
				// Capture current timestamp and embed it in the message
				auto timestamp = std::chrono::steady_clock::now();
				long long nanoseconds_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
						timestamp.time_since_epoch()).count();

				// First part of message contains the timestamp
				memcpy(message.data(), &nanoseconds_since_epoch, sizeof(long long));
			}

			// Send the message
			if (target_bytes_per_sec > 0.0) {
				const double expected_ns = (static_cast<double>(offered_bytes) * 1e9) / target_bytes_per_sec;
				auto target_time = pace_start + std::chrono::nanoseconds(static_cast<long long>(expected_ns));
				auto now = std::chrono::steady_clock::now();
				if (target_time > now) {
					std::this_thread::sleep_until(target_time);
				}
			}
			p.Publish(message.data(), message_size);
			offered_bytes += paddedMsgSizeWithHeader;

			sent_bytes += paddedMsgSizeWithHeader;
		}
		auto publish_dispatch_end = std::chrono::steady_clock::now();

		// Finalize publishing (Poll() seals, sets shutdown, joins threads, waits for ACKs)
		if (!p.Poll(n)) {
			LOG(ERROR) << "Latency test failed: not all messages acknowledged (ACK timeout or shortfall). See logs above for per-broker details.";
			exit(1);
		}

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
		double offered_seconds = std::chrono::duration<double>(publish_dispatch_end - pace_start).count();
		double achieved_offered_mbps = offered_seconds > 0.0
			? (static_cast<double>(offered_bytes) / (1024.0 * 1024.0)) / offered_seconds
			: 0.0;

		LOG(INFO) << "Publish completed in " << std::fixed << std::setprecision(2) 
			<< pub_seconds << " seconds, " << pubBandwidthMbps << " MB/s";
		LOG(INFO) << "End-to-end completed in " << std::fixed << std::setprecision(2) 
			<< e2e_seconds << " seconds, " << e2eBandwidthMbps << " MB/s";
		LOG(INFO) << "Latency offered-load summary: target=" << target_mbps
		          << " MB/s, achieved_offered=" << achieved_offered_mbps
		          << " MB/s, achieved_goodput=" << e2eBandwidthMbps << " MB/s";

		// Process latency data
		if (ShouldValidateOrder()) {
			s.DEBUG_check_order(order);
		}
		s.StoreLatency();
		CheckStageLatencyMonotonicity(ack_level);

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
