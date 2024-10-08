#include "common/config.h"
#include "heartbeat.h"
#include "topic_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include <string>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <emmintrin.h>
#include <thread>

#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>

#define CGROUP_BASE "/sys/fs/cgroup/embarcadero_cgroup"

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

bool AttachCgroup(int broker_id){
		int fd = open((CGROUP_BASE + std::to_string(broker_id) + "/cgroup.procs").c_str(), O_WRONLY);
		if (fd < 0){
			LOG(ERROR) << "Cgroup open failed:" << strerror(errno);
			return false;
		}
		std::string pid_str = std::to_string(getpid());
		if(write(fd, pid_str.c_str(), pid_str.length()) < 0){
			LOG(ERROR) << "Attaching to the cgroup failed:" << strerror(errno) << 
			" If Permission denied, chown the cgroup.procs file try again with 'sudo setcap cap_sys_admin,cap_dac_override,cap_dac_read_search=eip ./embarlet and run ./embarlet again' or just sudo";
			return false;
		}
		close(fd);
		/*
		std::string netns_path = "/var/run/netns/embarcadero_netns" + std::to_string(broker_id);
    int netns_fd = open(netns_path.c_str(), O_RDONLY);
    if (netns_fd < 0) {
        LOG(ERROR) << "Opening network namespace failed: " << strerror(errno);
        return false;
    }

    if (setns(netns_fd, CLONE_NEWNET) < 0) {
        LOG(ERROR) << "Attaching to network namespace failed: " << strerror(errno);
        close(netns_fd);
        return false;
    }

    close(netns_fd);
		*/
		return true;
}

int main(int argc, char* argv[]){

	// *************** Initializing Logging ********************** 
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();

	std::string head_addr = "127.0.0.1:" + std::to_string(BROKER_PORT);
	cxxopts::Options options("Embarcadero", "A totally ordered pub/sub system with CXL");
	// Ex: you can add arguments on command line like ./embarcadero --head or ./embarcadero --follower="HEAD_ADDR:PORT"
	options.add_options()
		("head", "Head Node")
		("follower", "Follower Address and Port", cxxopts::value<std::string>())
		("e,emul", "Use emulation instead of CXL")
		("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
		("network_threads", "Number of network IO threads", cxxopts::value<int>()->default_value(std::to_string(NUM_NETWORK_IO_THREADS)))
		("replicate_to_mem", "Replicate to Memory")
		("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
		;

	auto arguments = options.parse(argc, argv);

	FLAGS_v = arguments["log_level"].as<int>();
	FLAGS_logtostderr = 1; // log only to console, no files.
	//FLAGS_log_dir = "/tmp/vlog2_log";

	// *************** Initializing Broker ********************** 
	bool is_head_node = false;
	bool replicate_to_memory = false;
	if (arguments.count("replicate_to_mem")) {
		replicate_to_memory = true;
	}
	if (arguments.count("head")) {
		is_head_node = true;
	} else if (arguments.count("follower")) {
		head_addr = arguments["follower"].as<std::string>();
	} else {
		LOG(INFO) << "head_addr is set to default:" << head_addr;
	}
	HeartBeatManager heartbeat_manager(is_head_node, head_addr);
	int broker_id = heartbeat_manager.GetBrokerId();
	size_t colonPos = head_addr.find(':');

	LOG(INFO) << "Starting Embarlet broker_id:" << broker_id;
	// Check Cgroup setting
	if(arguments["run_cgroup"].as<int>() > 0){
		if(!AttachCgroup(broker_id) || !CheckAvailableCores()){
			LOG(ERROR) << "CGroup core throttle is wrong";
			return -1;
		}
	}

	Embarcadero::CXL_Type cxl_type = Embarcadero::CXL_Type::Real;
	if (arguments.count("emul")) {
		cxl_type = Embarcadero::CXL_Type::Emul;
		LOG(WARNING) << "Using emulated CXL";
	}
	int num_network_io_threads = arguments["network_threads"].as<int>();


	// *************** Initializing Managers ********************** 
	Embarcadero::CXLManager cxl_manager(broker_id, cxl_type, head_addr.substr(0, colonPos));
	Embarcadero::DiskManager disk_manager(broker_id, cxl_manager.GetCXLAddr(), replicate_to_memory);
	Embarcadero::NetworkManager network_manager(broker_id, num_network_io_threads);
	Embarcadero::TopicManager topic_manager(cxl_manager, disk_manager, broker_id);
	heartbeat_manager.RegisterCreateTopicEntryCallback(std::bind(&Embarcadero::TopicManager::CreateNewTopic, &topic_manager, std::placeholders::_1, 
				std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
	if(is_head_node){
		cxl_manager.RegisterGetRegisteredBrokersCallback(std::bind(&HeartBeatManager::GetRegisteredBrokers, &heartbeat_manager, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	}

	cxl_manager.SetTopicManager(&topic_manager);
	cxl_manager.SetNetworkManager(&network_manager);
	disk_manager.SetNetworkManager(&network_manager);
	network_manager.SetCXLManager(&cxl_manager);
	network_manager.SetDiskManager(&disk_manager);

	int fd = open("script_signal_pipe", O_WRONLY);
	if (fd != -1) {
		const char* signal = "ready";
		write(fd, signal, strlen(signal) + 1);
		close(fd);
	}
	LOG(INFO) << "Embarcadero initialized. Ready to go";

	// *************** Wait unless there's a failure ********************** 
	heartbeat_manager.Wait();

	LOG(INFO) << "Embarcadero Terminating";
	return 0;
}
