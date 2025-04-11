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
#include <thread>

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
	struct WriteTask {
		ScalogReplicationRequest request;
		WriteTask(const ScalogReplicationRequest& req) : request(req) {}
	};

	class LocalCutTracker {
		public:
			LocalCutTracker() : local_cut_(0), sequentially_written_(0) {}

			// Record a write and update local_cut
			void recordWrite(int64_t offset, int64_t size, int64_t number_of_messages) {
				if (size == 0) return;

				// Lock mutex
				absl::MutexLock lock(&mutex_);

				int64_t end = offset + size;

				// Find the first range that starts after our offset
				auto next_it = ranges.upper_bound(offset);

				// Keep track of the number of messages in the new range
				int64_t combined_num_messages = number_of_messages;
				// Check if we can merge with the previous range
				if (next_it != ranges.begin()) {
					auto prev_it = std::prev(next_it);
					if (prev_it->second.first >= offset) {
						// Our range overlaps with the previous one
						offset = prev_it->first;
						end = std::max(end, prev_it->second.first);
						combined_num_messages += prev_it->second.second;
						ranges.erase(prev_it);
					}
				}

				// Merge with any subsequent overlapping ranges
				while (next_it != ranges.end() && next_it->first <= end) {
					end = std::max(end, next_it->second.first);
					combined_num_messages += next_it->second.second;
					auto to_erase = next_it++;
					ranges.erase(to_erase);
				}

				// Insert the merged range
				ranges[offset] = std::make_pair(end, combined_num_messages);

				updateSequentiallyWritten();
			}

			int64_t getLocalCut() {
				absl::MutexLock lock(&mutex_);
				return local_cut_ - 1;
			}

		private:
		    // The pair contains the size as the first element and the number of messages as the second element.
			std::map<int64_t, std::pair<int64_t, int64_t>> ranges; // start -> end (exclusive)
			int64_t local_cut_;
			int64_t sequentially_written_;
			absl::Mutex mutex_;

			// Update the local cut value
			void updateSequentiallyWritten() {
				if (ranges.empty() || ranges.begin()->first > 0) {
					local_cut_ = 0;
					sequentially_written_ = 0;
					return;
				}

				// Start with the range that begins at offset 0
				auto zero_range = ranges.begin();
				int64_t current_end = zero_range->second.first;
				int64_t current_num_messages = zero_range->second.second;

				// Look for adjacent or overlapping ranges
				auto it = std::next(zero_range);
				while(it != ranges.end() && it->first <= current_end) {
					current_end = std::max(current_end, it->second.first);
					current_num_messages += it->second.second;
					++it;
				}

				sequentially_written_ = current_end;
				local_cut_ = current_num_messages;
			}
	};

	public:
		explicit ScalogReplicationServiceImpl(std::string base_filename, int broker_id)
			: base_filename_(std::move(base_filename)), running_(true), write_queue_(1024 * 8) {
				if (!OpenOutputFile()) {
					throw std::runtime_error("Failed to open replication file: " + base_filename_);
				}

				broker_id_ = broker_id;
				local_cut_tracker_ = std::make_unique<LocalCutTracker>();

				std::string scalog_seq_address = scalog_global_sequencer_ip_ + ":" + std::to_string(SCALOG_SEQ_PORT);
				std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(scalog_seq_address, grpc::InsecureChannelCredentials());
				stub_ = ScalogSequencer::NewStub(channel);

				writer_thread_ = std::thread(&ScalogReplicationServiceImpl::WriterLoop, this);
			}

		~ScalogReplicationServiceImpl() override {
			Shutdown();
		}

		void StartSendLocalCutThread() {
			send_local_cut_thread_ = std::thread(&ScalogReplicationServiceImpl::SendLocalCut, this);
		}

		void Shutdown() {
			bool expected = true;
			if (running_.compare_exchange_strong(expected, false)) {
				write_queue_.write(WriteTask(ScalogReplicationRequest{})); // unblock queue
				if (writer_thread_.joinable()) {
					writer_thread_.join();
				}

				std::lock_guard<std::mutex> lock(file_mutex_);
				CloseOutputFile();
			}

			if (send_local_cut_thread_.joinable()) {
				send_local_cut_thread_.join();
			}
		}

		Status Replicate(ServerContext* context, const ScalogReplicationRequest* request,
				ScalogReplicationResponse* response) override {
			if (!running_) {
				return CreateErrorResponse(response, "Service is shutting down", Status::CANCELLED);
			}
			if (!write_queue_.write(WriteTask(*request))) {
				return CreateErrorResponse(response, "Write queue is full", Status::CANCELLED);
			}
			response->set_success(true);
			return Status::OK;
		}

	private:
		bool OpenOutputFile() {
			fd_ = open(base_filename_.c_str(), O_RDWR | O_CREAT, 0644);
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

		void WriterLoop() {
			while (running_) {
				WriteTask task(ScalogReplicationRequest{});
				if (!write_queue_.read(task)) continue;

				std::lock_guard<std::mutex> file_lock(file_mutex_);
				try {
					if (fd_ == -1 && !ReopenOutputFile()) {
						LOG(ERROR) << "Failed to reopen file";
						continue;
					}
					WriteRequest(task.request);
				} catch (const std::exception& e) {
					LOG(ERROR) << "Write error: " << e.what();
				}
			}
		}

		void SendLocalCut(){
			grpc::ClientContext context;
			std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>> stream(
				stub_->HandleSendLocalCut(&context));

			// Spawn a thread to receive global cuts, passing the stream by reference
			std::thread receive_global_cut(&ScalogReplicationServiceImpl::ReceiveGlobalCut, this, std::ref(stream));

			while (running_) {
				LocalCut request;
				request.set_local_cut(local_cut_tracker_->getLocalCut());
				request.set_topic("");
				request.set_broker_id(broker_id_);
				request.set_epoch(local_epoch_);
				request.set_replica_id(replica_id_);

				// Send the LocalCut message to the server
				if (!stream->Write(request)) {
					std::cerr << "Stream to write local cut is closed, cleaning up..." << std::endl;
					break;
				}

				// Increment the epoch
				local_epoch_++;

				// Sleep until interval passes to send next local cut
				std::this_thread::sleep_for(local_cut_interval_);
			}

			stream->WritesDone();
			stop_reading_from_stream_ = true;
			receive_global_cut.join();
		}

		void ReceiveGlobalCut(std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>>& stream) {
			int num_global_cuts = 0;
			while (!stop_reading_from_stream_) {
				GlobalCut global_cut;
				if (stream->Read(&global_cut)) {
					// Convert google::protobuf::Map<int64_t, int64_t> to absl::flat_hash_map<int, int>
					for (const auto& entry : global_cut.global_cut()) {
						global_cut_[static_cast<int>(entry.first)] = static_cast<int>(entry.second);
					}

					ScalogSequencer(global_cut_);

					num_global_cuts++;
				}
			}

			// grpc::Status status = stream->Finish();
		}

		void ScalogSequencer(absl::btree_map<int, int> &global_cut) {
			static size_t seq = 0;
			static off_t disk_offset = 0;
			ScalogMessageHeader header_buffer;

			for (auto &cut : global_cut) {
				if (cut.first == broker_id_) {
					for (int i = 0; i < cut.second; i++) {
						ssize_t read_bytes = pread(fd_, &header_buffer, sizeof(header_buffer), disk_offset);
						if (read_bytes != sizeof(header_buffer)) {
							perror("Failed to read message header from file");
							return;
						}
						header_buffer.total_order = seq;
						std::atomic_thread_fence(std::memory_order_release);

						// Write the header back to the same file
						ssize_t written = pwrite(fd_, &header_buffer, sizeof(header_buffer), disk_offset);
						if (written == -1) {
							throw std::system_error(errno, std::generic_category(), "Failed to write updated header file with order to file");
						}
						if (written != sizeof(header_buffer)) {
							perror("Failed to write message header to file");
							return;
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

						disk_offset += header_buffer.paddedSize;
						seq++;
					}
				} else {
					// For messages not belonging to our broker, just update the sequence counter.
					seq += cut.second;
				}
			}
		}

		void WriteRequest(const ScalogReplicationRequest& request) {
			const auto& data = request.data();
			int64_t offset = request.offset();
			int64_t size = request.size();
			int64_t num_msg = request.num_msg();

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

			local_cut_tracker_->recordWrite(offset, bytes_written, num_msg);
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
		folly::MPMCQueue<WriteTask> write_queue_;
		std::thread writer_thread_;

		// Global seq ip
		std::string scalog_global_sequencer_ip_ = SCLAOG_SEQUENCER_IP;

		std::thread send_local_cut_thread_;

		/// Time between each local cut
		std::chrono::microseconds local_cut_interval_ = std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL);

		/// Map of broker_id to local cut
		absl::btree_map<int, int> global_cut_;

		std::unique_ptr<ScalogSequencer::Stub> stub_;

		/// Flag to indicate if we should stop reading from the stream
		bool stop_reading_from_stream_ = false;

		int broker_id_;
		int replica_id_ = 1;
		int local_epoch_ = 0;
		std::unique_ptr<LocalCutTracker> local_cut_tracker_;
};

ScalogReplicationManager::ScalogReplicationManager(int broker_id,
		const std::string& address,
		const std::string& port,
		const std::string& log_file) {
	try {
		std::string base_filename = log_file.empty() ? "scalogreplication_log.dat" : log_file;
		service_ = std::make_unique<ScalogReplicationServiceImpl>(base_filename, broker_id);

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

void ScalogReplicationManager::StartSendLocalCut() {
	service_->StartSendLocalCutThread();
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
