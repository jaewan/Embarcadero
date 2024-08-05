#include "../disk_manager/disk_manager.h"
#include "../network_manager/network_manager.h"
#include "../cxl_manager/cxl_manager.h"
#include <glog/logging.h>
#include "mimalloc.h"
#include <chrono>

#define NUM_TOPICS 1
#define LOOPLEN 250000
double NUM_THREADS = 32;

std::atomic<size_t> client_order_{0};

void TopicManagerWriteThread(int tid, Embarcadero::TopicManager &topic_manager){
	Embarcadero::PublishRequest req;
	memset(req.topic, 0, 31);
	std::sprintf(req.topic, "%d", tid%NUM_TOPICS);
	req.client_id = 0;
	req.acknowledge = 1;
	req.client_order = client_order_.fetch_add(1);
	req.size = 1024-64;
	for(int i=0; i<LOOPLEN; i++){
		req.payload_address = mi_malloc(1024);;
		Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)req.payload_address;
		header->client_id = 0;
		header->client_order = (tid*LOOPLEN)+i;
		header->size = req.size;
		header->total_order = 0;
		header->paddedSize = req.size;
		header->segment_header = nullptr;
		header->logical_offset = (size_t)-1; // Sentinel value
		header->next_message = nullptr;
		topic_manager.>PublishToCXL(req);
		mi_free(req.payload_address);
	}
}

// Topic Manager Test
// Send write requests directly to the topic manager.
void RawCXLWriteTest(Embarcadero::CXLManager &cxl_manager, Embarcadero::TopicManager topic_manager){
	double LOOPLEN = 25000;
	//********* Load Generate **************
	char topic[31];
	int order =2;
	for(int i=0; i<NUM_TOPICS; i++){
		memset(topic, 0, 31);
		std::sprintf(topic, "%d", i);
		cxl_manager.CreateNewTopic(topic, order);
	}

	LOG(INFO)<< "Starting Topic Manager Test";
	std::vector<std::thread> threads;
	auto start = std::chrono::high_resolution_clock::now();
	for (double i = 0; i < NUM_THREADS; ++i) {
		threads.emplace_back(TopicManagerWriteThread, i, topic_manager);
	}
	// Join threads
	for (double i = 0; i < NUM_THREADS; ++i) {
		threads[i].join();
	}

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> duration = end - start;

	double bytes_written =NUM_THREADS * (double)LOOPLEN/1024 ;
	double bandwidth = bytes_written / (duration.count() *1024); // Convert bytes to MB

	LOG(INFO) << "Runtime: " << duration.count();
	LOG(INFO) << "Internal Publish bandwidth: " << bandwidth << " GB/s";
	;
	sleep(2);
	size_t last_offset = (size_t)-2;
	void* last_addr = nullptr;
	void* messages;
	size_t messages_size;
	if(cxl_manager.GetMessageAddr(topic, last_offset, last_addr, messages, messages_size)){
		LOG(INFO) << "read :" << last_offset;
	}else{
		LOG(INFO) << "Did not read anything";
	}
	/*
		 size_t to_read_msg = LOOPLEN*1024*NUM_THREADS;
		 size_t off = 0;
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
	*/
}


// CXL Manager Test
// Send write and read requests and verify
void ReadWriteTest(Embarcadero::CXLManager &cxl_manager, Embarcadero::TopicManager topic_manager){
	char topic[31];
	memset(topic, 0, 31);
	topic[0] = '0';
	int order = 2;
	cxl_manager.CreateNewTopic(topic, order);

	std::string first_message = "testing write read";
	std::string second_message = "Second Message";

	Embarcadero::PublishRequest req;
	memset(req.topic, 0, 31);
	req.topic[0] = '0';
	req.client_id = 0;
	req.client_order = 1;
	req.size = 777;
	req.counter = (std::atomic<int>*)malloc(sizeof(std::atomic<int>));
	req.counter->store(1);

	req.payload_address = malloc(1024);;
	memcpy((uint8_t*)req.payload_address + 64, first_message, first_message.length());
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
	memcpy((uint8_t*)req1.payload_address + 64, second_message, second_message.length());
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
	LOF(INFO) << cxl_manager.GetMessageAddr(topic, last_offset, last_addr, messages, messages_size);
	LOG(INFO) << messages_size << std::endl;
	return ;
}

void SimulateNetworkManager(size_t message_size){
	Embarcadero::PublishRequest req;
	memset(req.topic, 0, 31);
	//std::sprintf(req.topic, "%d", tid%NUM_TOPICS);
	std::sprintf(req.topic, "%d", 0);
	req.client_id = 1;
	req.acknowledge = true;
	req.client_order = client_order_.fetch_add(1);
	req.size = message_size;
	int padding = message_size % 64;
	if(padding)
		padding = 64 - padding;
	size_t padded_size = message_size + padding + sizeof(Embarcadero::MessageHeader);
	for(int i=0; i<LOOPLEN; i++){
		req.payload_address = mi_malloc(padded_size);
		Embarcadero::MessageHeader *header = (Embarcadero::MessageHeader*)req.payload_address;
		header->client_id = 1;
		header->client_order = req.client_id;
		header->size = req.size;
		header->total_order = 0;
		header->paddedSize = padded_size;
		header->segment_header = nullptr;
		header->logical_offset = (size_t)-1; // Sentinel value
		header->next_message = nullptr;
		req.counter = (std::atomic<int>*)mi_malloc(sizeof(std::atomic<int>)); 
		req.counter->store(2);
		cxl_manager_->EnqueueRequest(req);
		disk_manager_->EnqueueRequest(req);
	}
}

//End to end test
void E2ETest(size_t message_size){
	LOG(INFO) << "Starting E2ETest";

	double bytes_written = NUM_THREADS * LOOPLEN * message_size;
	bytes_written = bytes_written/(double)(1024*1024);

	std::vector<std::thread> threads;
	auto start = std::chrono::high_resolution_clock::now();
	for (double i = 0; i < NUM_THREADS; ++i) {
		threads.emplace_back(SimulateNetworkManager, message_size);
	}
	LOG(INFO) << "Spawned network manger simulation";
	cxl_manager_->Wait1();
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> duration = end - start;
	//std::cout << " 1 MPMC bandwidth: " << bytes_written/duration.count() << " MB/s" << std::endl;
	VLOG(3)<< " 1 MPMC bandwidth: " << bytes_written/duration.count() << " MB/s";
	// Join threads
	for (double i = 0; i < NUM_THREADS; ++i) {
		threads[i].join();
	}
	LOG(INFO) << "Enqueued all reqs. Waiting for ack...";

	cxl_manager_->Wait2();
	end = std::chrono::high_resolution_clock::now();
	duration = end - start;
	VLOG(3)<< " 2 MPMC bandwidth: " << bytes_written/duration.count() << " MB/s";
	//std::cout << " 2 MPMC bandwidth: " << bytes_written/duration.count() << " MB/s" << std::endl;
	network_manager_->WaitUntilAcked();

	end = std::chrono::high_resolution_clock::now();
	duration = end - start;

	double bandwidth = bytes_written / (duration.count()); // Convert bytes to MB

	std::cout << "Runtime: " << duration.count() << std::endl;
	std::cout << "Internal Publish bandwidth: " << bandwidth << " MB/s" << std::endl;
}

