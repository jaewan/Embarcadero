#include "corfu_sequencer.grpc.pb.h"
#include "common/config.h"
#include "absl/container/flat_hash_map.h"

#include <grpcpp/grpcpp.h>

#include <mutex>
#include <future>
#include <string>
#include <queue>
#include <condition_variable>
#include <glog/logging.h>
#include <thread> 
#include <csignal>
#include <chrono> 
#include <errno.h> 
#include <cstring> 

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using corfusequencer::CorfuSequencer;
using corfusequencer::TotalOrderRequest;
using corfusequencer::TotalOrderResponse;

class CorfuSequencerImpl final : public CorfuSequencer::Service {
	public:
		CorfuSequencerImpl() {}

		Status GetTotalOrder(ServerContext* context, const TotalOrderRequest* request,
				TotalOrderResponse* response) override {
			size_t client_id = request->client_id();
			size_t batch_seq = request->batchseq();
			size_t num_msg = request->num_msg();
			size_t total_size = request->total_size();
			int broker_id = request->broker_id();

			{
				std::unique_lock<std::mutex> lock(mutex_);

				// Initialize client's batch sequence if this is the first request
				if (batch_seq_per_clients_.find(client_id) == batch_seq_per_clients_.end()) {
					if (batch_seq != 0) {
						LOG(ERROR) << "First batch sequence must be 0. given batch:" << batch_seq;
						return Status(grpc::StatusCode::INVALID_ARGUMENT,
								"First batch sequence must be 0");
					}
					batch_seq_per_clients_[client_id] = 0;
					pending_requests_[client_id] = std::priority_queue<std::unique_ptr<PendingRequest>, std::vector<std::unique_ptr<PendingRequest>>, ComparePendingRequestPtr>();
				}
				if (idx_per_broker_.find(broker_id) == idx_per_broker_.end()){
					idx_per_broker_[broker_id] = 0;
				}

				// If this is not the next expected batch sequence, queue it
				if (batch_seq != batch_seq_per_clients_[client_id]) {
					if (batch_seq < batch_seq_per_clients_[client_id]) {
						LOG(ERROR) << "Batch sequence " << batch_seq << 
							" already processed. Current batch:" << batch_seq_per_clients_[client_id];
						return Status(grpc::StatusCode::INVALID_ARGUMENT,
								"Batch sequence already processed");
					}

					// Create promise and future for this request
					std::promise<std::pair<uint64_t, uint64_t>> promise;
					auto future = promise.get_future();

					// Queue the request
					pending_requests_[client_id].push(std::make_unique<PendingRequest>(PendingRequest{
								batch_seq,
								std::move(promise),
								num_msg,
								broker_id,
								total_size
								}));

					// Release lock while waiting
					lock.unlock();

					// Wait for our turn
					try {
						auto result = future.get();
						response->set_total_order(result.first);
						response->set_log_idx(result.second);
						return grpc::Status::OK;
					} catch (const std::exception& e) {
						LOG(ERROR) << "Waiting for future error:" << strerror(errno);
						return Status(grpc::StatusCode::INTERNAL, e.what());
					}
				}

				response->set_total_order(next_order_);
				response->set_log_idx(idx_per_broker_[broker_id]);

				next_order_ += num_msg;
				idx_per_broker_[broker_id] += total_size;
				batch_seq_per_clients_[client_id]++;

				// Process any pending requests that are now ready
				ProcessPendingRequests(client_id);
			}

			return Status::OK;
		}

	private:
		struct PendingRequest {
			size_t batch_seq;
			std::promise<std::pair<uint64_t, uint64_t>> promise;
			size_t num_msg;
			int broker_id;
			size_t total_size;

			// Comparison operator for priority queue (lower batch_seq has higher priority)
			bool operator<(const PendingRequest& other) const {
				return batch_seq < other.batch_seq;
			}
		};
		// Custom comparator for unique_ptr<PendingRequest>
		struct ComparePendingRequestPtr {
			bool operator()(const std::unique_ptr<PendingRequest>& a, const std::unique_ptr<PendingRequest>& b) const {
				return a->batch_seq < b->batch_seq; 
			}
		};
		void ProcessPendingRequests(size_t client_id) {
			auto& queue = pending_requests_[client_id];

			while (!queue.empty() &&
					queue.top()->batch_seq == batch_seq_per_clients_[client_id]) {

				std::unique_ptr<PendingRequest> pending = std::move(const_cast<std::unique_ptr<PendingRequest>&>(queue.top()));
				queue.pop();

				// Fulfill the promise (access members using the arrow operator)
				pending->promise.set_value({next_order_, idx_per_broker_[pending->broker_id]});

				next_order_ += pending->num_msg;
				idx_per_broker_[pending->broker_id] += pending->total_size;
				batch_seq_per_clients_[client_id]++;
			}
		}


		std::mutex mutex_;
		absl::flat_hash_map<size_t, size_t> batch_seq_per_clients_;
		absl::flat_hash_map<size_t, size_t> idx_per_broker_;;
		absl::flat_hash_map<size_t, std::priority_queue<std::unique_ptr<PendingRequest>, std::vector<std::unique_ptr<PendingRequest>>, ComparePendingRequestPtr>> pending_requests_;
		size_t next_order_ = 0;
};

void RunServer() {
	// Read the port number from config.h
	const std::string server_address = "0.0.0.0:" + std::to_string(CORFU_SEQ_PORT);

	// Create an instance of the service implementation
	CorfuSequencerImpl service;

	// Create a gRPC server
	grpc::ServerBuilder builder;

	// Set the server to listen on the specified address and port
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

	// Register the service implementation with the server
	builder.RegisterService(&service);

	// Build the server
	std::unique_ptr<Server> server(builder.BuildAndStart());
	if (!server) {
		LOG(ERROR) << "Failed to start the server on port " << CORFU_SEQ_PORT;
		return;
	}

	LOG(INFO) << "Server listening on " << server_address;

	// Set up signal handler for graceful shutdown
	std::signal(SIGINT, [](int signal) {
			LOG(INFO) << "Received shutdown signal";
			exit(0);
			});

	// Wait for the server to shut down
	server->Wait();
}

int main(int argc, char** argv) {
	// Initialize Logging
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();
	FLAGS_logtostderr = 1; // Log only to console (no files)

	// Run the server
	RunServer();

	return 0;
}
