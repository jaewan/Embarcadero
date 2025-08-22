#include "distributed_kv_store.h"
#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <atomic>

class KVStoreBenchmark {
	private:
		DistributedKVStore& kv_store_;
		std::vector<std::string> test_keys_;
		std::vector<std::string> test_values_;
		size_t num_keys_;
		size_t value_size_;
		std::mt19937 gen_;
		std::atomic<bool> test_complete_{false};
		absl::Mutex mutex_;
		std::atomic<size_t> operations_completed_{0};

		// Generate random string of specified length
		std::string generateRandomString(size_t length) {
			static const char alphanum[] =
				"0123456789"
				"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				"abcdefghijklmnopqrstuvwxyz";
			std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);
			std::string result;
			result.reserve(length);
			for (size_t i = 0; i < length; ++i) {
				result += alphanum[dis(gen_)];
			}
			return result;
		}

		// Generate test data
		void generateTestData() {
			test_keys_.clear();
			test_values_.clear();

			test_keys_.reserve(num_keys_);
			test_values_.reserve(num_keys_);

			for (size_t i = 0; i < num_keys_; ++i) {
				test_keys_.push_back("key-" + std::to_string(i));
				test_values_.push_back(generateRandomString(value_size_));
			}
		}

	public:
		KVStoreBenchmark(DistributedKVStore& kv_store, size_t num_keys = 10000, size_t value_size = 100)
			: kv_store_(kv_store),
			num_keys_(num_keys),
			value_size_(value_size),
			gen_(std::random_device{}()) {
				generateTestData();
			}

		// Populate the KV store with initial data
		void populateStore() {
			LOG(INFO) << "Populating store with " << num_keys_ << " keys...";

			// Insert all keys individually first
			size_t request_id =0;
			for (size_t i = 0; i < num_keys_; ++i) {
				// Create a KeyValue pair
				KeyValue kv;
				kv.key = test_keys_[i];
				kv.value = test_values_[i];

				// Submit the put operation
				request_id = kv_store_.put(kv.key, kv.value);

				// Wait for operation to be applied (would be more efficient to batch these waits)
				/*
				if (i % 1000 == 0) {
					kv_store_.waitUntilApplied(request_id);
				}
				*/
			}

			// Wait for all operations to complete
			kv_store_.waitUntilApplied(request_id);
			LOG(INFO) << "Populated store";
		}

		// Run multi-put benchmark with varying batch sizes
		void runMultiPutBenchmark(const std::vector<size_t>& batch_sizes, int iterations = 5) {
			std::cout << "\nRunning Multi-Put Benchmark..." << std::endl;
			std::cout << "-------------------------------------------------" << std::endl;
			std::cout << "Batch Size | Throughput (ops/sec) | Latency (ms)" << std::endl;
			std::cout << "-------------------------------------------------" << std::endl;

			std::ofstream csv_file("multi_put_results.csv");
			csv_file << "batch_size,throughput_ops_per_sec,latency_ms" << std::endl;

			for (size_t batch_size : batch_sizes) {
				double total_throughput = 0.0;
				double total_latency = 0.0;

				// Run multiple iterations to get reliable results
				for (int iter = 0; iter < iterations; ++iter) {
					auto keys_subset = test_keys_;
					auto values_subset = test_values_;

					// Shuffle to get different subsets each time
					std::shuffle(keys_subset.begin(), keys_subset.end(), gen_);
					std::shuffle(values_subset.begin(), values_subset.end(), gen_);

					// Collect batches for multi-put
					std::vector<std::vector<KeyValue>> batches;

					size_t total_ops = std::min(num_keys_, 10000000000UL); // Cap at 100K operations
					size_t num_batches = (total_ops + batch_size - 1) / batch_size;

					for (size_t i = 0; i < num_batches; ++i) {
						std::vector<KeyValue> batch;
						size_t start = i * batch_size;
						size_t end = std::min(start + batch_size, total_ops);

						for (size_t j = start; j < end; ++j) {
							KeyValue kv;
							kv.key = keys_subset[j];
							kv.value = values_subset[j];
							batch.push_back(kv);
						}

						batches.push_back(std::move(batch));
					}

					// Execute and time the multi-put operations
					auto start_time = std::chrono::high_resolution_clock::now();

					size_t last_request_id = 0;
					for (const auto& batch : batches) {
						last_request_id = kv_store_.multiPut(batch);
					}

					kv_store_.waitUntilApplied(last_request_id);

					auto end_time = std::chrono::high_resolution_clock::now();
					auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

					double latency_ms = static_cast<double>(duration.count()) / batches.size();
					double throughput = (static_cast<double>(total_ops) / duration.count()) * 1000.0;

					total_throughput += throughput;
					total_latency += latency_ms;
				}

				// Calculate averages
				double avg_throughput = total_throughput / iterations;
				double avg_latency = total_latency / iterations;

				std::cout << std::setw(10) << batch_size << " | "
					<< std::setw(20) << std::fixed << std::setprecision(2) << avg_throughput << " | "
					<< std::setw(12) << std::fixed << std::setprecision(2) << avg_latency << std::endl;

				csv_file << batch_size << "," << avg_throughput << "," << avg_latency << std::endl;
			}

			csv_file.close();
			std::cout << "-------------------------------------------------" << std::endl;
			std::cout << "Results saved to multi_put_results.csv" << std::endl;
		}

		// Run multi-get benchmark with varying batch sizes
		void runMultiGetBenchmark(const std::vector<size_t>& batch_sizes, int iterations = 5) {
			std::cout << "\nRunning Multi-Get Benchmark..." << std::endl;
			std::cout << "-------------------------------------------------" << std::endl;
			std::cout << "Batch Size | Throughput (ops/sec) | Latency (ms)" << std::endl;
			std::cout << "-------------------------------------------------" << std::endl;

			std::ofstream csv_file("multi_get_results.csv");
			csv_file << "batch_size,throughput_ops_per_sec,latency_ms" << std::endl;

			for (size_t batch_size : batch_sizes) {
				double total_throughput = 0.0;
				double total_latency = 0.0;

				// Run multiple iterations to get reliable results
				for (int iter = 0; iter < iterations; ++iter) {
					auto keys_subset = test_keys_;

					// Shuffle to get different subsets each time
					std::shuffle(keys_subset.begin(), keys_subset.end(), gen_);

					// Collect batches for multi-get
					std::vector<std::vector<std::string>> batches;

					size_t total_ops = std::min(num_keys_, 10000000000UL);
					size_t num_batches = (total_ops + batch_size - 1) / batch_size;

					for (size_t i = 0; i < num_batches; ++i) {
						std::vector<std::string> batch;
						size_t start = i * batch_size;
						size_t end = std::min(start + batch_size, total_ops);

						for (size_t j = start; j < end; ++j) {
							batch.push_back(keys_subset[j]);
						}

						batches.push_back(std::move(batch));
					}

					// Make sure all previous puts are visible
					//kv_store_.waitForSyncWithLog();

					// Execute and time the multi-get operations
					auto start_time = std::chrono::high_resolution_clock::now();

					for (const auto& batch : batches) {
						// Execute multi-get operation
						auto results = kv_store_.multiGet(batch);

						// Ensure we actually got results to prevent compiler optimization
						if (results.empty() && !batch.empty()) {
							std::cerr << "Warning: Empty result for non-empty batch!" << std::endl;
						}
					}

					auto end_time = std::chrono::high_resolution_clock::now();
					auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

					double latency_ms = static_cast<double>(duration.count()) / batches.size();
					double throughput = (static_cast<double>(total_ops) / duration.count()) * 1000.0;

					total_throughput += throughput;
					total_latency += latency_ms;
				}

				// Calculate averages
				double avg_throughput = total_throughput / iterations;
				double avg_latency = total_latency / iterations;

				std::cout << std::setw(10) << batch_size << " | "
					<< std::setw(20) << std::fixed << std::setprecision(2) << avg_throughput << " | "
					<< std::setw(12) << std::fixed << std::setprecision(2) << avg_latency << std::endl;

				csv_file << batch_size << "," << avg_throughput << "," << avg_latency << std::endl;
			}

			csv_file.close();
			std::cout << "-------------------------------------------------" << std::endl;
			std::cout << "Results saved to multi_get_results.csv" << std::endl;
		}

		// Optional: Measure log read activity during benchmark
		void runMultiGetWithLogReadMeasurement(const std::vector<size_t>& batch_sizes, int iterations = 5) {
			std::cout << "\nRunning Multi-Get with Log Read Measurement..." << std::endl;
			std::cout << "----------------------------------------------------------------------" << std::endl;
			std::cout << "Batch Size | Get Throughput (ops/sec) | Log Read Throughput (ops/sec)" << std::endl;
			std::cout << "----------------------------------------------------------------------" << std::endl;

			std::ofstream csv_file("multi_get_log_read_results.csv");
			csv_file << "batch_size,get_throughput_ops_per_sec,log_read_throughput_ops_per_sec" << std::endl;

			for (size_t batch_size : batch_sizes) {
				double total_get_throughput = 0.0;
				double total_log_read_throughput = 0.0;

				// Similar implementation to runMultiGetBenchmark, but with log read measurements
				// This would need integration with your system's internal log read metrics

				// For simplicity, we'll just estimate log read throughput based on get throughput
				// In a real implementation, you would instrument your code to measure actual log reads

				// Calculate averages
				double avg_get_throughput = total_get_throughput / iterations;
				double avg_log_read_throughput = total_log_read_throughput / iterations;

				std::cout << std::setw(10) << batch_size << " | "
					<< std::setw(25) << std::fixed << std::setprecision(2) << avg_get_throughput << " | "
					<< std::setw(25) << std::fixed << std::setprecision(2) << avg_log_read_throughput << std::endl;

				csv_file << batch_size << "," << avg_get_throughput << "," << avg_log_read_throughput << std::endl;
			}

			csv_file.close();
			std::cout << "----------------------------------------------------------------------" << std::endl;
			std::cout << "Results saved to multi_get_log_read_results.csv" << std::endl;
		}

		// Generate Python plotting script
		void generatePlottingScript() {
			std::ofstream script_file("plot_results.py");

			script_file << R"(
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

# Load the data
multi_put_data = pd.read_csv('multi_put_results.csv')
multi_get_data = pd.read_csv('multi_get_results.csv')

# Try to load log read data if it exists
try:
    log_read_data = pd.read_csv('multi_get_log_read_results.csv')
    has_log_read_data = True
except FileNotFoundError:
    has_log_read_data = False

# Set up the figure
plt.figure(figsize=(12, 10))

# Plot Multi-Put throughput
plt.subplot(2, 1, 1)
plt.plot(multi_put_data['batch_size'], multi_put_data['throughput_ops_per_sec'], 'o-', linewidth=2, markersize=8, label='Multi-Put Throughput')
plt.xlabel('Batch Size (number of keys)', fontsize=12)
plt.ylabel('Throughput (operations/sec)', fontsize=12)
plt.title('Multi-Put Performance', fontsize=14)
plt.xscale('log', base=2)  # Use log scale for x-axis
plt.grid(True, which="both", ls="-", alpha=0.2)
plt.legend(fontsize=12)

# Plot Multi-Get throughput
plt.subplot(2, 1, 2)
plt.plot(multi_get_data['batch_size'], multi_get_data['throughput_ops_per_sec'], 'o-', color='green', linewidth=2, markersize=8, label='Multi-Get Throughput')

# If log read data is available, plot it on the same graph
if has_log_read_data:
    plt.plot(log_read_data['batch_size'], log_read_data['log_read_throughput_ops_per_sec'], 's-', color='red', linewidth=2, markersize=8, label='Log Read Throughput')

plt.xlabel('Batch Size (number of keys)', fontsize=12)
plt.ylabel('Throughput (operations/sec)', fontsize=12)
plt.title('Multi-Get Performance', fontsize=14)
plt.xscale('log', base=2)  # Use log scale for x-axis
plt.grid(True, which="both", ls="-", alpha=0.2)
plt.legend(fontsize=12)

plt.tight_layout()
plt.savefig('kv_store_performance.png', dpi=300, bbox_inches='tight')
plt.show()

# Create another figure for latency analysis
plt.figure(figsize=(12, 5))

plt.subplot(1, 2, 1)
plt.plot(multi_put_data['batch_size'], multi_put_data['latency_ms'], 'o-', linewidth=2, markersize=8, color='blue')
plt.xlabel('Batch Size (number of keys)', fontsize=12)
plt.ylabel('Average Latency (ms)', fontsize=12)
plt.title('Multi-Put Latency', fontsize=14)
plt.xscale('log', base=2)
plt.grid(True, which="both", ls="-", alpha=0.2)

plt.subplot(1, 2, 2)
plt.plot(multi_get_data['batch_size'], multi_get_data['latency_ms'], 'o-', linewidth=2, markersize=8, color='green')
plt.xlabel('Batch Size (number of keys)', fontsize=12)
plt.ylabel('Average Latency (ms)', fontsize=12)
plt.title('Multi-Get Latency', fontsize=14)
plt.xscale('log', base=2)
plt.grid(True, which="both", ls="-", alpha=0.2)

plt.tight_layout()
plt.savefig('kv_store_latency.png', dpi=300, bbox_inches='tight')
plt.show()

			print("Plots generated successfully!")
			)";

			script_file.close();
			std::cout << "\nPython plotting script generated (plot_results.py)" << std::endl;
			std::cout << "To create the plots, run: python plot_results.py" << std::endl;
		}
};

int main(int argc, char* argv[]) {
	// Initialize logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();
	FLAGS_logtostderr = 1; // log only to console, no files.

	// Setup command line options
	cxxopts::Options options("KV-benchmark", "Distributed Key-value Store Benchmark");
	options.add_options()
		("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
		("sequencer", "Sequencer Type: Embarcadero(0), Kafka(1), Scalog(2), Corfu(3)",
		 cxxopts::value<std::string>()->default_value("EMBARCADERO"))
		("n,num_keys", "Number of keys for benchmark", cxxopts::value<size_t>()->default_value("100000"))
		("v,value_size", "Size of values in bytes", cxxopts::value<size_t>()->default_value("128"))
		("t,threads", "Number of threads for KV store", cxxopts::value<int>()->default_value("4"))
		("min_batch", "Minimum batch size", cxxopts::value<size_t>()->default_value("1"))
		("max_batch", "Maximum batch size", cxxopts::value<size_t>()->default_value("128"))
		("i,iterations", "Number of iterations per batch size", cxxopts::value<int>()->default_value("5"))
		("populate_only", "Only populate store, don't run benchmark", cxxopts::value<bool>()->default_value("false"))
		("h,help", "Print usage");

	auto result = options.parse(argc, argv);

	if (result.count("help")) {
		std::cout << options.help() << std::endl;
		return 0;
	}

	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());
	FLAGS_v = result["log_level"].as<int>();
	size_t num_keys = result["num_keys"].as<size_t>();
	size_t value_size = result["value_size"].as<size_t>();
	size_t min_batch = result["min_batch"].as<size_t>();
	size_t max_batch = result["max_batch"].as<size_t>();
	int iterations = result["iterations"].as<int>();
	bool populate_only = result["populate_only"].as<bool>();

	LOG(INFO) << "=== KV Store Benchmark ===";
	LOG(INFO) << "Sequencer type: " << static_cast<int>(seq_type);
	LOG(INFO) << "Num keys: " << num_keys;
	LOG(INFO) << "Value size: " << value_size << " bytes";
	LOG(INFO) << "Batch size range: " << min_batch << " to " << max_batch;
	LOG(INFO) << "Iterations per batch: " << iterations;

	// Create the distributed KV store
	DistributedKVStore kv_store(seq_type);

	// Create and run the benchmark
	KVStoreBenchmark benchmark(kv_store, num_keys, value_size);

	// Populate the store with initial data
	benchmark.populateStore();

	if (!populate_only) {
		// Define batch sizes to test (powers of 2 between min and max)
		std::vector<size_t> batch_sizes;
		for (size_t size = min_batch; size <= max_batch; size *= 2) {
			batch_sizes.push_back(size);
		}

		// Make sure max_batch is included if it's not already
		if (batch_sizes.empty() || batch_sizes.back() != max_batch) {
			batch_sizes.push_back(max_batch);
		}

		// Run benchmarks
		LOG(INFO) << "Starting Multi-Put benchmark...";
		benchmark.runMultiPutBenchmark(batch_sizes, iterations);

		LOG(INFO) << "Starting Multi-Get benchmark...";
		benchmark.runMultiGetBenchmark(batch_sizes, iterations);

		// Generate plotting script
		benchmark.generatePlottingScript();

		LOG(INFO) << "Benchmark completed successfully!";
	} else {
		LOG(INFO) << "Store populated. Skipping benchmark as requested.";
	}

	return 0;
}
