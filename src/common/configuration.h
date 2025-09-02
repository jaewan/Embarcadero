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
        ConfigValue<size_t> batch_headers_size{1UL << 16, "EMBARCADERO_BATCH_HEADERS_SIZE"};
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
