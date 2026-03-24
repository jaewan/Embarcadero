#include "common.h"
#include "publisher.h"
#include "subscriber.h"
#include "test_utils.h"
#include "result_writer.h"
#include "../common/configuration.h"
#include "../common/order_level.h"

int main(int argc, char* argv[]) {
    // Initialize logging
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();
    FLAGS_logtostderr = 1; // log only to console, no files.
    
    // Setup command line options
    cxxopts::Options options("embarcadero-throughputTest", "Embarcadero Throughput Test");
    
    options.add_options()
        ("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
        ("a,ack_level", "Acknowledgement level", cxxopts::value<int>()->default_value("0"))
        ("o,order_level", "Order Level", cxxopts::value<int>()->default_value("0"))
        ("head_addr", "Head broker address for client control/data connections",
            cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("sequencer", "Sequencer Type: Embarcadero(0), Kafka(1), Scalog(2), Corfu(3)", 
            cxxopts::value<std::string>()->default_value("EMBARCADERO"))
        ("s,total_message_size", "Total size of messages to publish", 
            cxxopts::value<size_t>()->default_value("10737418240"))
        ("m,size", "Size of a message", cxxopts::value<size_t>()->default_value("1024"))
        ("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
        ("r,replication_factor", "Replication factor", cxxopts::value<int>()->default_value("0"))
        ("replicate_tinode", "Replicate Tinode for Disaggregated memory fault tolerance")
#ifdef COLLECT_LATENCY_STATS
        ("record_results", "Record Results in a csv file")
#endif
        ("t,test_number", "Test to run. 0:pub/sub 1:E2E 2:Latency 3:Parallel", 
            cxxopts::value<int>()->default_value("0"))
        ("p,parallel_client", "Number of parallel clients", cxxopts::value<int>()->default_value("1"))
        ("num_brokers_to_kill", "Number of brokers to kill during execution", 
            cxxopts::value<int>()->default_value("0"))
        ("failure_percentage", "When to fail brokers, after what percentages of messages sent", 
            cxxopts::value<double>()->default_value("0"))
        ("steady_rate", "Send message in steady rate")
        ("target_mbps", "Target offered load for latency mode (MB/s). 0 disables pacing",
            cxxopts::value<double>()->default_value("0"))
        ("n,num_threads_per_broker", "Number of request threads_per_broker", 
            cxxopts::value<size_t>()->default_value("4"))
        ("config", "Configuration file path", cxxopts::value<std::string>()->default_value("config/client.yaml"));
    
    auto result = options.parse(argc, argv);
    
    // *************** Load Configuration *********************
    Embarcadero::Configuration& config = Embarcadero::Configuration::getInstance();
    std::string config_file = result["config"].as<std::string>();
    
    if (!config.loadFromFile(config_file)) {
        LOG(WARNING) << "Failed to load configuration from " << config_file << ", using defaults";
        // Continue with defaults - don't fail
    } else {
        VLOG(1) << "Configuration loaded successfully from " << config_file;
    }
    
    // Extract parameters (with config override capability)
    size_t message_size = result["size"].as<size_t>();
    size_t total_message_size = result["total_message_size"].as<size_t>();
    
    // Use config value if command line argument is default
    size_t num_threads_per_broker;
    if (result["num_threads_per_broker"].count() == 0 || result["num_threads_per_broker"].as<size_t>() == 4) {
        // Use config value
        num_threads_per_broker = config.config().client.publisher.threads_per_broker.get();
        LOG(INFO) << "Using threads_per_broker from config: " << num_threads_per_broker;
    } else {
        // Use command line value
        num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
        LOG(INFO) << "Using threads_per_broker from command line: " << num_threads_per_broker;
    }
    int order = result["order_level"].as<int>();
    if (order == Embarcadero::kOrderLegacyStrong) {
        LOG(ERROR) << "Order 4 is no longer supported; use Order 5 for strong ordering.";
        return -1;
    }
    int replication_factor = result["replication_factor"].as<int>();
    bool replicate_tinode = result.count("replicate_tinode");
    int num_clients = result["parallel_client"].as<int>();
    int num_brokers_to_kill = result["num_brokers_to_kill"].as<int>();
    std::atomic<int> synchronizer{num_clients};
    int test_num = result["test_number"].as<int>();
    int ack_level = result["ack_level"].as<int>();
    SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());
    FLAGS_v = result["log_level"].as<int>();

    if (seq_type == heartbeat_system::SequencerType::CORFU && order != Embarcadero::kOrderTotal) {
        LOG(ERROR) << "Corfu supports only ORDER=2 in this implementation (got ORDER=" << order << ").";
        return -1;
    }

    // ACK level 2 is defined as "after replication". Running ACK=2 with
    // replication_factor=0 silently degrades semantics and causes confusing
    // timeout behavior. Keep legacy scripts working by promoting to 1 replica.
    if (seq_type == heartbeat_system::SequencerType::EMBARCADERO &&
        ack_level == 2 && replication_factor <= 0) {
        LOG(WARNING) << "ACK level 2 requires replication_factor>0; "
                     << "promoting replication_factor from " << replication_factor
                     << " to 1.";
        replication_factor = 1;
    }
    
    // Check if cgroup is properly set up
    if (result["run_cgroup"].as<int>() > 0 && !CheckAvailableCores()) {
        LOG(ERROR) << "CGroup core throttle is wrong";
        return -1;
    }
    
    // Legacy alignment path retained for non-Corfu ORDER=3 experiments.
    if (order == 3) {
        size_t padding = message_size % 64;
        if (padding) {
            padding = 64 - padding;
        }
        size_t paddedSize = message_size + padding + sizeof(Embarcadero::MessageHeader);
        
        size_t n = total_message_size / message_size;
        size_t total_payload = n * paddedSize;
        padding = total_payload % (BATCH_SIZE);
        if (padding) {
            padding = (num_threads_per_broker * BATCH_SIZE) - padding;
            LOG(INFO) << "Adjusting total message size from " << total_message_size 
                      << " to " << total_message_size + padding 
                      << " :" << (total_message_size + padding) % (num_threads_per_broker * BATCH_SIZE);
            total_message_size += padding;
        }
    }
    
    // Create gRPC stub
    std::string head_addr = GetHeadAddr(result);
    std::unique_ptr<HeartBeat::Stub> stub = HeartBeat::NewStub(
        grpc::CreateChannel(head_addr + ":" + std::to_string(BROKER_PORT),
                           grpc::InsecureChannelCredentials()));
    
    // Prepare topic
    char topic[TOPIC_NAME_SIZE];
    memset(topic, 0, TOPIC_NAME_SIZE);
    memcpy(topic, "TestTopic", 9);
    
    auto ensure_topic_ready = [&]() -> bool {
        if (!CreateNewTopic(stub, topic, order, seq_type, replication_factor,
                            replicate_tinode, ack_level)) {
            LOG(ERROR) << "Benchmark setup failed: topic creation did not succeed.";
            return false;
        }
        return true;
    };

    // Create result writer
    ResultWriter writer(result);
    
    // Run the specified test
    switch (test_num) {
        case 0: {
            // Publish and Subscribe test
            if (!ensure_topic_ready()) return EXIT_FAILURE;
            LOG(INFO) << "Running Publish and Subscribe: " << total_message_size;
            double pub_bandwidthMb = PublishThroughputTest(result, topic, synchronizer);
            sleep(3);
            double sub_bandwidthMb = SubscribeThroughputTest(result, topic);
            writer.SetPubResult(pub_bandwidthMb);
            writer.SetSubResult(sub_bandwidthMb);
            break;
        }
        case 1: {
            // E2E Throughput test
            LOG(INFO) << "Running E2E Throughput";
            if (!ensure_topic_ready()) return EXIT_FAILURE;
            std::pair<double, double> bandwidths = E2EThroughputTest(result, topic);
            writer.SetPubResult(bandwidths.first);
            writer.SetE2EResult(bandwidths.second);
            break;
        }
        case 2: {
            // E2E Latency test
            LOG(INFO) << "Running E2E Latency Test";
            if (!ensure_topic_ready()) return EXIT_FAILURE;
            std::pair<double, double> bandwidths = LatencyTest(result, topic);
            writer.SetPubResult(bandwidths.first);
            writer.SetSubResult(bandwidths.second);
            break;
        }
        case 3: {
            // Parallel Publish test
            LOG(INFO) << "Running Parallel Publish Test num_clients:" << num_clients 
                      << ":" << num_threads_per_broker;
            if (!ensure_topic_ready()) return EXIT_FAILURE;
            std::vector<std::thread> threads;
            std::vector<std::promise<double>> promises(num_clients);
            std::vector<std::future<double>> futures;
            std::future<double> sub_future;
            std::promise<double> sub_promise;
            std::vector<std::thread> sub_thread;
            
            sub_future = sub_promise.get_future();
            
            // Prepare the futures
            for (int i = 0; i < num_clients; ++i) {
                futures.push_back(promises[i].get_future());
            }
            
            // Launch the subscriber thread
            sub_thread.emplace_back([&result, &topic, &sub_promise]() {
                double res = SubscribeThroughputTest(result, topic);
                sub_promise.set_value(res);
            });
            
            // Launch the publisher threads
            for (int i = 0; i < num_clients; i++) {
                threads.emplace_back([&result, &topic, &synchronizer, &promises, i]() {
                    double res = PublishThroughputTest(result, topic, synchronizer);
                    promises[i].set_value(res);
                });
            }
            
            // Wait for results
            double aggregate_bandwidth = 0;
            for (int i = 0; i < num_clients; ++i) {
                if (threads[i].joinable()) {
                    threads[i].join();
                    aggregate_bandwidth += futures[i].get();
                }
            }
            
            double subBandwidth = 0;
            if (sub_thread[0].joinable()) {
                sub_thread[0].join();
                subBandwidth = sub_future.get();
            }
            
            writer.SetPubResult(aggregate_bandwidth);
            writer.SetSubResult(subBandwidth);
            
            std::cout << "Aggregate Bandwidth:" << aggregate_bandwidth;
            std::cout << "Sub Bandwidth:" << subBandwidth;
            break;
        }
        case 4: {
            // Broker failure test
            LOG(INFO) << "Running Broker failure at publish ";
            if (num_brokers_to_kill == 0) {
                LOG(WARNING) << "Number of broker fail in FailureTest is 0, are you sure about it?";
            }
            
            auto killbrokers = [&stub, num_brokers_to_kill]() {
                return KillBrokers(stub, num_brokers_to_kill);
            };
            
            if (!ensure_topic_ready()) return EXIT_FAILURE;
            double pub_bandwidthMb = FailurePublishThroughputTest(result, topic, killbrokers);
            writer.SetPubResult(pub_bandwidthMb);
            break;
        }
        case 5: {
            // Publish-only test
            VLOG(1) << "Running Publish : " << total_message_size;
            if (!ensure_topic_ready()) return EXIT_FAILURE;
            double pub_bandwidthMb = PublishThroughputTest(result, topic, synchronizer);
            writer.SetPubResult(pub_bandwidthMb);
            break;
        }
        case 6: {
            // Subscribe-only test
            LOG(INFO) << "Running Subscribe ";
            double sub_bandwidthMb = SubscribeThroughputTest(result, topic);
            writer.SetSubResult(sub_bandwidthMb);
            break;
        }
        case 7: {
            // Publish and Subscribe test
            if (!ensure_topic_ready()) return EXIT_FAILURE;
            LOG(INFO) << "Running Publish and Consume: " << total_message_size;
            double pub_bandwidthMb = PublishThroughputTest(result, topic, synchronizer);
            sleep(3);
            double sub_bandwidthMb = ConsumeThroughputTest(result, topic);
            break;
        }
        default:
            LOG(ERROR) << "Invalid test number option:" << result["test_number"].as<int>();
            break;
    }
    
		/* TODO(Jae) call terminatecluster after test
		 *  writer.~ResultWriter();
      257 -
      258 -    // Shutdown the cluster
      259 -    google::protobuf::Empty request, response;
      260 -    grpc::ClientContext context;
      261 -    VLOG(5) << "Calling TerminateCluster";
      262 -    stub->TerminateCluster(&context, request, &response);
		 */
    // Clean up
    // Note: writer destructor will be called automatically when it goes out of scope
    // TerminateCluster is skipped for testing - brokers should remain running

    return 0;
}
