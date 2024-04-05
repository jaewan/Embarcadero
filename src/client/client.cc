#include <iostream>
#include <arpa/inet.h>

#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts

#include "client.h"

int main(int argc, char* argv[]){
    PubConfig config = {
        acknowledge: true,
        client_id: 0, // TODO(erika): how is this set?
        client_order: 0, // TODO(erika): how is this set?
    };
    PubSubClient pubsub(&config, grpc::CreateChannel(DEFAULT_CHANNEL, grpc::InsecureChannelCredentials()));
    PublisherError pub_ret = pubsub.Publish("mytopic", "mymsg", strlen("mymsg"));
    std::cout << "Publisher received: " << pub_ret << std::endl;
    return 0;
}