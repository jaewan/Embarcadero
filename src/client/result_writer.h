#pragma once

#include "common.h"

/**
 * Class for writing test results to a file
 */
class ResultWriter {
public:
    /**
     * Constructor
     * @param result Parse result from command line
     */
    ResultWriter(const cxxopts::ParseResult& result);
    
    /**
     * Destructor - writes results to file
     */
    ~ResultWriter();
    
    /**
     * Sets the publish result
     * @param res Bandwidth in MBps
     */
    void SetPubResult(double res);
    
    /**
     * Sets the subscribe result
     * @param res Bandwidth in MBps
     */
    void SetSubResult(double res);
    
    /**
     * Sets the end-to-end result
     * @param res Bandwidth in MBps
     */
    void SetE2EResult(double res);

private:
    size_t message_size;
    size_t total_message_size;
    size_t num_threads_per_broker;
    int ack_level;
    int order;
    int replication_factor;
    bool replicate_tinode;
    bool record_result_;
    int num_clients;
    int num_brokers_to_kill;
    double failure_percentage;
    std::string seq_type;
    
    std::string result_path;
    double pubBandwidthMbps = 0;
    double subBandwidthMbps = 0;
    double e2eBandwidthMbps = 0;
};
