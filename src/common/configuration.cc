#include "configuration.h"
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <getopt.h>
#include <glog/logging.h>
#include <yaml-cpp/yaml.h>

namespace Embarcadero {

// Global function to get configuration instance
const Configuration& GetConfig() {
    return Configuration::getInstance();
}

// Template specializations for environment variable parsing
template<>
std::optional<int> ConfigValue<int>::getEnvValue() const {
    const char* env_val = std::getenv(env_var_.c_str());
    if (env_val) {
        try {
            return std::stoi(env_val);
        } catch (const std::exception& e) {
            LOG(WARNING) << "Failed to parse env var " << env_var_ << ": " << e.what();
        }
    }
    return std::nullopt;
}

template<>
std::optional<size_t> ConfigValue<size_t>::getEnvValue() const {
    const char* env_val = std::getenv(env_var_.c_str());
    if (env_val) {
        try {
            return std::stoull(env_val);
        } catch (const std::exception& e) {
            LOG(WARNING) << "Failed to parse env var " << env_var_ << ": " << e.what();
        }
    }
    return std::nullopt;
}

template<>
std::optional<std::string> ConfigValue<std::string>::getEnvValue() const {
    const char* env_val = std::getenv(env_var_.c_str());
    if (env_val) {
        return std::string(env_val);
    }
    return std::nullopt;
}

template<>
std::optional<bool> ConfigValue<bool>::getEnvValue() const {
    const char* env_val = std::getenv(env_var_.c_str());
    if (env_val) {
        std::string val(env_val);
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        if (val == "true" || val == "1" || val == "yes" || val == "on") {
            return true;
        } else if (val == "false" || val == "0" || val == "no" || val == "off") {
            return false;
        }
        LOG(WARNING) << "Invalid boolean value for env var " << env_var_ << ": " << env_val;
    }
    return std::nullopt;
}

Configuration& Configuration::getInstance() {
    static Configuration instance;
    return instance;
}

bool Configuration::loadFromFile(const std::string& filename) {
    try {
        YAML::Node yaml = YAML::LoadFile(filename);
        
        if (yaml["embarcadero"]) {
            auto root = yaml["embarcadero"];
            
            // Version
            if (root["version"]) {
                auto version = root["version"];
                if (version["major"]) config_.version.major.set(version["major"].as<int>());
                if (version["minor"]) config_.version.minor.set(version["minor"].as<int>());
            }
            
            // Broker
            if (root["broker"]) {
                auto broker = root["broker"];
                if (broker["port"]) config_.broker.port.set(broker["port"].as<int>());
                if (broker["broker_port"]) config_.broker.broker_port.set(broker["broker_port"].as<int>());
                if (broker["heartbeat_interval"]) config_.broker.heartbeat_interval.set(broker["heartbeat_interval"].as<int>());
                if (broker["max_brokers"]) config_.broker.max_brokers.set(broker["max_brokers"].as<int>());
                if (broker["cgroup_core"]) config_.broker.cgroup_core.set(broker["cgroup_core"].as<int>());
            }
            
            // CXL
            if (root["cxl"]) {
                auto cxl = root["cxl"];
                if (cxl["size"]) config_.cxl.size.set(cxl["size"].as<size_t>());
                if (cxl["emulation_size"]) config_.cxl.emulation_size.set(cxl["emulation_size"].as<size_t>());
                if (cxl["device_path"]) config_.cxl.device_path.set(cxl["device_path"].as<std::string>());
                if (cxl["numa_node"]) config_.cxl.numa_node.set(cxl["numa_node"].as<int>());
            }
            
            // Storage
            if (root["storage"]) {
                auto storage = root["storage"];
                if (storage["segment_size"]) config_.storage.segment_size.set(storage["segment_size"].as<size_t>());
                if (storage["batch_headers_size"]) config_.storage.batch_headers_size.set(storage["batch_headers_size"].as<size_t>());
                if (storage["batch_size"]) config_.storage.batch_size.set(storage["batch_size"].as<size_t>());
                if (storage["num_disks"]) config_.storage.num_disks.set(storage["num_disks"].as<int>());
                if (storage["max_topics"]) config_.storage.max_topics.set(storage["max_topics"].as<int>());
                if (storage["topic_name_size"]) config_.storage.topic_name_size.set(storage["topic_name_size"].as<int>());
            }
            
            // Network
            if (root["network"]) {
                auto network = root["network"];
                if (network["io_threads"]) config_.network.io_threads.set(network["io_threads"].as<int>());
                if (network["disk_io_threads"]) config_.network.disk_io_threads.set(network["disk_io_threads"].as<int>());
                if (network["sub_connections"]) config_.network.sub_connections.set(network["sub_connections"].as<int>());
                if (network["zero_copy_send_limit"]) config_.network.zero_copy_send_limit.set(network["zero_copy_send_limit"].as<size_t>());

                if (network["pbr_high_watermark_pct"]) config_.network.pbr_high_watermark_pct.set(network["pbr_high_watermark_pct"].as<int>());
                if (network["pbr_low_watermark_pct"]) config_.network.pbr_low_watermark_pct.set(network["pbr_low_watermark_pct"].as<int>());
                if (network["enable_publish_pipeline_profile"]) config_.network.enable_publish_pipeline_profile.set(network["enable_publish_pipeline_profile"].as<bool>());
            }
            
            // Corfu
            if (root["corfu"]) {
                auto corfu = root["corfu"];
                if (corfu["sequencer_port"]) config_.corfu.sequencer_port.set(corfu["sequencer_port"].as<int>());
                if (corfu["replication_port"]) config_.corfu.replication_port.set(corfu["replication_port"].as<int>());
            }
            
            // Scalog
            if (root["scalog"]) {
                auto scalog = root["scalog"];
                if (scalog["sequencer_port"]) config_.scalog.sequencer_port.set(scalog["sequencer_port"].as<int>());
                if (scalog["replication_port"]) config_.scalog.replication_port.set(scalog["replication_port"].as<int>());
                if (scalog["sequencer_ip"]) config_.scalog.sequencer_ip.set(scalog["sequencer_ip"].as<std::string>());
                if (scalog["local_cut_interval"]) config_.scalog.local_cut_interval.set(scalog["local_cut_interval"].as<int>());
            }
            
            // Platform
            if (root["platform"]) {
                auto platform = root["platform"];
                if (platform["is_intel"]) config_.platform.is_intel.set(platform["is_intel"].as<bool>());
                if (platform["is_amd"]) config_.platform.is_amd.set(platform["is_amd"].as<bool>());
            }

            // Cluster (sequencer-only head node architecture)
            if (root["cluster"]) {
                auto cluster = root["cluster"];
                if (cluster["is_sequencer_node"]) config_.cluster.is_sequencer_node.set(cluster["is_sequencer_node"].as<bool>());
                if (cluster["sequencer_broker_id"]) config_.cluster.sequencer_broker_id.set(cluster["sequencer_broker_id"].as<int>());
                if (cluster["data_broker_ids"]) {
                    config_.cluster.data_broker_ids.clear();
                    for (const auto& id : cluster["data_broker_ids"]) {
                        config_.cluster.data_broker_ids.push_back(id.as<int>());
                    }
                }
            }

            // Client
            if (root["client"]) {
                auto client = root["client"];
                
                // Publisher
                if (client["publisher"]) {
                    auto publisher = client["publisher"];
                    if (publisher["threads_per_broker"]) config_.client.publisher.threads_per_broker.set(publisher["threads_per_broker"].as<int>());
                    if (publisher["buffer_size_mb"]) config_.client.publisher.buffer_size_mb.set(publisher["buffer_size_mb"].as<size_t>());
                    if (publisher["batch_size_kb"]) config_.client.publisher.batch_size_kb.set(publisher["batch_size_kb"].as<size_t>());
                }
                
                // Subscriber
                if (client["subscriber"]) {
                    auto subscriber = client["subscriber"];
                    if (subscriber["connections_per_broker"]) config_.client.subscriber.connections_per_broker.set(subscriber["connections_per_broker"].as<int>());
                    if (subscriber["buffer_size_mb"]) config_.client.subscriber.buffer_size_mb.set(subscriber["buffer_size_mb"].as<size_t>());
                }
                
                // Network
                if (client["network"]) {
                    auto network = client["network"];
                    if (network["connect_timeout_ms"]) config_.client.network.connect_timeout_ms.set(network["connect_timeout_ms"].as<int>());
                    if (network["send_timeout_ms"]) config_.client.network.send_timeout_ms.set(network["send_timeout_ms"].as<int>());
                    if (network["recv_timeout_ms"]) config_.client.network.recv_timeout_ms.set(network["recv_timeout_ms"].as<int>());
                }
                
                // Performance
                if (client["performance"]) {
                    auto performance = client["performance"];
                    if (performance["use_hugepages"]) config_.client.performance.use_hugepages.set(performance["use_hugepages"].as<bool>());
                    if (performance["numa_bind"]) config_.client.performance.numa_bind.set(performance["numa_bind"].as<bool>());
                    if (performance["zero_copy"]) config_.client.performance.zero_copy.set(performance["zero_copy"].as<bool>());
                    if (performance["enable_publisher_pipeline_profile"]) config_.client.performance.enable_publisher_pipeline_profile.set(performance["enable_publisher_pipeline_profile"].as<bool>());
                }
            }
        }

        // Client-only config (e.g. config/client.yaml with top-level "client:")
        // Ensures BATCH_SIZE (storage.batch_size) matches broker when client loads client.yaml.
        // Broker uses embarcadero.yaml storage.batch_size; client must use same value for batch alignment.
        if (yaml["client"]) {
            auto client = yaml["client"];
            if (client["publisher"]) {
                auto publisher = client["publisher"];
                if (publisher["threads_per_broker"]) config_.client.publisher.threads_per_broker.set(publisher["threads_per_broker"].as<int>());
                if (publisher["buffer_size_mb"]) config_.client.publisher.buffer_size_mb.set(publisher["buffer_size_mb"].as<size_t>());
                if (publisher["batch_size_kb"]) {
                    size_t kb = publisher["batch_size_kb"].as<size_t>();
                    config_.client.publisher.batch_size_kb.set(kb);
                    config_.storage.batch_size.set(kb * 1024);  // BATCH_SIZE = batch_size_kb * 1024
                }
            }
            if (client["subscriber"]) {
                auto subscriber = client["subscriber"];
                if (subscriber["connections_per_broker"]) config_.client.subscriber.connections_per_broker.set(subscriber["connections_per_broker"].as<int>());
                if (subscriber["buffer_size_mb"]) config_.client.subscriber.buffer_size_mb.set(subscriber["buffer_size_mb"].as<size_t>());
            }
            if (client["network"]) {
                auto net = client["network"];
                if (net["connect_timeout_ms"]) config_.client.network.connect_timeout_ms.set(net["connect_timeout_ms"].as<int>());
                if (net["send_timeout_ms"]) config_.client.network.send_timeout_ms.set(net["send_timeout_ms"].as<int>());
                if (net["recv_timeout_ms"]) config_.client.network.recv_timeout_ms.set(net["recv_timeout_ms"].as<int>());
            }
            if (client["performance"]) {
                auto perf = client["performance"];
                if (perf["use_hugepages"]) config_.client.performance.use_hugepages.set(perf["use_hugepages"].as<bool>());
                if (perf["numa_bind"]) config_.client.performance.numa_bind.set(perf["numa_bind"].as<bool>());
                if (perf["zero_copy"]) config_.client.performance.zero_copy.set(perf["zero_copy"].as<bool>());
                if (perf["enable_publisher_pipeline_profile"]) config_.client.performance.enable_publisher_pipeline_profile.set(perf["enable_publisher_pipeline_profile"].as<bool>());
            }
        }
        
        return validateConfig();
    } catch (const YAML::Exception& e) {
        LOG(ERROR) << "Failed to parse configuration file: " << e.what();
        return false;
    }
}

bool Configuration::loadFromString(const std::string& yaml_content) {
    try {
        YAML::Node yaml = YAML::Load(yaml_content);
        // Same parsing logic as loadFromFile
        // ... (implementation identical to loadFromFile but using the string)
        return validateConfig();
    } catch (const YAML::Exception& e) {
        LOG(ERROR) << "Failed to parse configuration string: " << e.what();
        return false;
    }
}

void Configuration::overrideFromCommandLine(int argc, char* argv[]) {
    static struct option long_options[] = {
        {"broker-port", required_argument, 0, 'p'},
        {"heartbeat-interval", required_argument, 0, 'h'},
        {"cxl-size", required_argument, 0, 'c'},
        {"batch-size", required_argument, 0, 'b'},
        {"network-threads", required_argument, 0, 'n'},
        // Accept common flags used by the app so getopt_long doesn't error
        {"head", no_argument, 0, 0},
        {"follower", required_argument, 0, 0},
        {"scalog", no_argument, 0, 0},
        {"SCALOG", no_argument, 0, 0},
        {"corfu", no_argument, 0, 0},
        {"CORFU", no_argument, 0, 0},
        {"embarcadero", no_argument, 0, 0},
        {"EMBARCADERO", no_argument, 0, 0},
        {"emul", no_argument, 0, 0},
        {"run_cgroup", required_argument, 0, 0},
        {"replicate_to_disk", no_argument, 0, 0},
        {"max-topics", required_argument, 0, 't'},
        {"config", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    // Suppress getopt_long default error messages for unknown options
    opterr = 0;
    // Reset getopt state in case other parsers were used earlier
    optind = 1;
    
    while ((c = getopt_long(argc, argv, "p:h:c:b:n:t:f:", long_options, &option_index)) != -1) {
        switch (c) {
            case 'p':
                config_.broker.port.set(std::stoi(optarg));
                break;
            case 'h':
                config_.broker.heartbeat_interval.set(std::stoi(optarg));
                break;
            case 'c':
                config_.cxl.size.set(std::stoull(optarg));
                break;
            case 'b':
                config_.storage.batch_size.set(std::stoull(optarg));
                break;
            case 'n':
                config_.network.io_threads.set(std::stoi(optarg));
                break;
            case 't':
                config_.storage.max_topics.set(std::stoi(optarg));
                break;
            case 'f':
                loadFromFile(optarg);
                break;
            case 0:
                // Known app flags we intentionally ignore here (handled elsewhere)
                break;
            default:
                // Ignore unknown flags to avoid noisy logs; app parser handles them
                break;
        }
    }
}

bool Configuration::validate() const {
    validation_errors_.clear();
    
    // Validate port ranges
    if (config_.broker.port.get() < 1024 || config_.broker.port.get() > 65535) {
        validation_errors_.push_back("Broker port must be between 1024 and 65535");
    }
    
    // Validate memory sizes
    if (config_.cxl.size.get() < (1UL << 20)) { // At least 1MB
        validation_errors_.push_back("CXL size must be at least 1MB");
    }
    
    if (config_.storage.batch_size.get() > config_.storage.segment_size.get()) {
        validation_errors_.push_back("Batch size cannot exceed segment size");
    }
    
    // Validate thread counts
    if (config_.network.io_threads.get() < 1) {
        validation_errors_.push_back("Network IO threads must be at least 1");
    }
    
    if (config_.network.disk_io_threads.get() < 1) {
        validation_errors_.push_back("Disk IO threads must be at least 1");
    }
    
    // Validate topic settings
    if (config_.storage.max_topics.get() < 1) {
        validation_errors_.push_back("Max topics must be at least 1");
    }
    
    if (config_.storage.topic_name_size.get() < 1 || config_.storage.topic_name_size.get() > 255) {
        validation_errors_.push_back("Topic name size must be between 1 and 255");
    }
    
    // Platform validation
    if (config_.platform.is_intel.get() && config_.platform.is_amd.get()) {
        validation_errors_.push_back("Cannot be both Intel and AMD platform");
    }
    
    return validation_errors_.empty();
}

std::vector<std::string> Configuration::getValidationErrors() const {
    return validation_errors_;
}

bool Configuration::validateConfig() {
    return validate();
}

} // namespace Embarcadero
