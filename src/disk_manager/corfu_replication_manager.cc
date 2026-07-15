#include "corfu_replication_manager.h"
#include "corfu_replication.grpc.pb.h"
#include "common/replica_disk_dirs.h"
#include "disk_manager/corfu_replica_store.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/alarm.h>
#include <glog/logging.h>

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <system_error>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <shared_mutex>
#include <condition_variable>

namespace Corfu {

	using grpc::Server;
	using grpc::ServerBuilder;
	using grpc::ServerContext;
	using grpc::Status;
	using grpc::StatusCode;
	using corfureplication::CorfuReplicationService;
	using corfureplication::CorfuReplicationRequest;
	using corfureplication::CorfuReplicationResponse;
	using corfureplication::CorfuProbeRequest;
	using corfureplication::CorfuProbeResponse;
	using corfureplication::CorfuWriteOnceRequest;
	using corfureplication::CorfuJunkRequest;
	using corfureplication::CorfuWriteResponse;

	static CorfuSlotKey ToSlot(const corfureplication::CorfuSlotKey& s) {
		return CorfuSlotKey{s.topic(), s.broker_id(), s.broker_batch_seq()};
	}
	static CorfuValueId ToValue(const corfureplication::CorfuValueId& v) {
		return CorfuValueId{v.client_id(), v.original_client_batch_seq(), v.total_order(), v.num_msg(), v.total_size()};
	}
	static void SetValue(const CorfuValueId& v, corfureplication::CorfuValueId* out) {
		out->set_client_id(v.client_id); out->set_original_client_batch_seq(v.original_client_batch_seq);
		out->set_total_order(v.total_order); out->set_num_msg(v.num_msg); out->set_total_size(v.total_size);
	}
	static corfureplication::CorfuWriteStatus ToProto(CorfuWriteStatus s) {
		using P=corfureplication::CorfuWriteStatus; switch(s) { case CorfuWriteStatus::kWritten:return P::CORFU_WRITTEN; case CorfuWriteStatus::kAlreadySame:return P::CORFU_ALREADY_SAME; case CorfuWriteStatus::kAlreadyJunk:return P::CORFU_ALREADY_JUNK; case CorfuWriteStatus::kConflict:return P::CORFU_CONFLICT; default:return P::CORFU_IO_ERROR; }
	}
	static std::string SlotDebugString(const CorfuSlotKey& slot) {
		return "(topic=" + slot.topic + ",broker=" + std::to_string(slot.broker_id) +
			",batch=" + std::to_string(slot.broker_batch_seq) + ")";
	}
	static std::string ValueDebugString(const CorfuValueId& value) {
		return "(client=" + std::to_string(value.client_id) +
			",seq=" + std::to_string(value.original_client_batch_seq) +
			",order=" + std::to_string(value.total_order) +
			",messages=" + std::to_string(value.num_msg) +
			",bytes=" + std::to_string(value.total_size) + ")";
	}
	static uint32_t Crc32c(const uint8_t* p, size_t n) { uint32_t c=~0u; while(n--){c^=*p++;for(int i=0;i<8;++i)c=(c>>1)^(0x82f63b78u&-(c&1));}return ~c; }

	class CorfuReplicationServiceImpl final : public CorfuReplicationService::Service {
		public:
			explicit CorfuReplicationServiceImpl(std::string base_filename, void* cxl_addr, bool media_durable)
				: base_filename_(std::move(base_filename)), cxl_addr_(cxl_addr), media_durable_(media_durable), running_(true), fd_(-1) {
					if (media_durable_ && !OpenOutputFile()) {
						throw std::runtime_error("Failed to open replication file: " + base_filename_);
					}
					store_ = media_durable_
						? std::make_unique<CorfuReplicaStore>(base_filename_, base_filename_ + ".corfu_sidecar", true)
						: std::make_unique<CorfuReplicaStore>("", "", false);
					if (!media_durable_) {
						LOG(INFO) << "CORFU WriteOnce replica sink=memory-copy; "
						          << "payloads are retained only in process memory";
					}
					// WriteOnce owns the durability boundary (data fdatasync followed
					// by sidecar fdatasync).  A periodic fsync worker is both redundant
					// and unsafe here; it previously re-entered the file lock on error.
				}

			~CorfuReplicationServiceImpl() override {
				Shutdown();
			}

			void Shutdown() {
				bool expected = true;
				if (running_.compare_exchange_strong(expected, false)) {
					std::unique_lock<std::shared_mutex> lock(file_state_mutex_);
					CloseOutputFile();
				}
			}

			Status Replicate(ServerContext* context, const CorfuReplicationRequest* request,
					CorfuReplicationResponse* response) override {
				// An anonymous byte-range write has no slot/value identity, cannot
				// reject conflicts, and cannot participate in RF3 hole recovery.
				// Keep the RPC name for wire compatibility but fail closed so no
				// caller silently bypasses the ordered-chain protocol.
				response->set_success(false);
				return Status(StatusCode::FAILED_PRECONDITION,
					"legacy Replicate is disabled; use WriteOnce with slot/value identity");
				/*
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
					// ACK2 callers use this RPC reply as the replica's durable
					// completion. Keep the descriptor and payload write plus media
					// synchronization in the same request boundary; the periodic
					// fsync loop remains only a background hygiene mechanism.
					try {
						WriteRequestInternal(*request, current_fd); // Pass fd explicitly
						if (fdatasync(current_fd) != 0) {
							throw std::system_error(errno, std::generic_category(),
								"fdatasync failed for fd " + std::to_string(current_fd));
						}
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
				*/
			}

			Status ProbeSlot(ServerContext*, const CorfuProbeRequest* request, CorfuProbeResponse* response) override {
				if (!running_.load(std::memory_order_acquire)) return Status(StatusCode::CANCELLED, "service is shutting down");
				const auto r = store_->Probe(ToSlot(request->slot()));
				response->set_state(r.state == CorfuSlotState::kValue ? corfureplication::CORFU_VALUE : r.state == CorfuSlotState::kJunk ? corfureplication::CORFU_JUNK : corfureplication::CORFU_UNWRITTEN);
				if (r.state == CorfuSlotState::kValue) SetValue(r.value, response->mutable_value());
				return Status::OK;
			}
			Status WriteOnce(ServerContext*, const CorfuWriteOnceRequest* request, CorfuWriteResponse* response) override {
				constexpr uint64_t kMaxWriteBytes = 64ULL * 1024ULL * 1024ULL;
				if (!running_.load(std::memory_order_acquire) || request->size() != request->value().total_size() ||
					request->size() != static_cast<uint64_t>(request->payload().size()) || request->size() > kMaxWriteBytes ||
					Crc32c(reinterpret_cast<const uint8_t*>(request->payload().data()), request->payload().size()) != request->payload_crc32c()) {
					response->set_status(corfureplication::CORFU_IO_ERROR); return Status::OK;
				}
				const auto slot = ToSlot(request->slot());
				const auto value = ToValue(request->value());
				const auto write_status = store_->WriteOnce(
					slot, value, request->source_offset(), request->payload().data(), request->size());
				if (write_status == CorfuWriteStatus::kConflict) {
					const auto existing = store_->Probe(slot);
					LOG(ERROR) << "CORFU WriteOnce conflict slot=" << SlotDebugString(slot)
						<< " existing_state=" << static_cast<unsigned>(existing.state)
						<< " existing_value=" << ValueDebugString(existing.value)
						<< " incoming_value=" << ValueDebugString(value)
						<< "; refusing to overwrite a durable slot";
				}
				response->set_status(ToProto(write_status));
				return Status::OK;
			}
			Status WriteJunkOnce(ServerContext*, const CorfuJunkRequest* request, CorfuWriteResponse* response) override {
				if (!running_.load(std::memory_order_acquire)) return Status(StatusCode::CANCELLED, "service is shutting down");
				response->set_status(ToProto(store_->WriteJunkOnce(ToSlot(request->slot())))); return Status::OK;
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
				uint64_t log_idx = request.log_idx();
				uint64_t size = request.size();

				if (!cxl_addr_) {
					throw std::runtime_error("CXL address not initialized in ReplicationService");
				}

				// Phase 4 fix: Read directly from CXL memory instead of protobuf bytes
				void* source_addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(cxl_addr_) + log_idx);

				// Use the passed file descriptor
				ssize_t bytes_written = pwrite(current_fd, source_addr, size, log_idx);

				if (bytes_written == -1) {
					// Throw system_error to include errno
					throw std::system_error(errno, std::generic_category(), "pwrite failed for fd " + std::to_string(current_fd));
				}
				if (static_cast<size_t>(bytes_written) != size) {
					// This usually indicates a problem (e.g., disk full), treat as error
					throw std::runtime_error("Incomplete pwrite: expected " + std::to_string(size) +
							", wrote " + std::to_string(bytes_written) + " for fd " + std::to_string(current_fd));
				}
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
			void* const cxl_addr_;
			const bool media_durable_;
			std::atomic<bool> running_;
			int fd_ = -1;
			std::unique_ptr<CorfuReplicaStore> store_;
			std::shared_mutex file_state_mutex_; // Use shared mutex
			// Retained only for the unused legacy helper below; no thread is
			// started, because WriteOnce owns the synchronous durability boundary.
			std::condition_variable cv_fsync_;
			std::mutex fsync_cv_mutex_;

	};

	CorfuReplicationManager::CorfuReplicationManager(
			int broker_id,
			bool log_to_memory,
			void* cxl_addr,
			const std::string& address,
			const std::string& port,
			const std::string& log_file) {
		LOG(INFO) << "[CORFU_DEBUG] CorfuReplicationManager ctor start (broker_id=" << broker_id << ")";
		try {
			std::string base_dir;
			if (log_to_memory) {
				// RF1/ACK1 does not use a remote chain. Keep the listener alive for
				// normal broker startup, but WriteOnce rejects any ACK2 attempt.
				base_dir = "/tmp/";
		} else {
			// ACK2 may not silently select a convenient working-directory or /tmp
			// path.  The operator must name the replica medium explicitly so the
			// manifest and failure model have a concrete sink to validate.
			const char* configured_dirs = std::getenv("EMBARCADERO_REPLICA_DISK_DIRS");
			const char* configured_root = std::getenv("EMBARCADERO_REPLICA_DISK_ROOT");
			if ((configured_dirs == nullptr || *configured_dirs == '\0') &&
				(configured_root == nullptr || *configured_root == '\0')) {
				throw std::invalid_argument("Corfu RF>1 requires explicit EMBARCADERO_REPLICA_DISK_DIRS or EMBARCADERO_REPLICA_DISK_ROOT");
			}
			const auto dirs = Embarcadero::ResolveWritableReplicationDirs();
				base_dir = Embarcadero::SelectReplicationDirForBroker(broker_id, dirs);
				if (base_dir.empty()) throw std::invalid_argument("Corfu remote replica needs an explicit writable durable replication directory");
				std::error_code ec;
				std::filesystem::create_directories(base_dir, ec);
				if (ec) throw std::runtime_error("cannot create Corfu replica directory: " + ec.message());
				if (!base_dir.empty() && base_dir.back() != '/') base_dir.push_back('/');
			}
			std::string base_filename = log_file.empty()
				? base_dir + "corfu_replication_log" + std::to_string(broker_id) + ".dat"
				: log_file;
			LOG(INFO) << "[CORFU_DEBUG] CorfuReplicationManager: creating CorfuReplicationServiceImpl (base_filename=" << base_filename << ")";
			service_ = std::make_unique<CorfuReplicationServiceImpl>(base_filename, cxl_addr, !log_to_memory);
			LOG(INFO) << "[CORFU_DEBUG] CorfuReplicationManager: service created";

			// Every broker owns a distinct replica listener.  A configured base
			// port maps deterministically to membership/broker index.
			std::string server_address = address + ":" + (port.empty() ? std::to_string(CORFU_REP_PORT + broker_id) : port);
			ServerBuilder builder;

			// Set server options
			builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
			builder.RegisterService(service_.get());

			// Performance tuning options
			//builder.SetMaxReceiveMessageSize(16 * 1024 * 1024); // 16MB
			//builder.SetMaxSendMessageSize(16 * 1024 * 1024);    // 16MB

			LOG(INFO) << "[CORFU_DEBUG] CorfuReplicationManager: binding gRPC server to " << server_address;
			auto server = builder.BuildAndStart();
			if (!server) {
				throw std::runtime_error("Failed to start gRPC server");
			}
			server_ = std::move(server);

			LOG(INFO) << "[CORFU_DEBUG] CorfuReplicationManager: gRPC server listening on " << server_address;
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
		LOG(INFO) << "[CORFU_DEBUG] CorfuReplicationManager: constructor complete";
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
		// Ensure shutdown is only done once
		bool expected = false;
		if (!shutdown_.compare_exchange_strong(expected, true)) {
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
