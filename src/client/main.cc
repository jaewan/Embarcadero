#include "common.h"
#include "publisher.h"
#include "subscriber.h"
#include "test_utils.h"
#include "result_writer.h"

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
        ("sequencer", "Sequencer Type: Embarcadero(0), Kafka(1), Scalog(2), Corfu(3)", 
            cxxopts::value<std::string>()->default_value("EMBARCADERO"))
        ("s,total_message_size", "Total size of messages to publish", 
            cxxopts::value<size_t>()->default_value("10737418240"))
        ("m,size", "Size of a message", cxxopts::value<size_t>()->default_value("1024"))
        ("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
        ("r,replication_factor", "Replication factor", cxxopts::value<int>()->default_value("0"))
        ("replicate_tinode", "Replicate Tinode for Disaggregated memory fault tolerance")
        ("record_results", "Record Results in a csv file")
        ("t,test_number", "Test to run. 0:pub/sub 1:E2E 2:Latency 3:Parallel", 
            cxxopts::value<int>()->default_value("0"))
        ("p,parallel_client", "Number of parallel clients", cxxopts::value<int>()->default_value("1"))
        ("num_brokers_to_kill", "Number of brokers to kill during execution", 
            cxxopts::value<int>()->default_value("0"))
        ("failure_percentage", "When to fail brokers, after what percentages of messages sent", 
            cxxopts::value<double>()->default_value("0"))
        ("steady_rate", "Send message in steady rate")
        ("n,num_threads_per_broker", "Number of request threads_per_broker", 
            cxxopts::value<size_t>()->default_value("3"));
    
    auto result = options.parse(argc, argv);
    
    // Extract parameters
    size_t message_size = result["size"].as<size_t>();
    size_t total_message_size = result["total_message_size"].as<size_t>();
    size_t num_threads_per_broker = result["num_threads_per_broker"].as<size_t>();
    int order = result["order_level"].as<int>();
    int replication_factor = result["replication_factor"].as<int>();
    bool replicate_tinode = result.count("replicate_tinode");
    int num_clients = result["parallel_client"].as<int>();
    int num_brokers_to_kill = result["num_brokers_to_kill"].as<int>();
    std::atomic<int> synchronizer{num_clients};
    int test_num = result["test_number"].as<int>();
    int ack_level = result["ack_level"].as<int>();
    SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());
    FLAGS_v = result["log_level"].as<int>();
    
    // Check if cgroup is properly set up
    if (result["run_cgroup"].as<int>() > 0 && !CheckAvailableCores()) {
        LOG(ERROR) << "CGroup core throttle is wrong";
        return -1;
    }
    
    // Special handling for order level 3
    if (order == 3) {
        size_t padding = message_size % 64;
        if (padding) {
            padding = 64 - padding;
        }
        size_t paddedSize = message_size + padding + sizeof(Embarcadero::MessageHeader);
        if (BATCH_SIZE % (paddedSize)) {
            LOG(ERROR) << "Adjusting Batch size of message size!!";
            return 0;
        }
        
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
    std::unique_ptr<HeartBeat::Stub> stub = HeartBeat::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(BROKER_PORT), 
                           grpc::InsecureChannelCredentials()));
    
    // Prepare topic
    char topic[TOPIC_NAME_SIZE];
    memset(topic, 0, TOPIC_NAME_SIZE);
    memcpy(topic, "TestTopic", 9);
    
    // Create result writer
    ResultWriter writer(result);
    
    // Run the specified test
    switch (test_num) {
        case 0: {
            // Publish and Subscribe test
            CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode, ack_level);
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
            CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode, ack_level);
            std::pair<double, double> bandwidths = E2EThroughputTest(result, topic);
            writer.SetPubResult(bandwidths.first);
            writer.SetE2EResult(bandwidths.second);
            break;
        }
        case 2: {
            // E2E Latency test
            LOG(INFO) << "Running E2E Latency Test";
            CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode, ack_level);
            std::pair<double, double> bandwidths = LatencyTest(result, topic);
            writer.SetPubResult(bandwidths.first);
            writer.SetSubResult(bandwidths.second);
            break;
        }
        case 3: {
            // Parallel Publish test
            LOG(INFO) << "Running Parallel Publish Test num_clients:" << num_clients 
                      << ":" << num_threads_per_broker;
            CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode, ack_level);
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
            
            CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode, ack_level);
            double pub_bandwidthMb = FailurePublishThroughputTest(result, topic, killbrokers);
            writer.SetPubResult(pub_bandwidthMb);
            break;
        }
        case 5: {
            // Publish-only test
            LOG(INFO) << "Running Publish : " << total_message_size;
            CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode, ack_level);
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
            CreateNewTopic(stub, topic, order, seq_type, replication_factor, replicate_tinode, ack_level);
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
    
    // Clean up
    writer.~ResultWriter();
    
    // Shutdown the cluster
    google::protobuf::Empty request, response;
    grpc::ClientContext context;
    VLOG(5) << "Calling TerminateCluster";
    stub->TerminateCluster(&context, request, &response);
    
    return 0;
}
