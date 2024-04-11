#include "common/config.h"
#include "pub_queue.h"
#include "pub_task.h"
#include "peer.h"
#include "topic_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <set>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <emmintrin.h>

#include <thread>
//#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
void memcpy_nt(void* dst, const void* src, size_t size) {
    // Cast the input pointers to the appropriate types
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // Align the destination pointer to 16-byte boundary
    size_t alignment = reinterpret_cast<uintptr_t>(d) & 0xF;
    if (alignment) {
        alignment = 16 - alignment;
        size_t copy_size = (alignment > size) ? size : alignment;
        std::memcpy(d, s, copy_size);
        d += copy_size;
        s += copy_size;
        size -= copy_size;
    }

    // Copy the bulk of the data using non-temporal stores
    size_t block_size = size / 64;
    for (size_t i = 0; i < block_size; ++i) {
        _mm_stream_si64(reinterpret_cast<long long*>(d), *reinterpret_cast<const long long*>(s));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 8), *reinterpret_cast<const long long*>(s + 8));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 16), *reinterpret_cast<const long long*>(s + 16));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 24), *reinterpret_cast<const long long*>(s + 24));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 32), *reinterpret_cast<const long long*>(s + 32));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 40), *reinterpret_cast<const long long*>(s + 40));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 48), *reinterpret_cast<const long long*>(s + 48));
        _mm_stream_si64(reinterpret_cast<long long*>(d + 56), *reinterpret_cast<const long long*>(s + 56));
        d += 64;
        s += 64;
    }

    // Copy the remaining data using standard memcpy
    std::memcpy(d, s, size % 64);
}

size_t GetPhysicalCoreCount(){
	std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    std::set<std::pair<int, int>> coreIdentifiers; // Set to store unique (physical id, core id) pairs

    int physicalId = -1;
    int coreId = -1;

    while (std::getline(cpuinfo, line)) {
        std::istringstream iss(line);
        std::string key;
        if (getline(iss, key, ':')) {
            std::string value;
            getline(iss, value); // Read the rest of the line
            if (key.find("physical id") != std::string::npos) {
                physicalId = std::stoi(value);
            } else if (key.find("core id") != std::string::npos) {
                coreId = std::stoi(value);
            }

            // When we have both physical id and core id, insert them as a pair into the set
            if (physicalId != -1 && coreId != -1) {
                coreIdentifiers.insert(std::make_pair(physicalId, coreId));
                physicalId = -1; // Reset for the next processor entry
                coreId = -1;
            }
        }
    }

    return coreIdentifiers.size();
}

Embarcadero::TopicManager *t;
#define LOOPLEN 10000
void CXLWriteBandwidthTest(){
	Embarcadero::PublishRequest req;
	memset(req.topic, 0, 32);
	req.topic[0] = '0';
	req.client_id = 0;
	req.client_order = 1;
	req.size = 777;
	for(int i=0; i<LOOPLEN; i++){
		req.payload_address = malloc(1024);;
		t->PublishToCXL(req);
		free(req.payload_address);
	}
}

//Topic Manager Test
void RawCXLWriteTest(){
	int broker_id = 0;
	Embarcadero::CXLManager cxl_manager(200,broker_id);
	Embarcadero::TopicManager topic_manager(cxl_manager, broker_id);

	//********* Load Generate **************
	char topic[32];
	memset(topic, 0, 32);
	topic[0] = '0';
	topic_manager.CreateNewTopic(topic);

	std::cout << "Starting Topic Manager Test" << std::endl;
	double NUM_THREADS = 128;
    std::vector<std::thread> threads;
	t = &topic_manager;
    auto start = std::chrono::high_resolution_clock::now();
    for (double i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(CXLWriteBandwidthTest);
    }
    // Join threads
    for (double i = 0; i < NUM_THREADS; ++i) {
        threads[i].join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;

    double bytes_written =NUM_THREADS * (double)LOOPLEN/1024 ;
    double bandwidth = bytes_written / (duration.count() *1024); // Convert bytes to MB

    std::cout << "Runtime: " << duration.count() << std::endl;
    std::cout << "Internal Publish bandwidth: " << bandwidth << " GB/s" << std::endl;
}

int main(int argc, char* argv[]){
	//Initialize
	//size_t num_cores = GetPhysicalCoreCount();
	int broker_id = 0;
	Embarcadero::CXLManager cxl_manager(4000,broker_id);
	Embarcadero::DiskManager disk_manager(4000);
	Embarcadero::NetworkManager network_manager(4000, NUM_NETWORK_IO_THREADS);
	Embarcadero::TopicManager topic_manager(cxl_manager, broker_id);

	cxl_manager.SetTopicManager(&topic_manager);
	cxl_manager.SetNetworkManager(&network_manager);
	disk_manager.SetNetworkManager(&network_manager);
	network_manager.SetCXLManager(&cxl_manager);
	network_manager.SetDiskManager(&disk_manager);

	//********* Load Generate **************
	char topic[32];
	memset(topic, 0, 32);
	topic[0] = '0';
	topic_manager.CreateNewTopic(topic);

	std::cout << "You are now safe to go" << std::endl;
	cxl_manager.StartInternalTest();

	// Sequencer Test
	//sleep(2);
	//cxl_manager.Sequencer(topic);

	// Disk Manager Test

	/*
	char topic[32];
	memset(topic, 0, 32);
	topic[0] = '0';
	topic_manager.CreateNewTopic(topic);
	Embarcadero::PublishRequest req;
	std::atomic<int> c{2};
	memcpy(req.topic, topic, 32);
	req.payload_address = malloc(1024);
	req.counter = &c;
	req.size = 1024;
	req.acknowledge = true;
	cxl_manager.EnqueueRequest(req);
	disk_manager.EnqueueRequest(req);
	cxl_manager.GetMessageAddr(topic, last_offset, last_addr, messages, messages_size);
	*/

	// Network Manager Test
	/*
	char topic[32];
	memset(topic, 0, 32);
	topic[0] = '0';
	topic_manager.CreateNewTopic(topic);
	
	Embarcadero::NetworkRequest req;
	req.req_type = Embarcadero::Test;
	std::cout <<"Submitting reqs" << std::endl;
	for(int i =0; i<1000000; i++){
		network_manager.EnqueueRequest(req);
	}
		std::cout <<"Submitted all reqs" << std::endl;
	sleep(1);

	void* last_addr = nullptr;
	void* messages = nullptr;
	size_t messages_size;
	size_t last_offset = 0;
	cxl_manager.GetMessageAddr(topic, last_offset, last_addr, messages, messages_size);
	*/

	/*
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

	while(1){sleep(10);}
	return 0;
}
