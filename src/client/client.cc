#include <iostream>
#include <string>
#include <thread>
#include <stdlib.h>
#include <arpa/inet.h>

#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>

#include "client.h"

void PublishThroughputTest(size_t message_size, int duration, int num_threads){
	std::atomic<unsigned int> client_order_{0};
    PubConfig config = {
        acknowledge: true,
        client_id: 0, // TODO(erika): how is this set?
        client_order: 0, // TODO(erika): how is this set? // Client_order should be set in increasing order, starting from 0
    };
 	std::string channel = DEFAULT_CHANNEL;
    std::istringstream iss(channel);
    std::string ip;
    std::string port;

    std::getline(iss, ip, ':'); // Extract IP
    std::getline(iss, port, ':'); // Extract port

    int port_num = std::stoi(port); // Convert port to integer

	std::string channels[NUM_CHANNEL];
	std::vector<PubSubClient*> pubsubs;
	std::vector<CompletionQueue> cqs;
	for(int i=0; i<NUM_CHANNEL; i++){
		channels[i] = ip + ":" + std::to_string(port_num);
		port_num++; 
		pubsubs.emplace_back(new PubSubClient(&config, grpc::CreateChannel(channels[i], grpc::InsecureChannelCredentials()), num_threads, message_size));
	}

    LOG(INFO) << "Created gRPC channels:" << NUM_CHANNEL;
    LOG(INFO) << "Publishing message size:" << message_size << " for " << duration;

    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            std::chrono::time_point start = std::chrono::steady_clock::now();
            while (true) {
                PublisherError pub_ret = pubsubs[i%NUM_CHANNEL]->Publish("0", i, &client_order_);
                assert(pub_ret == ERR_NO_ERROR);
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(duration)) {
                    break;
                }
            }
        });
    }

    // Join all threads to wait for them to finish
    for (auto& thread : threads) {
        thread.join();
    }

	double num_messages = (double)client_order_.load();
	for(int i=0; i<NUM_CHANNEL; i++){
		delete pubsubs[i];
	}
	num_messages = num_messages/(double)1024;

	double data_sent = (num_messages * message_size)/(double)(1024); //In MB
	double bandwidth = (double)data_sent/(double)duration;
	LOG(INFO) << "Publish Bandwidth:" << bandwidth ;
	LOG(INFO) << "Num Messages:" << num_messages*1024 ;
}

void SubscribeThroughputTest(){
}

int main(int argc, char* argv[]) {
    // Initialize logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();
	FLAGS_logtostderr = 1; // log only to console, no files.

    // Create arguments and parse them
    cxxopts::Options options("embarc-client", "Embarcadero Client");
	options.add_options()
        ("d,duration", "Number of seconds to run", cxxopts::value<int>()->default_value("1"))
        ("s,size", "Size of a message", cxxopts::value<int>()->default_value("960"))
        ("t,num_thread", "Number of request threads", cxxopts::value<int>()->default_value("1"))
        //("b,benchmark", "Type of benchmark (Publish, Subscribe)", cxxopts::value<string>()->default_value("Publish"))
	;
	auto result = options.parse(argc, argv);
	size_t message_size = result["size"].as<int>();
    int duration = result["duration"].as<int>();
    int num_threads = result["num_thread"].as<int>();

	PublishThroughputTest(message_size, duration, num_threads);

    return 0;
}
