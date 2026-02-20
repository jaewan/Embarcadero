#include "corfu_sequencer.grpc.pb.h"
#include "common/config.h"
#include "../common/performance_utils.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <glog/logging.h>
#include <thread>
#include <csignal>
#include <chrono>
#include <errno.h>
#include <cstring>

#include "absl/container/flat_hash_map.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using corfusequencer::CorfuSequencer;
using corfusequencer::TotalOrderRequest;
using corfusequencer::TotalOrderResponse;

class CorfuSequencerImpl final : public CorfuSequencer::Service {
	public:
		CorfuSequencerImpl() {
			for (auto& v : idx_per_broker_) v.store(0, std::memory_order_relaxed);
			for (auto& v : batch_seq_per_broker_) v.store(0, std::memory_order_relaxed);
		}

		Status GetTotalOrder(ServerContext* context, const TotalOrderRequest* request,
				TotalOrderResponse* response) override {
			const uint64_t client_id = request->client_id();
			const uint64_t batch_seq = request->batchseq();
			const uint64_t num_msg = request->num_msg();
			const uint64_t total_size = request->total_size();
			const int broker_id = request->broker_id();

			// [[PHASE_8]] Bounds check for broker_id and sanity check for request fields
			if (broker_id < 0 || broker_id >= kMaxBrokers) {
				return Status(grpc::StatusCode::INVALID_ARGUMENT, "broker_id out of range");
			}
			if (num_msg == 0 || total_size == 0) {
				return Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid num_msg or total_size");
			}

			const uint64_t client_broker_key =
				(static_cast<uint64_t>(client_id) << 32) | static_cast<uint64_t>(broker_id);

			ClientBrokerState* state = GetOrCreateState(client_broker_key);

			std::unique_lock<std::mutex> client_lock(state->mu);

			if (batch_seq < state->expected_batch_seq) {
				return Status(grpc::StatusCode::INVALID_ARGUMENT, "Batch sequence already processed");
			}

			if (batch_seq != state->expected_batch_seq) {
				return Status(grpc::StatusCode::UNAVAILABLE, "Out of order batch sequence, please retry");
			}

			uint64_t start_order = next_order_.fetch_add(num_msg, std::memory_order_relaxed);
			uint64_t log_offset = idx_per_broker_[broker_id].fetch_add(total_size, std::memory_order_relaxed);
			uint64_t bbseq = batch_seq_per_broker_[broker_id].fetch_add(1, std::memory_order_relaxed);

			state->expected_batch_seq++;

			response->set_total_order(start_order);
			response->set_log_idx(log_offset);
			response->set_broker_batch_seq(bbseq);

			// [[PHASE_8]] CXL Flush Rule: Ensure all sequencer state changes are visible
			// Although this is a network service, we follow the project's consistency rule
			// for all ordering-critical state updates.
			Embarcadero::CXL::store_fence();

			return Status::OK;
		}

	private:
		static constexpr int kMaxBrokers = NUM_MAX_BROKERS;

		struct alignas(64) ClientBrokerState {
			std::mutex mu;
			uint64_t expected_batch_seq{0};
		};

		std::atomic<uint64_t> next_order_{0};
		std::array<std::atomic<uint64_t>, kMaxBrokers> idx_per_broker_;
		std::array<std::atomic<uint64_t>, kMaxBrokers> batch_seq_per_broker_;

		std::shared_mutex client_map_mu_;
		absl::flat_hash_map<uint64_t, std::unique_ptr<ClientBrokerState>> client_state_;

		ClientBrokerState* GetOrCreateState(uint64_t key) {
			{
				std::shared_lock<std::shared_mutex> rlock(client_map_mu_);
				auto it = client_state_.find(key);
				if (it != client_state_.end()) {
					return it->second.get();
				}
			}
			{
				std::unique_lock<std::shared_mutex> wlock(client_map_mu_);
				auto [it, inserted] = client_state_.emplace(key, nullptr);
				if (inserted) {
					it->second = std::make_unique<ClientBrokerState>();
				}
				return it->second.get();
			}
		}
};

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
