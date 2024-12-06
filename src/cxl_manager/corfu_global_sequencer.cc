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
					batch_seq_per_clients_[client_id] = 0;  // Always start from 0
					pending_requests_[client_id] = PriorityQueue();
				}
				if(idx_per_broker_.find(broker_id) == idx_per_broker_.end()){
					idx_per_broker_[broker_id] = 0;
				}

				// Check if this batch_seq has already been processed
				if (batch_seq < batch_seq_per_clients_[client_id]) {
					LOG(WARNING) << "Duplicate or already processed batch_seq " << batch_seq
						<< " for client " << client_id;
					return Status(grpc::StatusCode::INVALID_ARGUMENT, "Batch sequence already processed");
				}

				// If this is not the next expected batch sequence, queue it
				if (batch_seq != batch_seq_per_clients_[client_id]) {
					std::promise<std::pair<uint64_t, uint64_t>> promise;
					auto future = promise.get_future();

					// Queue the request
					pending_requests_[client_id].push(std::make_unique<PendingRequest>(PendingRequest{
								batch_seq, std::move(promise), num_msg, broker_id, total_size}));

					// Release the lock while waiting
					lock.unlock();

					// Wait for this request's turn
					try {
						auto result = future.get();
						response->set_total_order(result.first);
						response->set_log_idx(result.second);
						return grpc::Status::OK;
					} catch (const std::exception& e) {
						LOG(ERROR) << "Error waiting for future: " << e.what();
						return Status(grpc::StatusCode::INTERNAL, e.what());
					}
				}

				// Process the current request (this is the expected batch_seq)
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
				// Higher batch_seq has lower priority (reverse order for priority_queue)
				return batch_seq > other.batch_seq;
			}
		};
		struct ComparePendingRequestPtr {
			bool operator()(const std::unique_ptr<PendingRequest>& a,
					const std::unique_ptr<PendingRequest>& b) const
			{
				return a->batch_seq > b->batch_seq;
			}
		};

		using PriorityQueue = std::priority_queue<std::unique_ptr<PendingRequest>,
					std::vector<std::unique_ptr<PendingRequest>>,
					ComparePendingRequestPtr>;

		void ProcessPendingRequests(size_t client_id) {
        auto& queue = pending_requests_[client_id];

        while (!queue.empty() &&
               queue.top()->batch_seq == batch_seq_per_clients_[client_id]) {
            // Access the top request
            std::unique_ptr<PendingRequest> pending = std::move(const_cast<std::unique_ptr<PendingRequest>&>(queue.top()));
            queue.pop();

            // Fulfill the promise for the request
            pending->promise.set_value({next_order_, idx_per_broker_[pending->broker_id]});

            // Update the next order and broker index
            next_order_ += pending->num_msg;
            idx_per_broker_[pending->broker_id] += pending->total_size;

            // Increment the client's batch sequence
            batch_seq_per_clients_[client_id]++;
        }
    }

    std::mutex mutex_;
    absl::flat_hash_map<size_t, size_t> batch_seq_per_clients_; // Tracks next expected batch_seq per client
    absl::flat_hash_map<size_t, size_t> idx_per_broker_;        // Tracks log index per broker
    absl::flat_hash_map<size_t, PriorityQueue> pending_requests_; // Pending requests per client
    absl::flat_hash_map<size_t, uint64_t> client_order_offset_;  // Tracks starting offset for each client
    size_t next_order_ = 0; // The next global order value
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
