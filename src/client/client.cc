#include <iostream>
#include <stdlib.h>
#include <arpa/inet.h>

#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>

#include "client.h"

void PublishThroughputTest(){
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
	;
	auto result = options.parse(argc, argv);
	size_t message_size = result["size"].as<int>();
    int duration = result["duration"].as<int>();
	char* payload = (char*)malloc(sizeof(char) * (message_size + 64));
	MessageHeader *header = (MessageHeader*)payload;
    LOG(INFO) << "Running for " << duration << " seconds";

    PubConfig config = {
        acknowledge: true,
        client_id: 0, // TODO(erika): how is this set?
        client_order: 0, // TODO(erika): how is this set? // Client_order should be set in increasing order, starting from 0
    };
	header->client_id = 0;
	header->client_order = 0;
	header->size = message_size;
	header->total_order = 0;
	size_t padding = message_size%64;
	if(padding > 0)
		padding = 64 - padding;
	header->total_order = 0;
	header->paddedSize = message_size + padding;
	header->segment_header = nullptr;
	header->logical_offset = (size_t)-1;;
	header->next_message = nullptr;

    PubSubClient pubsub(&config, grpc::CreateChannel(DEFAULT_CHANNEL, grpc::InsecureChannelCredentials()));
    LOG(INFO) << "Created gRPC channel";
    LOG(INFO) << "Publishing message size:" << message_size << " for " << duration;

    std::chrono::time_point start = std::chrono::steady_clock::now();
    while (true) {
        PublisherError pub_ret = pubsub.Publish("0", payload, message_size);
        assert(pub_ret == ERR_NO_ERROR);
        if(std::chrono::steady_clock::now() - start > std::chrono::seconds(duration)) {
            break;
        }
		header->client_order++;
    }
	
	double data_sent = ((double)message_size*header->client_order)/(1024*1024); //In MB
	double bandwidth = (double)data_sent/(double)duration;
	LOG(INFO) << "Publish Bandwidth:" << bandwidth ;
    return 0;
}
