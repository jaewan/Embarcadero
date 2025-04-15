#include "result_writer.h"
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

ResultWriter::ResultWriter(const cxxopts::ParseResult& result)
    : message_size(result["size"].as<size_t>()),
      total_message_size(result["total_message_size"].as<size_t>()),
      num_threads_per_broker(result["num_threads_per_broker"].as<size_t>()),
      ack_level(result["ack_level"].as<int>()),
      order(result["order_level"].as<int>()),
      replication_factor(result["replication_factor"].as<int>()),
      replicate_tinode(result.count("replicate_tinode")),
      record_result_(result.count("record_results")),
      num_clients(result["parallel_client"].as<int>()),
      num_brokers_to_kill(result["num_brokers_to_kill"].as<int>()),
      failure_percentage(result["failure_percentage"].as<double>()),
      seq_type(result["sequencer"].as<std::string>()) {
    
    // Use the current time as a unique identifier for this test run
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::stringstream timestamp;
    timestamp << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S");
    
    // Base data directory, changed from hardcoded value to a configurable location
    // This uses the default location if EMBARCADERO_DATA_DIR env var is not set
    const char* data_dir_env = std::getenv("EMBARCADERO_DATA_DIR");
    std::string data_base_dir = data_dir_env ? data_dir_env : "/home/domin/Embarcadero/data/";
    
    // Define test type paths based on configuration
    int test_num = result["test_number"].as<int>();
    
    // Handle replication specially
    if (replication_factor > 0) {
        result_path = data_base_dir + "replication/";
        if (test_num == 2) {
            LOG(WARNING) << "Replication and latency tests cannot be combined. Decide where to store results";
        }
    } else if (test_num != 2 && test_num != 4) {
        result_path = data_base_dir + "throughput/";
    } else {
        // Default for latency and failure tests
        result_path = data_base_dir;
    }
    
    // Determine test-specific directory and filename
    std::string test_name;
    switch (test_num) {
        case 0:
            test_name = "pubsub";
            break;
        case 1:
            test_name = "e2e";
            break;
        case 2:
            test_name = "latency/e2e";
            break;
        case 3:
            test_name = "multiclient";
            break;
        case 4:
            test_name = "failure";
            break;
        case 5:
            test_name = "pub";
            break;
        case 6:
            test_name = "sub";
            break;
        default:
            test_name = "unknown";
            LOG(WARNING) << "Unknown test number: " << test_num;
            break;
    }
    
    // Combine path with test name
    result_path += test_name + "/";
    
    // Create output directory if it doesn't exist
    try {
        fs::create_directories(result_path);
    } catch (const fs::filesystem_error& e) {
        LOG(ERROR) << "Failed to create result directory: " << e.what();
    }
    
    // Define base result file name
    result_path += "result.csv";
    
    // If this is the first run, create the file with headers
    bool headers_needed = !fs::exists(result_path) || fs::file_size(result_path) == 0;
    
    if (record_result_ && headers_needed) {
        try {
            std::ofstream header_file(result_path);
            if (!header_file.is_open()) {
                LOG(ERROR) << "Failed to create result file: " << result_path << ": " << strerror(errno);
                return;
            }
            
            // Write CSV header
            header_file<< "message_size,"
                       << "total_message_size,"
                       << "num_threads_per_broker,"
                       << "ack_level,"
                       << "order,"
                       << "replication_factor,"
                       << "replicate_tinode,"
                       << "num_clients,"
                       << "num_brokers_to_kill,"
                       << "failure_percentage,"
                       << "sequencer_type,"
                       << "pub_bandwidth_mbps,"
                       << "sub_bandwidth_mbps,"
                       << "e2e_bandwidth_mbps\n";
            
            header_file.close();
            
            LOG(INFO) << "Created new result file with headers: " << result_path;
        } catch (const std::exception& e) {
            LOG(ERROR) << "Error creating header file: " << e.what();
        }
    }
}

ResultWriter::~ResultWriter() {
    if (!record_result_) {
        return;
    }
    
    try {
        std::ofstream file;
        file.open(result_path, std::ios::app);
        
        if (!file.is_open()) {
            LOG(ERROR) << "Error: Could not open file: " << result_path << " : " << strerror(errno);
            return;
        }
        
        // Format values for CSV output
        auto formatBool = [](bool value) -> std::string {
            return value ? "true" : "false";
        };
        
        auto formatFloat = [](double value) -> std::string {
            if (value == 0.0) return "0";
            std::stringstream ss;
            ss << std::fixed << std::setprecision(4) << value;
            return ss.str();
        };
        
        // Write test results to CSV file
        file << message_size << ","
             << total_message_size << ","
             << num_threads_per_broker << ","
             << ack_level << ","
             << order << ","
             << replication_factor << ","
             << formatBool(replicate_tinode) << ","
             << num_clients << ","
             << num_brokers_to_kill << ","
             << formatFloat(failure_percentage) << ","
             << seq_type << ","
             << formatFloat(pubBandwidthMbps) << ","
             << formatFloat(subBandwidthMbps) << ","
             << formatFloat(e2eBandwidthMbps) << "\n";
        
        file.close();
        
        // Calculate summary for display
        std::vector<std::pair<std::string, double>> results;
        if (pubBandwidthMbps > 0) results.push_back({"Publish", pubBandwidthMbps});
        if (subBandwidthMbps > 0) results.push_back({"Subscribe", subBandwidthMbps});
        if (e2eBandwidthMbps > 0) results.push_back({"End-to-end", e2eBandwidthMbps});
        
        // Log result summary
        LOG(INFO) << "Test results:";
        for (const auto& [name, value] : results) {
            LOG(INFO) << "  " << name << " bandwidth: " << std::fixed << std::setprecision(2) 
                      << value << " MB/s";
        }
        
        LOG(INFO) << "Results written to: " << result_path;
    } catch (const std::exception& e) {
        LOG(ERROR) << "Exception in ResultWriter destructor: " << e.what();
    }
}

void ResultWriter::SetPubResult(double res) {
    pubBandwidthMbps = res;
    LOG(INFO) << "Publish bandwidth: " << std::fixed << std::setprecision(2) << res << " MB/s";
}

void ResultWriter::SetSubResult(double res) {
    subBandwidthMbps = res;
    LOG(INFO) << "Subscribe bandwidth: " << std::fixed << std::setprecision(2) << res << " MB/s";
}

void ResultWriter::SetE2EResult(double res) {
    e2eBandwidthMbps = res;
    LOG(INFO) << "End-to-end bandwidth: " << std::fixed << std::setprecision(2) << res << " MB/s";
}
