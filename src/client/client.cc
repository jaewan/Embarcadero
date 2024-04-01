#include <iostream>
#include <arpa/inet.h>

#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts


#include "publisher.h"
#include "subscriber.h"

int main(int argc, char* argv[]){
    std::cout << "IN MAIN" << std::endl;

    PublisherConfig publisher_config;
    publisher_config.ack_level = ACK0;
    publisher_config.server_port = htons(CLIENT_PORT);
    publisher_config.server_ip = LOCAL_HOST;

    std::cout << publisher_config.server_ip << std::endl;
    std::cout << "MADE CONFIG" << std::endl;

    Publisher pub(&publisher_config);
    return 0;
}