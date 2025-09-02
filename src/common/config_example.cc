#include "configuration.h"
#include <iostream>
#include <glog/logging.h>

using namespace Embarcadero;

int main(int argc, char* argv[]) {
    // Initialize Google logging
    google::InitGoogleLogging(argv[0]);
    
    // Get configuration instance
    Configuration& config = Configuration::getInstance();
    
    // Load configuration from file
    if (!config.loadFromFile("config/embarcadero.yaml")) {
        LOG(ERROR) << "Failed to load configuration file";
        
        // Print validation errors if any
        auto errors = config.getValidationErrors();
        for (const auto& error : errors) {
            LOG(ERROR) << "Config validation error: " << error;
        }
        return 1;
    }
    
    // Override with command line arguments
    config.overrideFromCommandLine(argc, argv);
    
    // Example: Access configuration values directly
    LOG(INFO) << "Embarcadero version: " 
              << config.config().version.major.get() << "."
              << config.config().version.minor.get();
    
    LOG(INFO) << "Broker port: " << config.getBrokerPort();
    LOG(INFO) << "CXL size: " << config.getCXLSize() << " bytes";
    LOG(INFO) << "Batch size: " << config.getBatchSize() << " bytes";
    LOG(INFO) << "Network IO threads: " << config.getNetworkIOThreads();
    
    // Example: Using legacy macros (backward compatibility)
    // Note: These macros require config.h to be included
    // LOG(INFO) << "Legacy PORT macro: " << PORT;
    // LOG(INFO) << "Legacy BATCH_SIZE macro: " << BATCH_SIZE;
    
    // Example: Environment variable override
    // Set EMBARCADERO_BROKER_PORT=9999 to override the broker port
    if (getenv("EMBARCADERO_BROKER_PORT")) {
        LOG(INFO) << "Broker port overridden by env var: " << config.getBrokerPort();
    }
    
    // Validate final configuration
    if (!config.validate()) {
        LOG(ERROR) << "Configuration validation failed";
        auto errors = config.getValidationErrors();
        for (const auto& error : errors) {
            LOG(ERROR) << "Validation error: " << error;
        }
        return 1;
    }
    
    LOG(INFO) << "Configuration loaded and validated successfully";
    return 0;
}
