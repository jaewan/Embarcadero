#include <iostream>
#include <arpa/inet.h>

#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts

#include "client.h"

int main(int argc, char* argv[]){
    PubSubClient pubsub(grpc::CreateChannel(DEFAULT_CHANNEL, grpc::InsecureChannelCredentials()));
    PublisherError pub_ret = pubsub.Publish(0, 0, "Hello", strlen("Hello"));
    std::cout << "Publisher received: " << pub_ret << std::endl;
    return 0;
}