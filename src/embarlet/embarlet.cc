#include <string>
#include <thread>
#include <functional>
#include <iostream>

// System includes
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <string.h>

// SIMD includes
#include <emmintrin.h>

// Third-party libraries
#include <cxxopts.hpp>
#include <glog/logging.h>

// Project includes
#include "common/config.h"
#include "heartbeat.h"
#include "topic_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
#include "../cxl_manager/cxl_manager.h"

namespace {

constexpr char CGROUP_BASE[] = "/sys/fs/cgroup/embarcadero_cgroup";

// RAII wrapper for file descriptors
class ScopedFD {
	public:
		explicit ScopedFD(int fd) : fd_(fd) {}
		~ScopedFD() { if (fd_ >= 0) close(fd_); }
		int get() const { return fd_; }
		bool isValid() const { return fd_ >= 0; }

		// Prevent copying
		ScopedFD(const ScopedFD&) = delete;
		ScopedFD& operator=(const ScopedFD&) = delete;
	private:
		int fd_;
};

bool CheckAvailableCores() {
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

bool AttachCgroup(int broker_id) {
	std::string cgroup_path = std::string(CGROUP_BASE) + std::to_string(broker_id) + "/cgroup.procs";
	ScopedFD fd(open(cgroup_path.c_str(), O_WRONLY));

	if (!fd.isValid()) {
		LOG(ERROR) << "Cgroup open failed: " << strerror(errno);
		return false;
	}

	std::string pid_str = std::to_string(getpid());
	if (write(fd.get(), pid_str.c_str(), pid_str.length()) < 0) {
		LOG(ERROR) << "Attaching to the cgroup failed: " << strerror(errno)
			<< " If Permission denied, chown the cgroup.procs file try again with "
			<< "'sudo setcap cap_sys_admin,cap_dac_override,cap_dac_read_search=eip ./embarlet and run ./embarlet again' "
			<< "or just sudo";
		return false;
	}

	return true;
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
}

void SignalScriptReady() {
	ScopedFD fd(open("script_signal_pipe", O_WRONLY));
	if (fd.isValid()) {
		const char* signal = "ready";
		write(fd.get(), signal, strlen(signal) + 1);
	}
}

} // end of namespace

int main(int argc, char* argv[]) {
	// Initialize logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();

	// Parse command line arguments
	std::string head_addr = "127.0.0.1:" + std::to_string(BROKER_PORT);
	cxxopts::Options options("Embarcadero", "A totally ordered pub/sub system with CXL");

	options.add_options()
		("head", "Head Node")
		("follower", "Follower Address and Port", cxxopts::value<std::string>())
		("scalog", "Run also as a Scalog Replica")
		("corfu", "Run also as a Corfu Replica")
		("e,emul", "Use emulation instead of CXL")
		("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
		("network_threads", "Number of network IO threads",
		 cxxopts::value<int>()->default_value(std::to_string(NUM_NETWORK_IO_THREADS)))
		("replicate_to_disk", "Replicate to Disk instead of Memory")
		("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"));

	auto arguments = options.parse(argc, argv);

	FLAGS_v = arguments["log_level"].as<int>();
	FLAGS_logtostderr = 1; // log only to console, no files

	// *************** Initializing Broker ********************** 
	bool is_head_node = false;
	bool replicate_to_memory = true;
	heartbeat_system::SequencerType sequencerType = heartbeat_system::SequencerType::EMBARCADERO;

	if (arguments.count("replicate_to_disk")) {
		replicate_to_memory = false;
	}

	if (arguments.count("scalog")) {
		sequencerType = heartbeat_system::SequencerType::SCALOG;
	} else if (arguments.count("corfu")) {
		sequencerType = heartbeat_system::SequencerType::CORFU;
	}

	if (arguments.count("head")) {
		is_head_node = true;
	} else if (arguments.count("follower")) {
		head_addr = arguments["follower"].as<std::string>();
	} else {
		LOG(INFO) << "head_addr is set to default: " << head_addr;
	}

	// Handle cgroup if requested
	int cgroup_id = arguments["run_cgroup"].as<int>();
	if (cgroup_id > 0) {
		if (!AttachCgroup(cgroup_id) || !CheckAvailableCores()) {
			LOG(ERROR) << "CGroup core throttle is wrong";
			return EXIT_FAILURE;
		}
	}

	// *************** Initialize managers ********************** 
	heartbeat_system::HeartBeatManager heartbeat_manager(is_head_node, head_addr);
	int broker_id = heartbeat_manager.GetBrokerId();
	size_t colon_pos = head_addr.find(':');
	std::string head_ip = head_addr.substr(0, colon_pos);

	LOG(INFO) << "Starting Embarlet broker_id: " << broker_id;

	Embarcadero::CXL_Type cxl_type = Embarcadero::CXL_Type::Real;
	if (arguments.count("emul")) {
		cxl_type = Embarcadero::CXL_Type::Emul;
		LOG(WARNING) << "Using emulated CXL";
	}

	int num_network_io_threads = arguments["network_threads"].as<int>();

	// Create and connect all manager components
	Embarcadero::CXLManager cxl_manager(broker_id, cxl_type, head_ip);
	Embarcadero::DiskManager disk_manager(broker_id, cxl_manager.GetCXLAddr(),
			replicate_to_memory, sequencerType);
	Embarcadero::NetworkManager network_manager(broker_id, num_network_io_threads);
	Embarcadero::TopicManager topic_manager(cxl_manager, disk_manager, broker_id);

	// Register callbacks
	heartbeat_manager.RegisterCreateTopicEntryCallback(
			std::bind(&Embarcadero::TopicManager::CreateNewTopic, &topic_manager,
				std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
				std::placeholders::_4, std::placeholders::_5));

	if (is_head_node) {
		cxl_manager.RegisterGetRegisteredBrokersCallback(
				std::bind(&heartbeat_system::HeartBeatManager::GetRegisteredBrokers, &heartbeat_manager,
					std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	}

	topic_manager.RegisterGetNumBrokersCallback(
			std::bind(&heartbeat_system::HeartBeatManager::GetNumBrokers, &heartbeat_manager));

	network_manager.RegisterGetNumBrokersCallback(
			std::bind(&heartbeat_system::HeartBeatManager::GetNumBrokers, &heartbeat_manager));

	// Connect managers
	cxl_manager.SetTopicManager(&topic_manager);
	cxl_manager.SetNetworkManager(&network_manager);
	disk_manager.SetNetworkManager(&network_manager);
	network_manager.SetCXLManager(&cxl_manager);
	network_manager.SetDiskManager(&disk_manager);

	// Signal initialization completion
	SignalScriptReady();
	LOG(INFO) << "Embarcadero initialized. Ready to go";

	// *************** Wait unless there's a failure ********************** 
	heartbeat_manager.Wait();

	LOG(INFO) << "Embarcadero Terminating";
	return EXIT_SUCCESS;
}
