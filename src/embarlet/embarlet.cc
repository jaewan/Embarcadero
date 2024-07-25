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

Embarcadero::TopicManager *t;
// #define LOOPLEN 250000
#define LOOPLEN 5
#define NUM_TOPICS 1
double NUM_THREADS = 40;
void CXLWriteBandwidthTest(int tid){
	Embarcadero::PublishRequest req;
	memset(req.topic, 0, 31);
	req.topic[0] = '0';
	std::sprintf(req.topic, "%d", tid%NUM_TOPICS);
	req.client_id = 0;
	req.acknowledge = 1;
	req.client_order = 1;
	req.size = 1024-64;
	for(int i=0; i<LOOPLEN; i++){
		req.payload_address = malloc(1024);;
		Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)req.payload_address;
		header->client_id = 0;
		header->client_order = (tid*LOOPLEN)+i;
		header->size = req.size;
		header->total_order = 0;
		header->paddedSize = req.size;
		header->segment_header = nullptr;
		header->logical_offset = (size_t)-1; // Sentinel value
		header->next_message = nullptr;
		t->PublishToCXL(req);
		free(req.payload_address);
	}
}

void ScalogOrderTest(Embarcadero::CXLManager *cxl_manager, char* topic) {
	std::cout << "Starting ScalogOrderTest" << std::endl;

    std::vector<std::thread> threads;
	double num_threads = 1;
    auto start = std::chrono::high_resolution_clock::now();
    for (double i = 0; i < num_threads; ++i) {
        threads.emplace_back(CXLWriteBandwidthTest, i);
    }
    // Join threads
    for (double i = 0; i < num_threads; ++i) {
        threads[i].join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;

    double bytes_written =NUM_THREADS * (double)LOOPLEN/1024 ;
    double bandwidth = bytes_written / (duration.count() *1024); // Convert bytes to MB

    std::cout << "Runtime: " << duration.count() << std::endl;
    std::cout << "Internal Publish bandwidth: " << bandwidth << " GB/s" << std::endl;

	sleep(2);
	size_t last_offset = (size_t)-2;
	void* last_addr = nullptr;
	void* messages;
	size_t messages_size;
		if(cxl_manager->GetMessageAddr(topic, last_offset, last_addr, messages, messages_size)){
			std::cout << "read :" << last_offset<< std::endl;
		}else{
			std::cout << "Did not read anything" << std::endl;
		}

	// Print global cut
	absl::flat_hash_map<std::string, std::vector<int>> global_cut_map = cxl_manager->ScalogGetGlobalCut();
	std::vector<int> global_cut = global_cut_map[topic];

	// print global cut
	std::cout << "Contents of global_cut for topic \"" << topic << "\":" << std::endl;
	for (int value : global_cut) {
		std::cout << value << " ";
	}
	std::cout << std::endl;  // To add a newline at the end
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
	// Queue Size (1UL<<22)(1UL<<25)(1UL<<25) respectly performed 6GB/s 1kb message disk thread:8 cxl:16 network: 32
	Embarcadero::CXLManager cxl_manager((1UL<<22), broker_id);
	Embarcadero::DiskManager disk_manager((1UL<<25));
	Embarcadero::NetworkManager network_manager((1UL<<25), broker_id, NUM_NETWORK_IO_THREADS, false);
	Embarcadero::TopicManager topic_manager(cxl_manager, broker_id);
	heartbeat_manager.RegisterCreateTopicEntryCallback(std::bind(&Embarcadero::TopicManager::CreateNewTopic, &topic_manager, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

	cxl_manager.SetBroker(&heartbeat_manager);
	cxl_manager.SetTopicManager(&topic_manager);
	cxl_manager.SetNetworkManager(&network_manager);
	disk_manager.SetNetworkManager(&network_manager);
	network_manager.SetCXLManager(&cxl_manager);
	network_manager.SetDiskManager(&disk_manager);

	// //********* Load Generate **************
	// char topic[31];
	// memset(topic, 0, 31);
	// topic[0] = '0';
	// int order = 0;
	// topic_manager.CreateNewTopic(topic, order);

	// //********* Load Generate For Scalog **************
	// char topic[31];
	// memset(topic, 0, 31);
	// topic[0] = '0';
	// if (is_head_node) {
	// 	int order = 1;
	// 	cxl_manager.CreateNewTopic(topic, order, Embarcadero::Scalog);
	// }


	// t = &topic_manager;
	// ScalogOrderTest(&cxl_manager, topic);

	LOG(INFO) << "You are now safe to go";
	//cxl_manager.StartInternalTest();
	
	// *************** Wait unless there's a failure ********************** 
	heartbeat_manager.Wait();

	return 0;
}
