#include "common/config.h"
#include "pub_queue.h"
#include "pub_task.h"
#include "peer.h"
#include "topic_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include <iostream>
#include <string>
#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts

int main(int argc, char* argv[]){

	cxxopts::Options options("embarc", "Embrokeradaro (TODO: add real description)");
	options.add_options()
		("e,emul", "Use emulation instead of CXL")
	;
	auto result = options.parse(argc, argv);

	Embarcadero::CXL_Type cxl_type = Embarcadero::CXL_Type::Real;
	if (result.count("emul")) {
		cxl_type = Embarcadero::CXL_Type::Emul;
		std::cout << "WARNING: Using emulated CXL" << std::endl;
	}

	//Initialize
	int broker_id = 0;
	Embarcadero::CXLManager cxl_manager(10000, broker_id, cxl_type);
	Embarcadero::DiskManager disk_manager(10000);
	Embarcadero::NetworkManager network_manager(100000, NUM_IO_RECEIVE_THREADS, NUM_IO_ACK_THREADS);
	Embarcadero::TopicManager topic_manager(cxl_manager, broker_id);

	cxl_manager.SetTopicManager(&topic_manager);
	cxl_manager.SetNetworkManager(&network_manager);
	disk_manager.SetNetworkManager(&network_manager);
	network_manager.SetCXLManager(&cxl_manager);
	network_manager.SetDiskManager(&disk_manager);

/*
	char topic[32];
	memset(topic, 0, 32);
	topic[0] = '0';
	topic_manager.CreateNewTopic(topic);
	*/
	//********* Load Generate **************
	char topic[32];
	memset(topic, 0, 32);
	topic[0] = '0';
	topic_manager.CreateNewTopic(topic);
	
	/*
	Embarcadero::PublishRequest req;
	std::atomic<int> c{2};
	memcpy(req.topic, topic, 32);
	req.payload_address = malloc(1024);
	req.counter = &c;
	req.size = 1024;
	req.acknowledge = true;
	*/
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

	// Create a pub task
	// This is slightly less than ideal, because it involved two heap allocations.
	// But we'll worry about performance optimizations later.
	std::atomic<uint8_t> *num_subtasks = new std::atomic<uint8_t>(1); // this will be freed by the PubTask destructor
	PubTask *pt = new PubTask(num_subtasks);

	// Create a pub queue
	PubQueue *pq = new PubQueue(PUB_QUEUE_CAPACITY);

	pq_enqueue(pq, pt);
	pq_dequeue(pq, &pt);

	uint8_t tasks_left = pt->subtasks_left->fetch_sub(1, std::memory_order_seq_cst);
	printf("Tasks left: %d\n", tasks_left);
	assert(tasks_left == 1);

	pq_enqueue(pq, pt);
	pq_dequeue(pq, &pt);

	tasks_left = pt->subtasks_left->fetch_sub(1, std::memory_order_seq_cst);
	printf("Tasks left: %d\n", tasks_left);
	if (0 == tasks_left) {
		printf("No tasks left! Time to signal the completion queue before freeing the PubTask!\n");
		// TODO: signal completion queue
		delete pt;
	}
	assert(tasks_left == 0);

	delete pq;
	*/

	/*
	size_t num_net_threads = 3;
	NetworkManager network_manager(num_net_threads);
	network_manager.Run(DEFAULT_PORT);
	*/
	return 0;
}
