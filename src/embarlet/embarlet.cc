#include "config.h"
#include "../disk_manager/disk_manager.h"
//#include "../network_manager/network_manager.h"
//#include "../cxl_manager/cxl_manager.h"
#include <iostream>
#include "peer.h"
#include <string>
#include <cxxopts.hpp>

int main(int argc, char* argv[]){

	xxopts::Options options("embarcadero");

    options.add_options()
		("head", "Head Node")
        ("follower", "Follower Address and Port", cxxopts::value<std::string>());

    auto arguments = options.parse(argc, argv);

	if (arguments.count("head")) {
        // Initialize peer broker
        PeerBroker head_broker(true);

        head_broker.Run();
    } else if (arguments.count("follower")) {
        std::string follower = result["follower"].as<std::string>();

		std::string head_addr = follower.substr(0, follower.find(":"));
		std::string head_port = follower.substr(follower.find(":") + 1);
		
        PeerBroker follower_broker(false, head_addr, head_port);
        follower_broker.Run();
    } else {
        std::cout << "Invalid arguments" << std::endl;
    }

	return 0;
}
