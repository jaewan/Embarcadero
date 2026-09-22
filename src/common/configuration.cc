#include "configuration.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <limits>
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
    const char* value = std::getenv(env_var_.c_str());
    if (!value || !*value) return std::nullopt;
    size_t used = 0;
    const int result = std::stoi(value, &used);
    if (used != std::string(value).size()) throw std::invalid_argument("invalid integer environment: " + env_var_);
    return result;
}

template<>
std::optional<size_t> ConfigValue<size_t>::getEnvValue() const {
    const char* value = std::getenv(env_var_.c_str());
    if (!value || !*value) return std::nullopt;
    if (*value == '-') throw std::invalid_argument("negative size environment: " + env_var_);
    size_t used = 0;
    const auto result = std::stoull(value, &used);
    if (used != std::string(value).size() || result > std::numeric_limits<size_t>::max())
        throw std::invalid_argument("invalid size environment: " + env_var_);
    return static_cast<size_t>(result);
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
        throw std::invalid_argument("invalid boolean environment: " + env_var_);
    }
    return std::nullopt;
}

Configuration& Configuration::getInstance() {
    static Configuration instance;
    return instance;
}

std::string Configuration::getRuntimeMode() const {
    std::string mode = config_.client.runtime.mode.get();
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (mode == "throughput" || mode == "failure" || mode == "latency") {
        return mode;
    }
    return "throughput";
}

bool Configuration::loadFromFile(const std::string& filename) {
    std::ifstream input(filename);
    if (!input) { validation_errors_ = {"Cannot open configuration: " + filename}; return false; }
    std::ostringstream content;
    content << input.rdbuf();
    return loadFromString(content.str());
}

bool Configuration::loadFromString(const std::string& yaml_content) {
    if (frozen_) { LOG(ERROR) << "Configuration is frozen"; return false; }
    const auto previous = config_;
    try {
        YAML::Node yaml = YAML::Load(yaml_content);
        if (!yaml.IsMap() || (!yaml["embarcadero"] && !yaml["client"]))
            throw std::invalid_argument("expected an embarcadero or client mapping");
        
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
                if (cxl["emulation_size"]) {
                    config_.cxl.emulation_size.set(cxl["emulation_size"].as<size_t>());
                    LOG(WARNING) << "cxl.emulation_size is deprecated and ignored; cxl.size controls all backends";
                }
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
                if (corfu["sequencer_ip"]) config_.corfu.sequencer_ip.set(corfu["sequencer_ip"].as<std::string>());
            }
            
            // Scalog
            if (root["scalog"]) {
                auto scalog = root["scalog"];
                if (scalog["sequencer_port"]) config_.scalog.sequencer_port.set(scalog["sequencer_port"].as<int>());
                if (scalog["replication_port"]) config_.scalog.replication_port.set(scalog["replication_port"].as<int>());
                if (scalog["sequencer_ip"]) config_.scalog.sequencer_ip.set(scalog["sequencer_ip"].as<std::string>());
                if (scalog["local_cut_interval"]) config_.scalog.local_cut_interval.set(scalog["local_cut_interval"].as<int>());
            }

            // LazyLog
            if (root["lazylog"]) {
                auto lazylog = root["lazylog"];
                if (lazylog["sequencer_port"]) config_.lazylog.sequencer_port.set(lazylog["sequencer_port"].as<int>());
                if (lazylog["replication_port"]) config_.lazylog.replication_port.set(lazylog["replication_port"].as<int>());
                if (lazylog["sequencer_ip"]) config_.lazylog.sequencer_ip.set(lazylog["sequencer_ip"].as<std::string>());
                if (lazylog["local_cut_interval"]) config_.lazylog.local_cut_interval.set(lazylog["local_cut_interval"].as<int>());
            }
            
            // Platform
            if (root["platform"]) {
                auto platform = root["platform"];
                if (platform["is_intel"]) config_.platform.is_intel.set(platform["is_intel"].as<bool>());
                if (platform["is_amd"]) config_.platform.is_amd.set(platform["is_amd"].as<bool>());
            }

            if (root["cluster"]) {
                auto cluster = root["cluster"];
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

                // Runtime
                if (client["runtime"]) {
                    auto runtime = client["runtime"];
                    if (runtime["mode"]) config_.client.runtime.mode.set(runtime["mode"].as<std::string>());
                    if (runtime["ack_drain_ms_throughput"]) config_.client.runtime.ack_drain_ms_throughput.set(runtime["ack_drain_ms_throughput"].as<int>());
                    if (runtime["ack_drain_ms_failure"]) config_.client.runtime.ack_drain_ms_failure.set(runtime["ack_drain_ms_failure"].as<int>());
                    if (runtime["ack_drain_ms_latency"]) config_.client.runtime.ack_drain_ms_latency.set(runtime["ack_drain_ms_latency"].as<int>());
                    if (runtime["socket_send_buffer_bytes_throughput"]) config_.client.runtime.socket_send_buffer_bytes_throughput.set(runtime["socket_send_buffer_bytes_throughput"].as<size_t>());
                    if (runtime["socket_send_buffer_bytes_failure"]) config_.client.runtime.socket_send_buffer_bytes_failure.set(runtime["socket_send_buffer_bytes_failure"].as<size_t>());
                    if (runtime["socket_send_buffer_bytes_latency"]) config_.client.runtime.socket_send_buffer_bytes_latency.set(runtime["socket_send_buffer_bytes_latency"].as<size_t>());
                    if (runtime["socket_recv_buffer_bytes_throughput"]) config_.client.runtime.socket_recv_buffer_bytes_throughput.set(runtime["socket_recv_buffer_bytes_throughput"].as<size_t>());
                    if (runtime["socket_recv_buffer_bytes_failure"]) config_.client.runtime.socket_recv_buffer_bytes_failure.set(runtime["socket_recv_buffer_bytes_failure"].as<size_t>());
                    if (runtime["socket_recv_buffer_bytes_latency"]) config_.client.runtime.socket_recv_buffer_bytes_latency.set(runtime["socket_recv_buffer_bytes_latency"].as<size_t>());
                    if (runtime["tcp_user_timeout_ms_throughput"]) config_.client.runtime.tcp_user_timeout_ms_throughput.set(runtime["tcp_user_timeout_ms_throughput"].as<int>());
                    if (runtime["tcp_user_timeout_ms_failure"]) config_.client.runtime.tcp_user_timeout_ms_failure.set(runtime["tcp_user_timeout_ms_failure"].as<int>());
                    if (runtime["tcp_user_timeout_ms_latency"]) config_.client.runtime.tcp_user_timeout_ms_latency.set(runtime["tcp_user_timeout_ms_latency"].as<int>());
                    if (runtime["header_send_timeout_ms_throughput"]) config_.client.runtime.header_send_timeout_ms_throughput.set(runtime["header_send_timeout_ms_throughput"].as<int>());
                    if (runtime["header_send_timeout_ms_failure"]) config_.client.runtime.header_send_timeout_ms_failure.set(runtime["header_send_timeout_ms_failure"].as<int>());
                    if (runtime["header_send_timeout_ms_latency"]) config_.client.runtime.header_send_timeout_ms_latency.set(runtime["header_send_timeout_ms_latency"].as<int>());
                    if (runtime["session_rto_min_ms_throughput"]) config_.client.runtime.session_rto_min_ms_throughput.set(runtime["session_rto_min_ms_throughput"].as<int>());
                    if (runtime["session_rto_min_ms_failure"]) config_.client.runtime.session_rto_min_ms_failure.set(runtime["session_rto_min_ms_failure"].as<int>());
                    if (runtime["session_rto_min_ms_latency"]) config_.client.runtime.session_rto_min_ms_latency.set(runtime["session_rto_min_ms_latency"].as<int>());
                    if (runtime["ack_timeout_sec_throughput"]) config_.client.runtime.ack_timeout_sec_throughput.set(runtime["ack_timeout_sec_throughput"].as<int>());
                    if (runtime["ack_timeout_sec_failure"]) config_.client.runtime.ack_timeout_sec_failure.set(runtime["ack_timeout_sec_failure"].as<int>());
                    if (runtime["ack_timeout_sec_latency"]) config_.client.runtime.ack_timeout_sec_latency.set(runtime["ack_timeout_sec_latency"].as<int>());
                    if (runtime["epoll_wait_writable_ms_throughput"]) config_.client.runtime.epoll_wait_writable_ms_throughput.set(runtime["epoll_wait_writable_ms_throughput"].as<int>());
                    if (runtime["epoll_wait_writable_ms_failure"]) config_.client.runtime.epoll_wait_writable_ms_failure.set(runtime["epoll_wait_writable_ms_failure"].as<int>());
                    if (runtime["epoll_wait_writable_ms_latency"]) config_.client.runtime.epoll_wait_writable_ms_latency.set(runtime["epoll_wait_writable_ms_latency"].as<int>());
                }
                
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
            if (client["runtime"]) {
                auto runtime = client["runtime"];
                if (runtime["mode"]) config_.client.runtime.mode.set(runtime["mode"].as<std::string>());
                if (runtime["ack_drain_ms_throughput"]) config_.client.runtime.ack_drain_ms_throughput.set(runtime["ack_drain_ms_throughput"].as<int>());
                if (runtime["ack_drain_ms_failure"]) config_.client.runtime.ack_drain_ms_failure.set(runtime["ack_drain_ms_failure"].as<int>());
                if (runtime["ack_drain_ms_latency"]) config_.client.runtime.ack_drain_ms_latency.set(runtime["ack_drain_ms_latency"].as<int>());
                if (runtime["socket_send_buffer_bytes_throughput"]) config_.client.runtime.socket_send_buffer_bytes_throughput.set(runtime["socket_send_buffer_bytes_throughput"].as<size_t>());
                if (runtime["socket_send_buffer_bytes_failure"]) config_.client.runtime.socket_send_buffer_bytes_failure.set(runtime["socket_send_buffer_bytes_failure"].as<size_t>());
                if (runtime["socket_send_buffer_bytes_latency"]) config_.client.runtime.socket_send_buffer_bytes_latency.set(runtime["socket_send_buffer_bytes_latency"].as<size_t>());
                if (runtime["socket_recv_buffer_bytes_throughput"]) config_.client.runtime.socket_recv_buffer_bytes_throughput.set(runtime["socket_recv_buffer_bytes_throughput"].as<size_t>());
                if (runtime["socket_recv_buffer_bytes_failure"]) config_.client.runtime.socket_recv_buffer_bytes_failure.set(runtime["socket_recv_buffer_bytes_failure"].as<size_t>());
                if (runtime["socket_recv_buffer_bytes_latency"]) config_.client.runtime.socket_recv_buffer_bytes_latency.set(runtime["socket_recv_buffer_bytes_latency"].as<size_t>());
                if (runtime["tcp_user_timeout_ms_throughput"]) config_.client.runtime.tcp_user_timeout_ms_throughput.set(runtime["tcp_user_timeout_ms_throughput"].as<int>());
                if (runtime["tcp_user_timeout_ms_failure"]) config_.client.runtime.tcp_user_timeout_ms_failure.set(runtime["tcp_user_timeout_ms_failure"].as<int>());
                if (runtime["tcp_user_timeout_ms_latency"]) config_.client.runtime.tcp_user_timeout_ms_latency.set(runtime["tcp_user_timeout_ms_latency"].as<int>());
                if (runtime["header_send_timeout_ms_throughput"]) config_.client.runtime.header_send_timeout_ms_throughput.set(runtime["header_send_timeout_ms_throughput"].as<int>());
                if (runtime["header_send_timeout_ms_failure"]) config_.client.runtime.header_send_timeout_ms_failure.set(runtime["header_send_timeout_ms_failure"].as<int>());
                if (runtime["header_send_timeout_ms_latency"]) config_.client.runtime.header_send_timeout_ms_latency.set(runtime["header_send_timeout_ms_latency"].as<int>());
                if (runtime["session_rto_min_ms_throughput"]) config_.client.runtime.session_rto_min_ms_throughput.set(runtime["session_rto_min_ms_throughput"].as<int>());
                if (runtime["session_rto_min_ms_failure"]) config_.client.runtime.session_rto_min_ms_failure.set(runtime["session_rto_min_ms_failure"].as<int>());
                if (runtime["session_rto_min_ms_latency"]) config_.client.runtime.session_rto_min_ms_latency.set(runtime["session_rto_min_ms_latency"].as<int>());
                if (runtime["ack_timeout_sec_throughput"]) config_.client.runtime.ack_timeout_sec_throughput.set(runtime["ack_timeout_sec_throughput"].as<int>());
                if (runtime["ack_timeout_sec_failure"]) config_.client.runtime.ack_timeout_sec_failure.set(runtime["ack_timeout_sec_failure"].as<int>());
                if (runtime["ack_timeout_sec_latency"]) config_.client.runtime.ack_timeout_sec_latency.set(runtime["ack_timeout_sec_latency"].as<int>());
                if (runtime["epoll_wait_writable_ms_throughput"]) config_.client.runtime.epoll_wait_writable_ms_throughput.set(runtime["epoll_wait_writable_ms_throughput"].as<int>());
                if (runtime["epoll_wait_writable_ms_failure"]) config_.client.runtime.epoll_wait_writable_ms_failure.set(runtime["epoll_wait_writable_ms_failure"].as<int>());
                if (runtime["epoll_wait_writable_ms_latency"]) config_.client.runtime.epoll_wait_writable_ms_latency.set(runtime["epoll_wait_writable_ms_latency"].as<int>());
            }
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
        
        if (validateConfig()) return true;
        config_ = previous;
        return false;
    } catch (const std::exception& e) {
        config_ = previous;
        validation_errors_ = {e.what()};
        LOG(ERROR) << "Failed to parse configuration: " << e.what();
        return false;
    }
}

void Configuration::overrideFromCommandLine(int argc, char* argv[]) {
    if (frozen_) throw std::logic_error("configuration is frozen");
    // The application owns short flags. Never reinterpret -c (cgroup) as size,
    // or re-load --config after an earlier explicit override.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const auto eq = arg.find('=');
        const auto key = arg.substr(0, eq);
        if (key != "--broker-port" && key != "--heartbeat-interval" &&
            key != "--cxl-size" && key != "--batch-size" &&
            key != "--network-threads" && key != "--network_threads" && key != "--max-topics") continue;
        std::string value;
        if (eq != std::string::npos) value = arg.substr(eq + 1);
        else if (i + 1 < argc) value = argv[++i];
        else throw std::invalid_argument("missing value for " + key);
        size_t used = 0;
        if (value.empty() || value.front() == '-') throw std::invalid_argument("invalid value for " + key);
        const auto n = std::stoull(value, &used);
        if (used != value.size()) throw std::invalid_argument("invalid value for " + key);
        if (key == "--cxl-size") config_.cxl.size.setOverride(n);
        else if (key == "--batch-size") config_.storage.batch_size.setOverride(n);
        else {
            if (n > static_cast<unsigned long long>(std::numeric_limits<int>::max()))
                throw std::invalid_argument("value out of range for " + key);
            if (key == "--broker-port") config_.broker.port.setOverride(n);
            else if (key == "--heartbeat-interval") config_.broker.heartbeat_interval.setOverride(n);
            else if (key == "--max-topics") config_.storage.max_topics.setOverride(n);
            else config_.network.io_threads.setOverride(n);
        }
    }
}

bool Configuration::finalize() {
    if (frozen_) return true;
    if (!validate()) return false;
    config_.broker.broker_port.freeze();
    config_.broker.cgroup_core.freeze();
    config_.broker.heartbeat_interval.freeze();
    config_.broker.max_brokers.freeze();
    config_.broker.port.freeze();
    config_.client.network.connect_timeout_ms.freeze();
    config_.client.network.recv_timeout_ms.freeze();
    config_.client.network.send_timeout_ms.freeze();
    config_.client.performance.enable_publisher_pipeline_profile.freeze();
    config_.client.performance.numa_bind.freeze();
    config_.client.performance.use_hugepages.freeze();
    config_.client.performance.zero_copy.freeze();
    config_.client.publisher.batch_size_kb.freeze();
    config_.client.publisher.buffer_size_mb.freeze();
    config_.client.publisher.threads_per_broker.freeze();
    config_.client.runtime.ack_drain_ms_failure.freeze();
    config_.client.runtime.ack_drain_ms_latency.freeze();
    config_.client.runtime.ack_drain_ms_throughput.freeze();
    config_.client.runtime.ack_timeout_sec_failure.freeze();
    config_.client.runtime.ack_timeout_sec_latency.freeze();
    config_.client.runtime.ack_timeout_sec_throughput.freeze();
    config_.client.runtime.epoll_wait_writable_ms_failure.freeze();
    config_.client.runtime.epoll_wait_writable_ms_latency.freeze();
    config_.client.runtime.epoll_wait_writable_ms_throughput.freeze();
    config_.client.runtime.header_send_timeout_ms_failure.freeze();
    config_.client.runtime.header_send_timeout_ms_latency.freeze();
    config_.client.runtime.header_send_timeout_ms_throughput.freeze();
    config_.client.runtime.mode.freeze();
    config_.client.runtime.session_rto_min_ms_failure.freeze();
    config_.client.runtime.session_rto_min_ms_latency.freeze();
    config_.client.runtime.session_rto_min_ms_throughput.freeze();
    config_.client.runtime.socket_recv_buffer_bytes_failure.freeze();
    config_.client.runtime.socket_recv_buffer_bytes_latency.freeze();
    config_.client.runtime.socket_recv_buffer_bytes_throughput.freeze();
    config_.client.runtime.socket_send_buffer_bytes_failure.freeze();
    config_.client.runtime.socket_send_buffer_bytes_latency.freeze();
    config_.client.runtime.socket_send_buffer_bytes_throughput.freeze();
    config_.client.runtime.tcp_user_timeout_ms_failure.freeze();
    config_.client.runtime.tcp_user_timeout_ms_latency.freeze();
    config_.client.runtime.tcp_user_timeout_ms_throughput.freeze();
    config_.client.subscriber.buffer_size_mb.freeze();
    config_.client.subscriber.connections_per_broker.freeze();
    config_.cluster.sequencer_broker_id.freeze();
    config_.corfu.replication_port.freeze();
    config_.corfu.sequencer_ip.freeze();
    config_.corfu.sequencer_port.freeze();
    config_.cxl.device_path.freeze();
    config_.cxl.emulation_size.freeze();
    config_.cxl.numa_node.freeze();
    config_.cxl.size.freeze();
    config_.lazylog.local_cut_interval.freeze();
    config_.lazylog.replication_port.freeze();
    config_.lazylog.sequencer_ip.freeze();
    config_.lazylog.sequencer_port.freeze();
    config_.network.disk_io_threads.freeze();
    config_.network.enable_publish_pipeline_profile.freeze();
    config_.network.io_threads.freeze();
    config_.network.pbr_high_watermark_pct.freeze();
    config_.network.pbr_low_watermark_pct.freeze();
    config_.network.sub_connections.freeze();
    config_.network.zero_copy_send_limit.freeze();
    config_.platform.is_amd.freeze();
    config_.platform.is_intel.freeze();
    config_.scalog.local_cut_interval.freeze();
    config_.scalog.replication_port.freeze();
    config_.scalog.sequencer_ip.freeze();
    config_.scalog.sequencer_port.freeze();
    config_.storage.batch_headers_size.freeze();
    config_.storage.batch_size.freeze();
    config_.storage.max_topics.freeze();
    config_.storage.num_disks.freeze();
    config_.storage.segment_size.freeze();
    config_.storage.topic_name_size.freeze();
    config_.version.major.freeze();
    config_.version.minor.freeze();
    frozen_ = true;
    return true;
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
    
    const auto segment = config_.storage.segment_size.get();
    const auto batch = config_.storage.batch_size.get();
    const auto ring = config_.storage.batch_headers_size.get();
    if (segment <= 4096 || segment % 64 != 0 || batch == 0 || batch > segment - std::min(segment, size_t{4096}))
        validation_errors_.push_back("Segment must be 64-byte aligned with room for its initial 4096-byte prefix and a nonzero batch");
    if (ring < 2 * 128 || ring % 128 != 0)
        validation_errors_.push_back("PBR must contain at least two complete 128-byte entries");
    if (config_.broker.max_brokers.get() < 1 || config_.broker.max_brokers.get() > 32)
        validation_errors_.push_back("max_brokers must be in [1,32]");
    if (config_.cluster.sequencer_broker_id.get() < 0 ||
        config_.cluster.sequencer_broker_id.get() >= config_.broker.max_brokers.get())
        validation_errors_.push_back("sequencer_broker_id must be a configured broker");
    if (config_.broker.broker_port.get() < 1024 || config_.broker.broker_port.get() > 65535 ||
        config_.broker.port.get() > 65535 - config_.broker.max_brokers.get() + 1)
        validation_errors_.push_back("Configured broker ports are out of range");
    if (config_.storage.num_disks.get() < 1 || config_.broker.heartbeat_interval.get() < 1)
        validation_errors_.push_back("Disk count and heartbeat interval must be positive");
    const auto high = config_.network.pbr_high_watermark_pct.get();
    const auto low = config_.network.pbr_low_watermark_pct.get();
    if (low < 0 || low >= high || high > 100)
        validation_errors_.push_back("PBR watermarks require 0 <= low < high <= 100");

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
    
    std::vector<int> ids = config_.cluster.data_broker_ids;
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end() ||
        std::any_of(ids.begin(), ids.end(), [&](int id) { return id < 0 || id >= config_.broker.max_brokers.get(); }))
        validation_errors_.push_back("data_broker_ids must be distinct configured broker ids");

    // Platform validation
    if (config_.platform.is_intel.get() && config_.platform.is_amd.get()) {
        validation_errors_.push_back("Cannot be both Intel and AMD platform");
    }

    // Runtime mode validation
    if (getRuntimeMode() != config_.client.runtime.mode.get()) {
        std::string mode = config_.client.runtime.mode.get();
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (mode != "throughput" && mode != "failure" && mode != "latency") {
            validation_errors_.push_back("client.runtime.mode must be one of: throughput, failure, latency");
        }
    }

    // Runtime socket/tcp values must be positive (or non-negative for timeout).
    if (config_.client.runtime.socket_send_buffer_bytes_throughput.get() == 0 ||
        config_.client.runtime.socket_send_buffer_bytes_failure.get() == 0 ||
        config_.client.runtime.socket_send_buffer_bytes_latency.get() == 0 ||
        config_.client.runtime.socket_recv_buffer_bytes_throughput.get() == 0 ||
        config_.client.runtime.socket_recv_buffer_bytes_failure.get() == 0 ||
        config_.client.runtime.socket_recv_buffer_bytes_latency.get() == 0) {
        validation_errors_.push_back("client.runtime socket buffer bytes must be > 0");
    }
    if (config_.client.runtime.tcp_user_timeout_ms_throughput.get() < 0 ||
        config_.client.runtime.tcp_user_timeout_ms_failure.get() < 0 ||
        config_.client.runtime.tcp_user_timeout_ms_latency.get() < 0) {
        validation_errors_.push_back("client.runtime tcp_user_timeout_ms_* must be >= 0");
    }
    if (config_.client.runtime.header_send_timeout_ms_throughput.get() <= 0 ||
        config_.client.runtime.header_send_timeout_ms_throughput.get() > 600000 ||
        config_.client.runtime.header_send_timeout_ms_failure.get() <= 0 ||
        config_.client.runtime.header_send_timeout_ms_failure.get() > 600000 ||
        config_.client.runtime.header_send_timeout_ms_latency.get() <= 0 ||
        config_.client.runtime.header_send_timeout_ms_latency.get() > 600000) {
        validation_errors_.push_back("client.runtime header_send_timeout_ms_* must be in [1, 600000]");
    }
    if (config_.client.runtime.session_rto_min_ms_throughput.get() < 2 ||
        config_.client.runtime.session_rto_min_ms_throughput.get() > 60000 ||
        config_.client.runtime.session_rto_min_ms_failure.get() < 2 ||
        config_.client.runtime.session_rto_min_ms_failure.get() > 60000 ||
        config_.client.runtime.session_rto_min_ms_latency.get() < 2 ||
        config_.client.runtime.session_rto_min_ms_latency.get() > 60000) {
        validation_errors_.push_back("client.runtime session_rto_min_ms_* must be in [2, 60000]");
    }
    if (config_.client.runtime.ack_timeout_sec_throughput.get() < 0 ||
        config_.client.runtime.ack_timeout_sec_failure.get() < 0 ||
        config_.client.runtime.ack_timeout_sec_latency.get() < 0) {
        validation_errors_.push_back("client.runtime ack_timeout_sec_* must be >= 0");
    }
    if (config_.client.runtime.epoll_wait_writable_ms_throughput.get() < 0 ||
        config_.client.runtime.epoll_wait_writable_ms_failure.get() < 0 ||
        config_.client.runtime.epoll_wait_writable_ms_latency.get() < 0) {
        validation_errors_.push_back("client.runtime epoll_wait_writable_ms_* must be >= 0");
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
