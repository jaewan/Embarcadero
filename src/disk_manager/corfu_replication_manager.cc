#include "corfu_replication_manager.h"

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

#include "corfu_replication.grpc.pb.h"

namespace Corfu {

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using corfureplication::CorfuReplicationService;
using corfureplication::CorfuReplicationRequest;
using corfureplication::CorfuReplicationResponse;

class CorfuReplicationServiceImpl final : public CorfuReplicationService::Service {
	public:
		explicit CorfuReplicationServiceImpl(std::string base_filename)
			: base_filename_(std::move(base_filename)), running_(true) {
				if (!OpenOutputFile()) {
					throw std::runtime_error("Failed to open replication file: " + base_filename_);
				}
			}

		~CorfuReplicationServiceImpl() override {
			Shutdown();
		}

		void Shutdown() {
			bool expected = true;
			if (running_.compare_exchange_strong(expected, false)) {
				std::lock_guard<std::mutex> lock(file_mutex_);
				CloseOutputFile();
			}
		}

		Status Replicate(ServerContext* context, const CorfuReplicationRequest* request,
				CorfuReplicationResponse* response) override {
			if (!running_) {
				return CreateErrorResponse(response, "Service is shutting down", Status::CANCELLED);
			}

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

		void WriteRequest(const CorfuReplicationRequest& request) {
			const auto& data = request.data();
			int64_t offset = request.offset();
			int64_t size = request.size();

			if (data.size() != static_cast<size_t>(size)) {
				throw std::runtime_error("Size mismatch: request.size() = " +
						std::to_string(size) + ", but data.size() = " +
						std::to_string(data.size()));
			}

			ssize_t bytes_written = pwrite(fd_, data.data(), size, offset);
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

		Status CreateErrorResponse(CorfuReplicationResponse* response,
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
};

CorfuReplicationManager::CorfuReplicationManager(const std::string& address,
		const std::string& port,
		const std::string& log_file) {
	try {
		std::string base_filename = log_file.empty() ? "corfureplication_log.dat" : log_file;
		service_ = std::make_unique<CorfuReplicationServiceImpl>(base_filename);

		std::string server_address = address + ":" + (port.empty() ? std::to_string(CORFU_REP_PORT) : port);
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

		VLOG(5) << "Corfu replication server listening on " << server_address;
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

CorfuReplicationManager::~CorfuReplicationManager() {
	Shutdown();
}

void CorfuReplicationManager::Wait() {
	LOG(WARNING) << "Wait() called explicitly - this is not recommended as it may cause deadlocks";
	if (server_ && server_thread_.joinable()) {
		server_thread_.join();
	}
}

void CorfuReplicationManager::Shutdown() {
	static std::atomic<bool> shutdown_in_progress(false);

	// Ensure shutdown is only done once
	bool expected = false;
	if (!shutdown_in_progress.compare_exchange_strong(expected, true)) {
		return;
	}

	VLOG(5) << "Shutting down Corfu replication manager...";

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

	VLOG(5) << "Corfu replication manager shutdown completed";
}

} // namespace Corfu
