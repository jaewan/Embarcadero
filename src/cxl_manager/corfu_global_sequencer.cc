#include "cxl_manager/corfu_sequencer_service.h"

#include "common/config.h"

#include <grpcpp/grpcpp.h>

#include <csignal>
#include <memory>
#include <string>
#include <glog/logging.h>

using grpc::Server;
using grpc::ServerBuilder;

void RunServer() {
	const std::string server_address = "0.0.0.0:" + std::to_string(CORFU_SEQ_PORT);

	CorfuSequencerImpl service;

	grpc::ServerBuilder builder;
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
	builder.RegisterService(&service);

	std::unique_ptr<Server> server(builder.BuildAndStart());
	if (!server) {
		LOG(ERROR) << "Failed to start the server on port " << CORFU_SEQ_PORT;
		return;
	}

	LOG(INFO) << "Server listening on " << server_address;

	std::signal(SIGINT, [](int signal) {
		(void)signal;
		LOG(INFO) << "Received shutdown signal";
		exit(0);
	});

	server->Wait();
}

int main(int argc, char** argv) {
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();
	FLAGS_logtostderr = 1;

	RunServer();

	return 0;
}
