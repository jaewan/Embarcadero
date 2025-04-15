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
#include <shared_mutex>
#include <condition_variable>

#include "corfu_replication.grpc.pb.h"

namespace Corfu {

	using grpc::Server;
	using grpc::ServerBuilder;
	using grpc::ServerContext;
	using grpc::Status;
	using grpc::StatusCode;
	using corfureplication::CorfuReplicationService;
	using corfureplication::CorfuReplicationRequest;
	using corfureplication::CorfuReplicationResponse;

	class CorfuReplicationServiceImpl final : public CorfuReplicationService::Service {
		public:
			explicit CorfuReplicationServiceImpl(std::string base_filename)
				: base_filename_(std::move(base_filename)), running_(true), fd_(-1) {
					if (!OpenOutputFile()) {
						throw std::runtime_error("Failed to open replication file: " + base_filename_);
					}
					fsync_thread_ = std::thread(&CorfuReplicationServiceImpl::FsyncLoop, this);
				}

			~CorfuReplicationServiceImpl() override {
				Shutdown();
				if (fsync_thread_.joinable()) {
					fsync_thread_.join();
				}
			}

			void Shutdown() {
				bool expected = true;
				if (running_.compare_exchange_strong(expected, false)) {
					cv_fsync_.notify_one(); // Wake up fsync thread if sleeping
					std::unique_lock<std::shared_mutex> lock(file_state_mutex_);
					CloseOutputFile();
				}
			}

			Status Replicate(ServerContext* context, const CorfuReplicationRequest* request,
					CorfuReplicationResponse* response) override {
				if (!running_) {
					return CreateErrorResponse(response, "Service is shutting down", StatusCode::CANCELLED);
				}
				int current_fd = -1;

				{
					std::shared_lock<std::shared_mutex> lock(file_state_mutex_);
					if(!running_){
						return CreateErrorResponse(response, "Service is shutting down", StatusCode::CANCELLED);
					}
					if(fd_ == -1){
						LOG(ERROR) << "Replication failed: File descriptor is invalid.";
						return CreateErrorResponse(response, "File descriptor invalid", StatusCode::UNAVAILABLE);
					}
					current_fd = fd_;
					// --- Perform the write under shared lock ---
					try {
						WriteRequestInternal(*request, current_fd); // Pass fd explicitly
						response->set_success(true);
						return Status::OK;
					} catch (const std::system_error& e) {
						LOG(ERROR) << "System error during pwrite: " << e.what() << " (code: " << e.code() << ")";
						// Check for EBADF specifically, might indicate fd became invalid
						if (e.code().value() == EBADF) {
							// Potentially trigger a reopen sequence or mark service unhealthy
							LOG(ERROR) << "Bad file descriptor encountered during write!";
							// Consider attempting a controlled reopen here or in fsync thread
							// AttemptReopen(); // Needs careful implementation with unique_lock
						}
						return CreateErrorResponse(response, std::string("Write Error: ") + e.what(), StatusCode::INTERNAL);
					} catch (const std::exception& e) {
						LOG(ERROR) << "Exception during replication write: " << e.what();
						return CreateErrorResponse(response, std::string("Error: ") + e.what(), StatusCode::INTERNAL);
					}
				}
			}

		private:
			bool OpenOutputFile() {
				std::unique_lock<std::shared_mutex> lock(file_state_mutex_);
				if (fd_ != -1) { // Already open
					return true;
				}
				fd_ = open(base_filename_.c_str(), O_WRONLY | O_CREAT, 0644);
				if (fd_ == -1) {
					LOG(ERROR) << "Failed to open file: " << strerror(errno);
					return false;
				}
				return true;
			}

			bool ReopenOutputFile() {
				std::unique_lock<std::shared_mutex> lock(file_state_mutex_);
				CloseOutputFile();
				return OpenOutputFile();
			}

			void CloseOutputFile() {
				if (fd_ != -1) {
					close(fd_);
					fd_ = -1;
				}
			}

			void WriteRequestInternal(const CorfuReplicationRequest& request, int current_fd) const {
				const auto& data = request.data();
				int64_t offset = request.offset();
				int64_t size = request.size();

				if (data.size() != static_cast<size_t>(size)) {
					throw std::runtime_error("Size mismatch: request.size() = " +
							std::to_string(size) + ", but data.size() = " +
							std::to_string(data.size()));
				}

				// Use the passed file descriptor
				ssize_t bytes_written = pwrite(current_fd, data.data(), size, offset);

				if (bytes_written == -1) {
					// Throw system_error to include errno
					throw std::system_error(errno, std::generic_category(), "pwrite failed for fd " + std::to_string(current_fd));
				}
				if (bytes_written != size) {
					// This usually indicates a problem (e.g., disk full), treat as error
					throw std::runtime_error("Incomplete pwrite: expected " + std::to_string(size) +
							", wrote " + std::to_string(bytes_written) + " for fd " + std::to_string(current_fd));
				}
				// VLOG(5) << "Successfully wrote " << bytes_written << " bytes at offset " << offset; // Optional verbose logging
			}


			// Dedicated thread loop for periodic fsync
			void FsyncLoop() {
				const std::chrono::seconds flush_interval(5);
				VLOG(1) << "Fsync thread started.";

				while (running_.load()) {
					// Wait for the interval or shutdown signal
					std::unique_lock<std::mutex> lock(fsync_cv_mutex_); // Mutex for CV wait
					if (cv_fsync_.wait_for(lock, flush_interval, [this]{ return !running_.load(); })) {
						// Returns true if predicate is true (i.e., running_ is false)
						break; // Exit loop if shutting down
					}
					// Timed out, proceed with fsync attempt

					VLOG(5) << "Fsync thread waking up to sync.";

					// Acquire exclusive lock to ensure file state doesn't change during fsync
					std::unique_lock<std::shared_mutex> file_lock(file_state_mutex_);

					if (!running_.load()) { // Double check after acquiring lock
						break;
					}

					if (fd_ != -1) {
						VLOG(5) << "Attempting fsync on fd " << fd_;
						if (fsync(fd_) == -1) {
							LOG(ERROR) << "fsync failed for fd " << fd_ << ": " << strerror(errno);
							// Consider attempting ReopenOutputFile here if fsync fails due to EBADF or EIO?
							if (errno == EBADF || errno == EIO) {
								LOG(ERROR) << "Attempting to reopen file due to fsync error.";
								// Note: ReopenOutputFile acquires its own unique lock, which is fine
								// since we already hold it. Re-entrancy isn't an issue here.
								// However, directly calling it might be cleaner to release/reacquire
								// or have a helper that assumes lock is held.
								// For simplicity, let's assume Close+Open handles it.
								CloseOutputFile();
								OpenOutputFile(); // This will log errors if it fails
							}
						} else {
							VLOG(5) << "fsync completed successfully for fd " << fd_;
						}
					} else {
						VLOG(1) << "Skipping fsync, file descriptor is invalid.";
						// Maybe try to reopen here as well?
						// OpenOutputFile(); // If fd is -1, try reopening it
					}
					// file_lock (unique_lock) is released here
				}
				VLOG(1) << "Fsync thread stopping.";
			}


			Status CreateErrorResponse(CorfuReplicationResponse* response,
            const std::string& message,
            grpc::StatusCode code) {
        response->set_success(false);

        // Construct the status object first
        grpc::Status status_to_return(code, message);

        // Log based on the created status
        if (status_to_return.error_code() != grpc::StatusCode::CANCELLED || message.find("shutting down") == std::string::npos) {
             // Log the integer code value and the message
             LOG(ERROR) << "Replication error (code: " << status_to_return.error_code() << "): " << status_to_return.error_message();
        } else {
             VLOG(1) << "Replication cancelled: " << status_to_return.error_message();
        }

        return status_to_return;
    }

			const std::string base_filename_;
			int fd_ = -1;
			std::atomic<bool> running_;
			std::shared_mutex file_state_mutex_; // Use shared mutex

			// For fsync thread
			std::thread fsync_thread_;
			std::condition_variable cv_fsync_;
			std::mutex fsync_cv_mutex_; // Mutex needed for condition variable wait
	};

	CorfuReplicationManager::CorfuReplicationManager(
			int broker_id,
			bool log_to_memory,
			const std::string& address,
			const std::string& port,
			const std::string& log_file) {
		try {
			int disk_to_write = broker_id % NUM_DISKS ;
			std::string base_dir = "../../.Replication/disk" + std::to_string(disk_to_write) + "/";
			if(log_to_memory){
				base_dir = "/tmp/";
			}
			std::string base_filename = log_file.empty() ? base_dir+"corfu_replication_log"+std::to_string(broker_id) +".dat" : log_file;
			service_ = std::make_unique<CorfuReplicationServiceImpl>(base_filename);

			std::string server_address = address + ":" + (port.empty() ? std::to_string(CORFU_REP_PORT) : port);
			ServerBuilder builder;

			// Set server options
			builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
			builder.RegisterService(service_.get());

			// Performance tuning options
			//builder.SetMaxReceiveMessageSize(16 * 1024 * 1024); // 16MB
			//builder.SetMaxSendMessageSize(16 * 1024 * 1024);    // 16MB

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
