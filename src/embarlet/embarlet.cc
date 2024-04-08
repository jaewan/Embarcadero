#include <iostream>
#include <string>
#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>

#include "common/config.h"
#include "peer.h"
#include "topic_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include "ack_queue.h"
#include "req_queue.h"

int main(int argc, char* argv[]){
	// Initialize logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();

	FLAGS_logtostderr = 1; // log only to console, no files.

	cxxopts::Options options("embarc", "Embrokeradaro (TODO: add real description)");
	options.add_options()
		("e,emul", "Use emulation instead of CXL")
	;
	auto result = options.parse(argc, argv);

	Embarcadero::CXL_Type cxl_type = Embarcadero::CXL_Type::Real;
	if (result.count("emul")) {
		cxl_type = Embarcadero::CXL_Type::Emul;
		LOG(WARNING) << "Using emulated CXL";
	}

	// Initialize queues
	auto ackQueue = std::make_shared<Embarcadero::AckQueue>(ACK_QUEUE_SIZE);
	auto cxlReqQueue = std::make_shared<Embarcadero::ReqQueue>(REQ_QUEUE_SIZE);
	auto diskReqQueue = std::make_shared<Embarcadero::ReqQueue>(REQ_QUEUE_SIZE);

	// Initialize components
	int broker_id = 0;
	Embarcadero::CXLManager cxl_manager(ackQueue, cxlReqQueue, broker_id, cxl_type);
	Embarcadero::DiskManager disk_manager(ackQueue, diskReqQueue);
	Embarcadero::NetworkManager network_manager(ackQueue, cxlReqQueue, diskReqQueue, NUM_IO_RECEIVE_THREADS, NUM_IO_ACK_THREADS);
	Embarcadero::TopicManager topic_manager(cxl_manager, broker_id);
	cxl_manager.SetTopicManager(&topic_manager);

	//********* Load Generate **************
	char topic[32] = { 0 };
	topic[0] = 'a';
	topic_manager.CreateNewTopic(topic);

	/*
	Embarcadero::NetworkRequest req;
	req.req_type = Embarcadero::Test;
	std::cout <<"Submitting reqs" << std::endl;
	for(int i =0; i<1000000; i++){
		network_manager.EnqueueAck(req);
	}
	std::cout <<"Submitted all reqs" << std::endl;
	//cxl_manager.EnqueueRequest(req);
	//disk_manager.EnqueueRequest(req);
	sleep(1);

	void* last_addr = nullptr;
	void* messages = nullptr;
	size_t messages_size;
	size_t last_offset = 0;
	cxl_manager.GetMessageAddr(topic, last_offset, last_addr, messages, messages_size);
	*/

    sleep(60);

	/*
	//Embarcadero::CXLManager cxlmanager;
	cxxopts::Options options("embarcadero", "a totally ordered pub/sub system with CXL");

	// Ex: you can add arguments on command line like ./embarcadero --head or ./embarcadero --follower="10.182.0.4:8080"
	options.add_options()
		("head", "Head Node")
		("follower", "Follower Address and Port", cxxopts::value<std::string>());

	auto arguments = options.parse(argc, argv);

	if (arguments.count("head")) {
		// Initialize peer broker
		PeerBroker head_broker(true);

		head_broker.Run();
	} else if (arguments.count("follower")) {
		std::string follower = arguments["follower"].as<std::string>();

		std::string head_addr = follower.substr(0, follower.find(":"));
		std::string head_port = follower.substr(follower.find(":") + 1);

		PeerBroker follower_broker(false, head_addr, head_port);
		follower_broker.Run();
	} else {
		std::cout << "Invalid arguments" << std::endl;
	}
	*/
	return 0;
}
