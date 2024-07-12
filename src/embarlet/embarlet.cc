#include "common/config.h"
#include "pub_queue.h"
#include "pub_task.h"
#include "heartbeat.h"
#include "topic_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <emmintrin.h>
#include <thread>

#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>

bool CheckAvailableCores(){
	sleep(1);
	size_t num_cores = 0;
	cpu_set_t mask;
	CPU_ZERO(&mask);

	if (sched_getaffinity(0, sizeof(mask), &mask) == -1) {
			perror("sched_getaffinity");
			exit(EXIT_FAILURE);
	}

	printf("This process can run on CPUs: ");
	for (int i = 0; i < CPU_SETSIZE; i++) {
			if (CPU_ISSET(i, &mask)) {
					printf("%d ", i);
					num_cores++;
			}
	}
	return num_cores == CGROUP_CORE;
}

int main(int argc, char* argv[]){

	// *************** Initializing Logging ********************** 
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();

	cxxopts::Options options("Embarcadero", "A totally ordered pub/sub system with CXL");
	// Ex: you can add arguments on command line like ./embarcadero --head or ./embarcadero --follower="HEAD_ADDR:PORT"
	options.add_options()
			("head", "Head Node")
			("follower", "Follower Address and Port", cxxopts::value<std::string>())
			("e,emul", "Use emulation instead of CXL")
			("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
			("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
		;

	auto arguments = options.parse(argc, argv);

	FLAGS_v = arguments["log_level"].as<int>();
	FLAGS_logtostderr = 1; // log only to console, no files.
	//FLAGS_log_dir = "/tmp/vlog2_log";

	// *************** Initializing Broker ********************** 
	bool is_head_node = false;
	std::string head_addr = "127.0.0.1:" + std::to_string(BROKER_PORT);

	if (arguments.count("head")) {
		is_head_node = true;
	} else if (arguments.count("follower")) {
		head_addr = arguments["follower"].as<std::string>();
	} else {
		LOG(ERROR) << "Invalid arguments";
	}
	HeartBeatManager heartbeat_manager(is_head_node, head_addr);
	int broker_id = heartbeat_manager.GetBrokerId();

	LOG(INFO) << "Starting Embarlet broker_id:" << broker_id;
	// Check Cgroup setting
	if(arguments["run_cgroup"].as<int>() > 0 && !CheckAvailableCores()){
		LOG(ERROR) << "CGroup core throttle is wrong";
		return -1;
	}

	// *************** Initializing Managers ********************** 
	Embarcadero::CXLManager cxl_manager((1UL<<23), broker_id);
	Embarcadero::DiskManager disk_manager((1UL<<23));
	Embarcadero::NetworkManager network_manager(128, broker_id, NUM_NETWORK_IO_THREADS, false);
	Embarcadero::TopicManager topic_manager(cxl_manager, broker_id);

	cxl_manager.SetTopicManager(&topic_manager);
	cxl_manager.SetNetworkManager(&network_manager);
	disk_manager.SetNetworkManager(&network_manager);
	network_manager.SetCXLManager(&cxl_manager);
	network_manager.SetDiskManager(&disk_manager);

	//********* Load Generate **************
	/*
	if(is_head_node || true){ 
		char topic[31];
		memset(topic, 0, 31);
		topic[0] = '0';
		int order = 0;
		topic_manager.CreateNewTopic(topic, order);
	}
	*/

	LOG(INFO) << "You are now safe to go";
	//cxl_manager.StartInternalTest();
	
	// *************** Wait unless there's a failure ********************** 
	heartbeat_manager.Wait();

	return 0;
}
