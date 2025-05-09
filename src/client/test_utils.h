#pragma once

#include "common.h"
#include "publisher.h"
#include "subscriber.h"

/**
 * Runs a failure publish throughput test
 * @param result Parse result from command line
 * @param topic Topic name
 * @param killbrokers Function to kill brokers
 * @return Bandwidth in MBps
 */
double FailurePublishThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE], 
                                  std::function<bool()> killbrokers);

/**
 * Runs a publish throughput test
 * @param result Parse result from command line
 * @param topic Topic name
 * @param synchronizer Synchronizer for parallel tests
 * @return Bandwidth in MBps
 */
double PublishThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE], 
                            std::atomic<int>& synchronizer);

/**
 * Runs a subscribe throughput test
 * @param result Parse result from command line
 * @param topic Topic name
 * @return Bandwidth in MBps
 */
double SubscribeThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]);
double ConsumeThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]);

/**
 * Runs an end-to-end throughput test
 * @param result Parse result from command line
 * @param topic Topic name
 * @return Pair of publish and E2E bandwidth in MBps
 */
std::pair<double, double> E2EThroughputTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]);

/**
 * Runs a latency test
 * @param result Parse result from command line
 * @param topic Topic name
 * @return Pair of publish and E2E bandwidth in MBps
 */
std::pair<double, double> LatencyTest(const cxxopts::ParseResult& result, char topic[TOPIC_NAME_SIZE]);

/**
 * Kills a number of brokers
 * @param stub gRPC stub
 * @param num_brokers Number of brokers to kill
 * @return true if successful, false otherwise
 */
bool KillBrokers(std::unique_ptr<HeartBeat::Stub>& stub, int num_brokers);
