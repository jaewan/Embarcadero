#include "distributed_kv_store.h"

int main(int argc, char* argv[]) {
	// Initialize logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();
	FLAGS_logtostderr = 1; // log only to console, no files.

	// Setup command line options
	cxxopts::Options options("KV-test", "Distributed Key-value Store Test");
	options.add_options()
		("l,log_level", "Log level", cxxopts::value<int>()->default_value("1"))
		("sequencer", "Sequencer Type: Embarcadero(0), Kafka(1), Scalog(2), Corfu(3)", 
		 cxxopts::value<std::string>()->default_value("EMBARCADERO"));

	auto result = options.parse(argc, argv);

	SequencerType seq_type = parseSequencerType(result["sequencer"].as<std::string>());
	FLAGS_v = result["log_level"].as<int>();

	DistributedKVStore kvStore(1, seq_type, 4);

	size_t opId = kvStore.put("key1", "value1");
	size_t opId1 = kvStore.put("key2", "loooooooooong value1");
	std::this_thread::sleep_for(std::chrono::seconds(1));

	VLOG(3) <<"ending";

	return 0;
}
