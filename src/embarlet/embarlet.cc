#include <chrono>
#include <string>
#include <thread>
#include <functional>
#include <iostream>
#include <fstream>

// System includes
#include <csignal>
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
#include "common/configuration.h"
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
	// Use non-blocking mode to avoid deadlock if script isn't ready
	ScopedFD fd(open("script_signal_pipe", O_WRONLY | O_NONBLOCK));
	if (fd.isValid()) {
		const char* signal = "ready";
		ssize_t result = write(fd.get(), signal, strlen(signal));
		if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			VLOG(1) << "Failed to signal script readiness: " << strerror(errno);
		}
	} else {
		VLOG(1) << "Failed to open script_signal_pipe: " << strerror(errno);
	}

	// Also write a ready file for scripts to poll (more robust than named pipes)
	// File is named with PID to avoid conflicts between multiple test runs
	std::string ready_file = "/tmp/embarlet_" + std::to_string(getpid()) + "_ready";
	std::ofstream ready_stream(ready_file);
	if (ready_stream.is_open()) {
		ready_stream << "ready\n";
		ready_stream.close();
		VLOG(1) << "Wrote readiness signal to " << ready_file;
	} else {
		LOG(WARNING) << "Failed to write readiness signal file: " << ready_file;
	}
}

// [[GRACEFUL_SHUTDOWN]] Set by SIGTERM/SIGINT so main can request shutdown and let destructors run
// (Topic/NetworkManager set stop_threads_ in destructors → EpochSequencerThread2 drain runs).
static volatile std::sig_atomic_t g_shutdown_requested = 0;

static void ShutdownSignalHandler(int signum) {
	(void)signum;
	g_shutdown_requested = 1;
}

} // end of namespace

int main(int argc, char* argv[]) {
	// Initialize logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();

	// [[GRACEFUL_SHUTDOWN]] Handle SIGTERM/SIGINT so brokers can drain and exit cleanly
	std::signal(SIGTERM, ShutdownSignalHandler);
	std::signal(SIGINT, ShutdownSignalHandler);

	// Parse command line arguments
	std::string head_addr = "127.0.0.1:" + std::to_string(BROKER_PORT);
	cxxopts::Options options("Embarcadero", "A totally ordered pub/sub system with CXL");

	options.add_options()
		("head", "Head Node")
		("follower", "Follower Address and Port", cxxopts::value<std::string>())
		("scalog", "Run also as a Scalog Replica")
		("SCALOG", "Run also as a Scalog Replica")
		("corfu", "Run also as a Corfu Replica")
		("CORFU", "Run also as a Corfu Replica")
		("embarcadero", "Run as a Embarcadero Replica")
		("EMBARCADERO", "Run as a Embarcadero Replica")
		("e,emul", "Use emulation instead of CXL")
		("c,run_cgroup", "Run within cgroup", cxxopts::value<int>()->default_value("0"))
		("network_threads", "Number of network IO threads",
		 cxxopts::value<int>()->default_value(std::to_string(NUM_NETWORK_IO_THREADS)))
		("replicate_to_disk", "Replicate to Disk instead of Memory")
		("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
		("config", "Configuration file path", cxxopts::value<std::string>()->default_value("config/embarcadero.yaml"));

	auto arguments = options.parse(argc, argv);

	FLAGS_v = arguments["log_level"].as<int>();
	FLAGS_logtostderr = 1; // log only to console, no files

	// *************** Load Configuration *********************
	Embarcadero::Configuration& config = Embarcadero::Configuration::getInstance();
	std::string config_file = arguments["config"].as<std::string>();
	
	if (!config.loadFromFile(config_file)) {
		LOG(ERROR) << "Failed to load configuration from " << config_file;
		auto errors = config.getValidationErrors();
		for (const auto& error : errors) {
			LOG(ERROR) << "Config error: " << error;
		}
		return EXIT_FAILURE;
	}
	
	// Override configuration with command line arguments
	config.overrideFromCommandLine(argc, argv);
	
	LOG(INFO) << "Configuration loaded successfully from " << config_file;

	// *************** Initializing Broker ********************** 
	bool is_head_node = false;
	bool replicate_to_memory = true;
	heartbeat_system::SequencerType sequencerType = heartbeat_system::SequencerType::EMBARCADERO;

	if (arguments.count("replicate_to_disk")) {
		replicate_to_memory = false;
	}

	if (arguments.count("scalog") || arguments.count("SCALOG")) {
		sequencerType = heartbeat_system::SequencerType::SCALOG;
	} else if (arguments.count("corfu") || arguments.count("CORFU")) {
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

	// [[SEQUENCER_ONLY_HEAD_NODE]] - Check if this is a sequencer-only head node
	// Must be checked before HeartBeatManager is created so it can advertise accepts_publishes
	bool is_sequencer_node = config.isSequencerNode();

	// *************** Initialize managers **********************
	heartbeat_system::HeartBeatManager heartbeat_manager(is_head_node, head_addr, is_sequencer_node);
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
	// Note: is_sequencer_node was already fetched from config before HeartBeatManager creation
	Embarcadero::CXLManager cxl_manager(broker_id, cxl_type, head_ip);
	Embarcadero::DiskManager disk_manager(broker_id, cxl_manager.GetCXLAddr(),
			replicate_to_memory, sequencerType);
	// Skip networking on sequencer-only node (no client connections)
	Embarcadero::NetworkManager network_manager(broker_id, num_network_io_threads, is_sequencer_node);
	Embarcadero::TopicManager topic_manager(cxl_manager, disk_manager, broker_id, is_sequencer_node);

	// Register callbacks
	heartbeat_manager.RegisterCreateTopicEntryCallback(
			std::bind(&Embarcadero::TopicManager::CreateNewTopic, &topic_manager,
				std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
				std::placeholders::_4, std::placeholders::_5, std::placeholders::_6));

	if (is_head_node) {
		cxl_manager.RegisterGetRegisteredBrokersCallback(
				[&heartbeat_manager](absl::btree_set<int> &registered_brokers,
					Embarcadero::MessageHeader** msg_to_order,
					Embarcadero::TInode *tinode) -> int {
					return heartbeat_manager.GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
				});
	}

	topic_manager.RegisterGetNumBrokersCallback(
			std::bind(&heartbeat_system::HeartBeatManager::GetNumBrokers, &heartbeat_manager));

	topic_manager.RegisterGetRegisteredBrokersCallback(
			[&heartbeat_manager](absl::btree_set<int> &registered_brokers,
				Embarcadero::MessageHeader** msg_to_order,
				Embarcadero::TInode *tinode) -> int {
				return heartbeat_manager.GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			});


	network_manager.RegisterGetNumBrokersCallback(
			std::bind(&heartbeat_system::HeartBeatManager::GetNumBrokers, &heartbeat_manager));

	// Connect managers
	cxl_manager.SetTopicManager(&topic_manager);
	cxl_manager.SetNetworkManager(&network_manager);
	network_manager.SetCXLManager(&cxl_manager);
	network_manager.SetDiskManager(&disk_manager);
	network_manager.SetTopicManager(&topic_manager);

	// [[FIX: 9/12 subscriber connections]] Only signal "ready" when the data port is actually
	// listening. NetworkManager constructor returns when MainThread has *started*, but MainThread
	// may still be in bind()/listen() (bind retries with sleep(5)). Signaling ready before
	// listen() causes clients to connect too early → connection refused → one broker shows 0/3
	// subscriber connections (9/12 total). For sequencer-only nodes we skip networking so
	// IsListening() stays false; do not wait in that case.
	if (!is_sequencer_node) {
		const int listen_wait_seconds = 30;
		const int poll_ms = 100;
		int waited_ms = 0;
		while (!network_manager.IsListening() && waited_ms < listen_wait_seconds * 1000) {
			std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
			waited_ms += poll_ms;
			if (waited_ms % 5000 == 0 && waited_ms > 0) {
				LOG(INFO) << "Waiting for data port to listen (broker " << broker_id << ")... " << (waited_ms / 1000) << "s";
			}
		}
		if (!network_manager.IsListening()) {
			LOG(ERROR) << "Data port did not start listening within " << listen_wait_seconds << "s (broker " << broker_id << ")";
		}
	}

	// Signal initialization completion
	SignalScriptReady();
	LOG(INFO) << "Embarcadero initialized. Ready to go";

	// *************** Wait until shutdown requested (SIGTERM/SIGINT) **********************
	// Run Wait() in a thread so we can poll g_shutdown_requested and call RequestShutdown(),
	// allowing destructors to run (stop_threads_ set → EpochSequencerThread2 drain, etc.).
	std::thread wait_thread([&heartbeat_manager]() { heartbeat_manager.Wait(); });
	while (g_shutdown_requested == 0) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	LOG(INFO) << "Shutdown requested, stopping heartbeat and joining threads...";
	heartbeat_manager.RequestShutdown();
	if (wait_thread.joinable()) wait_thread.join();

	LOG(INFO) << "Embarcadero Terminating";
	return EXIT_SUCCESS;
}
