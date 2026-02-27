#ifndef EMBARCADERO_CONFIGURATION_H_
#define EMBARCADERO_CONFIGURATION_H_

#include <string>
#include <memory>
#include <optional>
#include <variant>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace Embarcadero {

/**
 * Configuration value that can be overridden by environment variables
 */
template<typename T>
class ConfigValue {
public:
    ConfigValue() = default;
    ConfigValue(T default_value, const std::string& env_var = "")
        : value_(default_value), env_var_(env_var) {}

    T get() const {
        if (!env_var_.empty()) {
            auto env_value = getEnvValue();
            if (env_value.has_value()) {
                return env_value.value();
            }
        }
        return value_;
    }

    void set(T value) { value_ = value; }
    const std::string& env_var() const { return env_var_; }

private:
    T value_;
    std::string env_var_;

    std::optional<T> getEnvValue() const;
};

/**
 * Main configuration structure
 */
struct EmbarcaderoConfig {
    // Version information
    struct Version {
        ConfigValue<int> major{1, "EMBARCADERO_VERSION_MAJOR"};
        ConfigValue<int> minor{0, "EMBARCADERO_VERSION_MINOR"};
    } version;

    // Broker configuration
    struct Broker {
        ConfigValue<int> port{1214, "EMBARCADERO_BROKER_PORT"};
        ConfigValue<int> broker_port{12140, "EMBARCADERO_BROKER_PORT_ALT"};
        ConfigValue<int> heartbeat_interval{3, "EMBARCADERO_HEARTBEAT_INTERVAL"};
        ConfigValue<int> max_brokers{4, "EMBARCADERO_MAX_BROKERS"};
        ConfigValue<int> cgroup_core{85, "EMBARCADERO_CGROUP_CORE"};
    } broker;

    // CXL memory configuration
    struct CXL {
        ConfigValue<size_t> size{1UL << 35, "EMBARCADERO_CXL_SIZE"};
        ConfigValue<size_t> emulation_size{1UL << 35, "EMBARCADERO_CXL_EMUL_SIZE"};
        ConfigValue<std::string> device_path{"/dev/dax0.0", "EMBARCADERO_CXL_DEVICE"};
        ConfigValue<int> numa_node{2, "EMBARCADERO_CXL_NUMA_NODE"};
    } cxl;

    // Storage configuration
    struct Storage {
        ConfigValue<size_t> segment_size{1UL << 34, "EMBARCADERO_SEGMENT_SIZE"};
        // 10MB = 81,920 slots (128B each); holds 10GB/4 brokers (~2.5GB each) at ~2MB batch size. 64KB was too small → PBR full → ACK timeout.
        ConfigValue<size_t> batch_headers_size{10UL * 1024 * 1024, "EMBARCADERO_BATCH_HEADERS_SIZE"};
        ConfigValue<size_t> batch_size{1UL << 19, "EMBARCADERO_BATCH_SIZE"};
        ConfigValue<int> num_disks{2, "EMBARCADERO_NUM_DISKS"};
        ConfigValue<int> max_topics{32, "EMBARCADERO_MAX_TOPICS"};
        ConfigValue<int> topic_name_size{31, "EMBARCADERO_TOPIC_NAME_SIZE"};
    } storage;

    // Network configuration
    struct Network {
        ConfigValue<int> io_threads{8, "EMBARCADERO_NETWORK_IO_THREADS"};
        ConfigValue<int> disk_io_threads{4, "EMBARCADERO_DISK_IO_THREADS"};
        ConfigValue<int> sub_connections{3, "EMBARCADERO_SUB_CONNECTIONS"};
        ConfigValue<size_t> zero_copy_send_limit{1UL << 23, "EMBARCADERO_ZERO_COPY_LIMIT"};

        // PBR (batch header ring) backpressure: stop reading from TCP when utilization above high, resume when below low.
        ConfigValue<int> pbr_high_watermark_pct{80, "EMBARCADERO_PBR_HIGH_WATERMARK_PCT"};
        ConfigValue<int> pbr_low_watermark_pct{50, "EMBARCADERO_PBR_LOW_WATERMARK_PCT"};
        // Publish pipeline profile (DrainPayloadToBuffer, etc.). When false, no timing/RecordProfile in hot path (higher throughput for benchmarks).
        ConfigValue<bool> enable_publish_pipeline_profile{true, "EMBARCADERO_ENABLE_PUBLISH_PIPELINE_PROFILE"};
    } network;

    // Corfu configuration
    struct Corfu {
        ConfigValue<int> sequencer_port{50052, "EMBARCADERO_CORFU_SEQ_PORT"};
        ConfigValue<int> replication_port{50053, "EMBARCADERO_CORFU_REP_PORT"};
    } corfu;

    // Scalog configuration
    struct Scalog {
        ConfigValue<int> sequencer_port{50051, "EMBARCADERO_SCALOG_SEQ_PORT"};
        ConfigValue<int> replication_port{50052, "EMBARCADERO_SCALOG_REP_PORT"};
        ConfigValue<std::string> sequencer_ip{"192.168.60.173", "EMBARCADERO_SCALOG_SEQ_IP"};
        ConfigValue<int> local_cut_interval{100, "EMBARCADERO_SCALOG_CUT_INTERVAL"};
    } scalog;

    // Platform detection
    struct Platform {
        ConfigValue<bool> is_intel{false, "EMBARCADERO_PLATFORM_INTEL"};
        ConfigValue<bool> is_amd{false, "EMBARCADERO_PLATFORM_AMD"};
    } platform;

    struct Cluster {
        ConfigValue<int> sequencer_broker_id{0, "EMBARCADERO_SEQUENCER_BROKER_ID"};
        std::vector<int> data_broker_ids;
    } cluster;

    // Client configuration
    struct Client {
        struct Runtime {
            // Supported modes: throughput, failure, latency
            ConfigValue<std::string> mode{"throughput", "EMBARCADERO_RUNTIME_MODE"};
            // Default ACK drain after successful publish completion in each mode.
            ConfigValue<int> ack_drain_ms_throughput{50, "EMBARCADERO_ACK_DRAIN_MS_THROUGHPUT"};
            ConfigValue<int> ack_drain_ms_failure{3000, "EMBARCADERO_ACK_DRAIN_MS_FAILURE"};
            ConfigValue<int> ack_drain_ms_latency{50, "EMBARCADERO_ACK_DRAIN_MS_LATENCY"};
        } runtime;

        struct Publisher {
            ConfigValue<int> threads_per_broker{4, "EMBARCADERO_CLIENT_PUB_THREADS"};
            ConfigValue<size_t> buffer_size_mb{768, "EMBARCADERO_CLIENT_PUB_BUFFER_MB"};
            ConfigValue<size_t> batch_size_kb{2048, "EMBARCADERO_CLIENT_PUB_BATCH_KB"};
        } publisher;

        struct Subscriber {
            ConfigValue<int> connections_per_broker{3, "EMBARCADERO_CLIENT_SUB_CONNECTIONS"};
            ConfigValue<size_t> buffer_size_mb{256, "EMBARCADERO_CLIENT_SUB_BUFFER_MB"};
        } subscriber;

        struct Network {
            ConfigValue<int> connect_timeout_ms{2000, "EMBARCADERO_CLIENT_CONNECT_TIMEOUT"};
            ConfigValue<int> send_timeout_ms{5000, "EMBARCADERO_CLIENT_SEND_TIMEOUT"};
            ConfigValue<int> recv_timeout_ms{5000, "EMBARCADERO_CLIENT_RECV_TIMEOUT"};
        } network;

        struct Performance {
            ConfigValue<bool> use_hugepages{true, "EMBARCADERO_CLIENT_USE_HUGEPAGES"};
            ConfigValue<bool> numa_bind{true, "EMBARCADERO_CLIENT_NUMA_BIND"};
            ConfigValue<bool> zero_copy{true, "EMBARCADERO_CLIENT_ZERO_COPY"};
            /** Lightweight publisher pipeline profile (buffer write + send). Default false to avoid overhead. */
            ConfigValue<bool> enable_publisher_pipeline_profile{false, "EMBARCADERO_ENABLE_PUBLISHER_PIPELINE_PROFILE"};
        } performance;
    } client;
};

/**
 * Configuration manager singleton
 */
class Configuration {
public:
    static Configuration& getInstance();

    // Load configuration from file
    bool loadFromFile(const std::string& filename);
    
    // Load configuration from YAML string
    bool loadFromString(const std::string& yaml_content);
    
    // Override with command line arguments
    void overrideFromCommandLine(int argc, char* argv[]);
    
    // Get the configuration
    const EmbarcaderoConfig& config() const { return config_; }
    EmbarcaderoConfig& config() { return config_; }

    // Helper methods for common access patterns
    int getBrokerPort() const { return config_.broker.port.get(); }
    size_t getCXLSize() const { return config_.cxl.size.get(); }
    size_t getBatchSize() const { return config_.storage.batch_size.get(); }
    int getMaxTopics() const { return config_.storage.max_topics.get(); }
    int getNetworkIOThreads() const { return config_.network.io_threads.get(); }

    // Cluster configuration helpers
    int getSequencerBrokerId() const { return config_.cluster.sequencer_broker_id.get(); }
    std::vector<int> getDataBrokerIds() const {
        return config_.cluster.data_broker_ids;
    }

    // Validation
    bool validate() const;
    std::vector<std::string> getValidationErrors() const;

private:
    Configuration() = default;
    Configuration(const Configuration&) = delete;
    Configuration& operator=(const Configuration&) = delete;

    EmbarcaderoConfig config_;
    mutable std::vector<std::string> validation_errors_;

    // Helper methods for parsing
    void parseYAMLNode(const std::string& key, const void* node);
    bool validateConfig();
};

// Template specializations for getEnvValue
template<>
std::optional<int> ConfigValue<int>::getEnvValue() const;

template<>
std::optional<size_t> ConfigValue<size_t>::getEnvValue() const;

template<>
std::optional<std::string> ConfigValue<std::string>::getEnvValue() const;

template<>
std::optional<bool> ConfigValue<bool>::getEnvValue() const;

} // namespace Embarcadero

#endif // EMBARCADERO_CONFIGURATION_H_
