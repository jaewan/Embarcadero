#include "scalog_replication_manager.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/alarm.h>
#include <glog/logging.h>

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "scalog_replication.grpc.pb.h"

namespace Scalog {

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using scalogreplication::ScalogReplicationService;
using scalogreplication::ScalogReplicationRequest;
using scalogreplication::ScalogReplicationResponse;

class ScalogReplicationServiceImpl final : public ScalogReplicationService::Service {
	public:
		explicit ScalogReplicationServiceImpl(std::string base_filename)
			: base_filename_(std::move(base_filename)), running_(true) {
				if (!OpenOutputFile()) {
					throw std::runtime_error("Failed to open replication file: " + base_filename_);
				}
				//TODO(Tony) spawn a new thread to send local cut in certain interval (as we already do in primary node) and give order to written messages
			}

		~ScalogReplicationServiceImpl() override {
			Shutdown();
		}

		void Shutdown() {
			bool expected = true;
			if (running_.compare_exchange_strong(expected, false)) {
				std::lock_guard<std::mutex> lock(file_mutex_);
				CloseOutputFile();
			}
		}

		Status Replicate(ServerContext* context, const ScalogReplicationRequest* request,
				ScalogReplicationResponse* response) override {
			if (!running_) {
				return CreateErrorResponse(response, "Service is shutting down", Status::CANCELLED);
			}

			// Increment request count
			replicate_count_.fetch_add(1, std::memory_order_relaxed);

			// Log the current replicate count
			LOG(INFO) << "Number of replication requests: " << replicate_count_.load(std::memory_order_relaxed);

			// Lock only for file operations
			std::lock_guard<std::mutex> lock(file_mutex_);

			// Double-check running flag after acquiring the lock (check-then-act pattern)
			if (!running_) {
				return CreateErrorResponse(response, "Service is shutting down", Status::CANCELLED);
			}

			try {
				if (fd_ == -1) {
					if (!ReopenOutputFile()) {
						return CreateErrorResponse(response, "Failed to reopen file", Status::CANCELLED);
					}
				}

				WriteRequest(*request);

				response->set_success(true);
				return Status::OK;
			} catch (const std::exception& e) {
				LOG(ERROR) << "Exception during replication: " << e.what();
				return CreateErrorResponse(response, std::string("Error: ") + e.what(), Status::CANCELLED);
			}
		}

	private:
		bool OpenOutputFile() {
			fd_ = open(base_filename_.c_str(), O_WRONLY | O_CREAT, 0644);
			if (fd_ == -1) {
				LOG(ERROR) << "Failed to open file: " << strerror(errno);
				return false;
			}
			return true;
		}

		bool ReopenOutputFile() {
			CloseOutputFile();
			return OpenOutputFile();
		}

		void CloseOutputFile() {
			if (fd_ != -1) {
				close(fd_);
				fd_ = -1;
			}
		}

		void WriteRequest(const ScalogReplicationRequest& request) {
			const auto& data = request.data();
			int64_t offset = request.offset();
			int64_t size = request.size();

			if (data.size() != static_cast<size_t>(size)) {
				throw std::runtime_error("Size mismatch: request.size() = " +
						std::to_string(size) + ", but data.size() = " +
						std::to_string(data.size()));
			}

			ssize_t bytes_written = pwrite(fd_, data.data(), size, offset);
			//TODO(tony) update local cut
			// Make sure that 
			// 1. local cut ensures all prior messages are written (there can be holes b/c of multi-threaded request)
			// 2. Local cut maintains the number of messages, not number of batches
			if (bytes_written == -1) {
				throw std::system_error(errno, std::generic_category(), "pwrite failed");
			}
			if (bytes_written != size) {
				throw std::runtime_error("Incomplete pwrite: expected " + std::to_string(size) +
						", wrote " + std::to_string(bytes_written));
			}

			static std::chrono::steady_clock::time_point last_sync =
				std::chrono::steady_clock::now();

			constexpr int flush_interval_sec = 5;

			const auto now = std::chrono::steady_clock::now();
			if (now - last_sync >= std::chrono::seconds(flush_interval_sec)) {
				if (fsync(fd_) == -1) {
					throw std::system_error(errno, std::generic_category(), "fsync failed");
				}
				last_sync = now;
			}
		}

		Status CreateErrorResponse(ScalogReplicationResponse* response,
				const std::string& message,
				const Status& status) {
			response->set_success(false);
			LOG(ERROR) << "Replication error: " << message;
			return status;
		}

		const std::string base_filename_;
		int fd_ = -1;
		std::atomic<bool> running_;
		std::mutex file_mutex_;

		// Testing
		std::atomic<int64_t> replicate_count_{0}; // Track number of replication requests
};

ScalogReplicationManager::ScalogReplicationManager(const std::string& address,
		const std::string& port,
		const std::string& log_file) {
	try {
		std::string base_filename = log_file.empty() ? "scalogreplication_log.dat" : log_file;
		service_ = std::make_unique<ScalogReplicationServiceImpl>(base_filename);

		std::string server_address = address + ":" + (port.empty() ? std::to_string(SCALOG_REP_PORT) : port);

		LOG(INFO) << "Starting scalog replication manager at " << server_address;

		ServerBuilder builder;

		// Set server options
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(service_.get());

		// Performance tuning options
		builder.SetMaxReceiveMessageSize(16 * 1024 * 1024); // 16MB
		builder.SetMaxSendMessageSize(16 * 1024 * 1024);    // 16MB
		builder.SetSyncServerOption(ServerBuilder::SyncServerOption::NUM_CQS, 4);

		auto server = builder.BuildAndStart();
		if (!server) {
			throw std::runtime_error("Failed to start gRPC server");
		}
		server_ = std::move(server);

		VLOG(5) << "Scalog replication server listening on " << server_address;
	} catch (const std::exception& e) {
		LOG(ERROR) << "Failed to initialize replication manager: " << e.what();
		Shutdown();
		throw;
	}

	server_thread_ = std::thread([this]() {
			if (server_) {
			server_->Wait();
			}
			});
}

ScalogReplicationManager::~ScalogReplicationManager() {
	Shutdown();
}

void ScalogReplicationManager::Wait() {
	LOG(WARNING) << "Wait() called explicitly - this is not recommended as it may cause deadlocks";
	if (server_ && server_thread_.joinable()) {
		server_thread_.join();
	}
}

void ScalogReplicationManager::Shutdown() {
	static std::atomic<bool> shutdown_in_progress(false);

	// Ensure shutdown is only done once
	bool expected = false;
	if (!shutdown_in_progress.compare_exchange_strong(expected, true)) {
		return;
	}

	VLOG(5) << "Shutting down Scalog replication manager...";

	// 1. Shutdown service first to reject new requests
	if (service_) {
		service_->Shutdown();
	}

	// 2. Then shutdown server - this will unblock the Wait() call in server_thread_
	if (server_) {
		server_->Shutdown();
	}

	// 3. Join the server thread to avoid any race conditions
	if (server_thread_.joinable()) {
		server_thread_.join();
	}

	service_.reset();
	server_.reset();

	VLOG(5) << "Scalog replication manager shutdown completed";
}

} // namespace Scalog
