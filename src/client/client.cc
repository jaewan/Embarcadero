#include <iostream>
#include <arpa/inet.h>

#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>

#include "client.h"

int main(int argc, char* argv[]) {
    // Initialize logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();
	FLAGS_logtostderr = 1; // log only to console, no files.

    // Create arguments and parse them
    cxxopts::Options options("embarc-client", "Embarcadero Client");
	options.add_options()
        ("d,duration", "Number of seconds to run", cxxopts::value<int>())
	;
	auto result = options.parse(argc, argv);
    int duration = result["duration"].as<int>();
    LOG(INFO) << "Running for " << duration << " seconds";

    PubConfig config = {
        acknowledge: true,
        client_id: 0, // TODO(erika): how is this set?
        client_order: 0, // TODO(erika): how is this set?
    };
    PubSubClient pubsub(&config, grpc::CreateChannel(DEFAULT_CHANNEL, grpc::InsecureChannelCredentials()));
    LOG(INFO) << "Created gRPC channel";

    std::chrono::time_point start = std::chrono::steady_clock::now();
    while (true) {
        PublisherError pub_ret = pubsub.Publish("a", "mymsg", strlen("mymsg"));
        assert(pub_ret == ERR_NO_ERROR);
        if(std::chrono::steady_clock::now() - start > std::chrono::seconds(duration)) {
            break;
        }
    }
    return 0;
}