#include "scalog_replication_client.h"

#include <grpcpp/grpcpp.h>
#include <glog/logging.h>

#include <chrono>
#include <thread>
#include <random>

namespace Scalog {

ScalogReplicationClient::ScalogReplicationClient(const char* topic, size_t replication_factor, const std::string& address, int broker_id)
	: topic_(topic), replication_factor_(replication_factor), broker_id_(broker_id) {

		// Set the server address
		server_address_ = address + ":" + std::to_string(SCALOG_REP_PORT + broker_id);

		// Initialize random generator for exponential backoff
		{
			std::lock_guard<std::mutex> lock(rng_mutex_);
			random_engine_ = std::mt19937(std::random_device{}());
		}

		// Initialize channel and stub under mutex protection
		{
			std::lock_guard<std::mutex> lock(mutex_);
			CreateChannelLocked();
		}
	}

ScalogReplicationClient::~ScalogReplicationClient() {
	// No need to explicitly clean up channel or stub
	// They will be released by their respective smart pointers
}

bool ScalogReplicationClient::Connect(int timeout_seconds) {
	LOG(INFO) << "Attempting to connect to server at " << server_address_ << " ...";

	// Quick check without lock
	if (is_connected_.load(std::memory_order_acquire)) {
		return true;
	}

	// Acquire lock for connection attempt
	std::lock_guard<std::mutex> lock(mutex_);

	// Double-check after acquiring lock
	if (is_connected_.load(std::memory_order_relaxed)) {
		return true;
	}

	// Check if we need to recreate the channel
	if (!channel_ || !stub_) {
		CreateChannelLocked();
	}

	// Wait for the channel to connect
	auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);

	bool connected = channel_->WaitForConnected(deadline);
	if (connected) {
		is_connected_.store(true, std::memory_order_release);
	} else {
		LOG(ERROR) << "Failed to connect to server at " << server_address_ << " within timeout";
	}

	return connected;
}

bool ScalogReplicationClient::ReplicateData(size_t offset, size_t size, size_t num_msg, void* data,
		int max_retries) {
	if (!EnsureConnected()) {
		// Try to reconnect - this is thread-safe
		if (!Reconnect()) {
			return false;
		}
	}

	// Create request - no shared state accessed here
	scalogreplication::ScalogReplicationRequest request;
	request.set_offset(offset);
	request.set_data(std::string(static_cast<char*>(data), size));
	request.set_size(size);
	request.set_num_msg(num_msg);

	// Create response object - local to this call
	scalogreplication::ScalogReplicationResponse response;

	bool success = false;

	// Get a reference to the stub for thread-safe access
	std::unique_ptr<scalogreplication::ScalogReplicationService::Stub> local_stub;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!stub_) {
			return false;
		}
		// Create a new stub instance using the same channel
		local_stub = scalogreplication::ScalogReplicationService::NewStub(channel_);
	}

	// Retry loop
	for (int retry = 0; retry <= max_retries; retry++) {
		if (retry > 0) {
			LOG(INFO) << "Retry attempt " << retry << " for request ID: " << offset;

			// Calculate backoff with jitter - thread-safe
			int sleep_ms = CalculateBackoffMs(retry);
			std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

			// Check connection before retry - thread-safe
			if (!is_connected_.load(std::memory_order_acquire)) {
				if (!Reconnect()) {
					continue;
				}
			}
		}

		// Create new context for each attempt
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

		// Call the RPC using our thread-local stub copy
		grpc::Status status = local_stub->Replicate(&context, request, &response);

		// Handle response
		if (status.ok()) {
			if (response.success()) {
				success = true;
				break; // Exit retry loop on success
			} else {
				LOG(ERROR) << "Replication failed for ID " << offset;
				// Continue with retry if server reported failure
			}
		} else {
			LOG(ERROR) << "RPC failed for ID " << offset << ": " << status.error_code()
				<< ": " << status.error_message();

			// Mark as disconnected on RPC failure
			is_connected_.store(false, std::memory_order_release);

			// Don't retry if the error is not retriable
			if (status.error_code() == grpc::StatusCode::INVALID_ARGUMENT ||
					status.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
					status.error_code() == grpc::StatusCode::UNAUTHENTICATED) {
				break;
			}
		}
	}

	return success;
}

bool ScalogReplicationClient::IsConnected() const {
	return is_connected_.load(std::memory_order_acquire);
}

bool ScalogReplicationClient::Reconnect(int timeout_seconds) {
	// Check if reconnection is already in progress by another thread
	bool expected = false;
	if (!reconnection_in_progress_.compare_exchange_strong(expected, true,
				std::memory_order_acq_rel)) {
		// Another thread is already reconnecting, wait for it
		std::lock_guard<std::mutex> lock(reconnect_mutex_);
		// By the time we get the lock, reconnection should be complete
		return is_connected_.load(std::memory_order_acquire);
	}

	// We are responsible for reconnection
	{
		std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex_);

		LOG(INFO) << "Attempting to reconnect to server at " << server_address_ << "...";
		is_connected_.store(false, std::memory_order_release);

		// Recreate channel and stub
		{
			std::lock_guard<std::mutex> lock(mutex_);
			CreateChannelLocked();
		}

		bool connected = Connect(timeout_seconds);

		// Mark reconnection as complete
		reconnection_in_progress_.store(false, std::memory_order_release);

		return connected;
	}
}

void ScalogReplicationClient::CreateChannelLocked() {
	// This method should be called with mutex_ already locked
	channel_ = grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials());
	stub_ = scalogreplication::ScalogReplicationService::NewStub(channel_);
}

bool ScalogReplicationClient::EnsureConnected() {
	// Use relaxed ordering for first check as this is just an optimization
	if (!is_connected_.load(std::memory_order_relaxed)) {
		return Connect();
	}
	return true;
}

int ScalogReplicationClient::CalculateBackoffMs(int retry_attempt) {
	// Base delay: 100ms, max delay: 5000ms
	const int base_delay_ms = 100;
	const int max_delay_ms = 5000;

	// Calculate exponential backoff
	int delay = std::min(max_delay_ms, base_delay_ms * (1 << retry_attempt));

	// Add jitter (0-20% of delay) in a thread-safe manner
	int jitter;
	{
		std::lock_guard<std::mutex> lock(rng_mutex_);
		std::uniform_int_distribution<int> dist(0, delay / 5);
		jitter = dist(random_engine_);
	}

	return delay + jitter;
}

} // End of namespace Scalog
