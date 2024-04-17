#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <emmintrin.h>
#include <cxxopts.hpp> // https://github.com/jarro2783/cxxopts
#include <glog/logging.h>
#include <thread>

#include "common/config.h"
#include "peer.h"
#include "topic_manager.h"
#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include "ack_queue.h"
#include "req_queue.h"

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

/*
Embarcadero::TopicManager *t;
#define LOOPLEN 100
#define NUM_TOPICS 1
double NUM_THREADS = 1;
void CXLWriteBandwidthTest(int tid){
	Embarcadero::PublishRequest req;
	memset(req.topic, 0, 31);
	tid = tid%NUM_TOPICS;
	std::sprintf(req.topic, "%d", tid);
	req.client_id = 0;
	req.client_order = 1;
	req.size = 1024-64;
	for(int i=0; i<LOOPLEN; i++){
		req.payload_address = malloc(1024);;
		Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)req.payload_address;
		header->client_id = 0;
		header->client_order = i;
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

//Topic Manager Test
void RawCXLWriteTest(){
	int broker_id = 0;
	Embarcadero::CXLManager cxl_manager(200,broker_id);
	Embarcadero::TopicManager topic_manager(cxl_manager, broker_id);
	cxl_manager.SetTopicManager(&topic_manager);

	// ********* Load Generate **************
	char topic[31];
	int order =1;
	for(int i=0; i<NUM_TOPICS; i++){
		memset(topic, 0, 31);
		std::sprintf(topic, "%d", i);
		cxl_manager.CreateNewTopic(topic, order);
	}

	std::cout << "Starting Topic Manager Test" << std::endl;
    std::vector<std::thread> threads;
	t = &topic_manager;
    auto start = std::chrono::high_resolution_clock::now();
    for (double i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(CXLWriteBandwidthTest, i);
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

	sleep(2);
	size_t last_offset = (size_t)-2;
	void* last_addr = nullptr;
	void* messages;
	size_t messages_size;
	size_t off = 0;
	size_t to_read_msg = LOOPLEN*1024*NUM_THREADS;
	while(to_read_msg > 0){
		if(cxl_manager.GetMessageAddr(topic, last_offset, last_addr, messages, messages_size)){
			Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)messages;
			for(off; off<last_offset; off++){
				//std::cout << header->total_order << std::endl;
			}
			to_read_msg -= messages_size;
		}else{
			std::cout << std::endl;
		}
	};
}

void ReadWriteTest(){
	int broker_id = 0;
	Embarcadero::CXLManager cxl_manager(4000,broker_id, 4);
	Embarcadero::TopicManager topic_manager(cxl_manager, broker_id);

	cxl_manager.SetTopicManager(&topic_manager);

	char topic[31];
	memset(topic, 0, 31);
	topic[0] = '0';
	int order = 1;
	cxl_manager.CreateNewTopic(topic, order);

	Embarcadero::PublishRequest req;
	memset(req.topic, 0, 31);
	req.topic[0] = '0';
	req.client_id = 0;
	req.client_order = 1;
	req.size = 777;
	req.counter = (std::atomic<int>*)malloc(sizeof(std::atomic<int>));
	req.counter->store(1);

	req.payload_address = malloc(1024);;
	memcpy(req.payload_address + 64, "testing write read", 18);
					Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)req.payload_address;
					header->client_id = 0;
					header->client_order = 0;
					header->size = 777;
					header->total_order = 0;
					header->paddedSize = 64 - (777 % 64) + 777;
					header->segment_header = nullptr;
					header->logical_offset = (size_t)-1; // Sentinel value
					header->next_message = nullptr;
	cxl_manager.EnqueueRequest(req);

	Embarcadero::PublishRequest req1;
	memset(req1.topic, 0, 31);
	req1.topic[0] = '0';
	req1.client_id = 0;
	req1.client_order = 2;
	req1.size = 777;
	req1.payload_address = malloc(1024);;
	req1.counter = (std::atomic<int>*)malloc(sizeof(std::atomic<int>));
	req1.counter->store(1);
	memcpy(req1.payload_address + 64, "Second Message", 14);
					header = (Embarcadero::MessageHeader*)req1.payload_address;
					header->client_id = 0;
					header->client_order = 1;
					header->size = 777;
					header->total_order = 0;
					header->paddedSize = 64 - (777 % 64) + 777;
					header->segment_header = nullptr;
					header->logical_offset = (size_t)-1; // Sentinel value
					header->next_message = nullptr;
	cxl_manager.EnqueueRequest(req1);

	size_t last_offset = (size_t)-2;
	void* last_addr = nullptr;
	void* messages;
	size_t messages_size;
	sleep(2);
	std::cout << cxl_manager.GetMessageAddr(topic, last_offset, last_addr, messages, messages_size)
	<< std::endl;
	std::cout << messages_size << std::endl;
	return ;
}
*/

int main(int argc, char* argv[]){
  // Initialize logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();

	//size_t num_cores = GetPhysicalCoreCount();
  	cxxopts::Options options("Embarcadero", "A totally ordered pub/sub system with CXL");
	// Ex: you can add arguments on command line like ./embarcadero --head or ./embarcadero --follower="10.182.0.4:8080"
  	options.add_options()
		("head", "Head Node")
			("follower", "Follower Address and Port", cxxopts::value<std::string>())
			("e,emul", "Use emulation instead of CXL")
			("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
		;
	
	auto arguments = options.parse(argc, argv);

	FLAGS_v = arguments["log_level"].as<int>();
	//FLAGS_logtostderr = 1; // log only to console, no files.
	FLAGS_log_dir = "/tmp/vlog2_log";
	
	Embarcadero::CXL_Type cxl_type = Embarcadero::CXL_Type::Real;
	if (arguments.count("emul")) {
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


	//********* Load Generate **************
	char topic[31];
	memset(topic, 0, 31);
	topic[0] = '0';
	int order = 0;
	topic_manager.CreateNewTopic(topic, order);

	LOG(INFO) << "You are now safe to go ";
	//cxl_manager.StartInternalTest();


	while(true){
		sleep(60);
	}
	return 0;
}
