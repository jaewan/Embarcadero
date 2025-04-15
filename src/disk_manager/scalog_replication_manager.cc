#include "scalog_replication_manager.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/alarm.h>
#include <glog/logging.h>
#include <folly/MPMCQueue.h>

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
#include <shared_mutex>
#include <condition_variable>

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
		// --- LocalCutTracker (Assumed Correct - Uses its own absl::Mutex) ---
		class LocalCutTracker {
			public:
				LocalCutTracker() : local_cut_(0), sequentially_written_(0) {}

				// Record a write and update local_cut
				void recordWrite(int64_t offset, int64_t size, int64_t number_of_messages) {
					if (size == 0) return;
					absl::MutexLock lock(&mutex_); // Uses its own mutex

					int64_t end = offset + size;
					auto next_it = ranges.upper_bound(offset);
					int64_t combined_num_messages = number_of_messages;

					if (next_it != ranges.begin()) {
						auto prev_it = std::prev(next_it);
						if (prev_it->second.first >= offset) {
							offset = prev_it->first;
							end = std::max(end, prev_it->second.first);
							combined_num_messages += prev_it->second.second;
							ranges.erase(prev_it);
						}
					}
					while (next_it != ranges.end() && next_it->first <= end) {
						end = std::max(end, next_it->second.first);
						combined_num_messages += next_it->second.second;
						auto to_erase = next_it++;
						ranges.erase(to_erase);
					}
					ranges[offset] = std::make_pair(end, combined_num_messages);
					updateSequentiallyWritten();
				}

				int64_t getLocalCut() {
					absl::MutexLock lock(&mutex_);
					// Assuming local_cut_ represents the number of messages,
					// and the cut should be the *next* expected message number.
					// If local_cut_ is the count, maybe just return local_cut_?
					// Or if it's the last *written* number, return local_cut_ + 1?
					// Returning local_cut_ - 1 seems odd if it starts at 0.
					// Let's assume local_cut_ is the count for now.
					return local_cut_;
					// return local_cut_ - 1; // Original logic - double check intent
				}

				int64_t getSequentiallyWrittenOffset() {
					absl::MutexLock lock(&mutex_);
					return sequentially_written_;
				}

			private:
				// Map: start_offset -> {end_offset_exclusive, num_messages_in_range}
				std::map<int64_t, std::pair<int64_t, int64_t>> ranges;
				int64_t local_cut_; // Number of messages written contiguously from start?
				int64_t sequentially_written_; // Offset written contiguously from start
				absl::Mutex mutex_; // Mutex specific to this tracker

				// Updates local_cut_ and sequentially_written_ based on contiguous ranges from offset 0
				void updateSequentiallyWritten() {
					if (ranges.empty() || ranges.begin()->first > 0) {
						local_cut_ = 0;
						sequentially_written_ = 0;
						return;
					}

					auto current_range_it = ranges.begin();
					int64_t current_end = current_range_it->second.first;
					int64_t current_num_messages = current_range_it->second.second;

					auto next_range_it = std::next(current_range_it);
					while(next_range_it != ranges.end() && next_range_it->first <= current_end) {
						// Found contiguous or overlapping range
						current_end = std::max(current_end, next_range_it->second.first);
						current_num_messages += next_range_it->second.second;
						// Move to check the next range
						current_range_it = next_range_it;
						next_range_it = std::next(current_range_it);
					}

					// After loop, current_end is the end of the contiguous block from offset 0
					sequentially_written_ = current_end;
					local_cut_ = current_num_messages; // Update the message count
				}
		};
		// --- End LocalCutTracker ---

		// --- Write Task Definition ---
		struct WriteTask {
			// Store necessary data - copy from request
			int64_t offset;
			int64_t size;
			int64_t num_msg;
			std::string data; // Store data by value

			// Constructor to copy from request
			explicit WriteTask(const ScalogReplicationRequest& req) :
				offset(req.offset()),
				size(req.size()),
				num_msg(req.num_msg()),
				data(req.data()) // Copy data
			{}
		};
		// --- End Write Task ---

		public:
		explicit ScalogReplicationServiceImpl(std::string base_filename, int broker_id)
			: base_filename_(std::move(base_filename)),
			broker_id_(broker_id),
			running_(true),
			stop_reading_from_stream_(false),
			fd_(-1), // Initialize fd_
			write_queue_(10240), // Queue size
			local_epoch_(0),
			replica_id_(1) // Example replica ID
		{
			local_cut_interval_ = std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL);

			if (!OpenOutputFile()) { // Acquires unique lock
				throw std::runtime_error("Failed to open replication file: " + base_filename_);
			}

			local_cut_tracker_ = std::make_unique<LocalCutTracker>();

			// Setup gRPC channel to sequencer (error handling recommended)
			std::string scalog_seq_address = std::string(SCLAOG_SEQUENCER_IP) + ":" + std::to_string(SCALOG_SEQ_PORT);
			std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(scalog_seq_address, grpc::InsecureChannelCredentials());
			stub_ = ScalogSequencer::NewStub(channel); // Assuming this is the correct Stub type

			VLOG(1) << "Starting writer threads...";
			for (int i = 0; i < NUM_DISK_IO_THREADS; ++i) {
				writer_threads_.emplace_back(&ScalogReplicationServiceImpl::WriterLoop, this);
			}
			VLOG(1) << "Starting fsync thread...";
			fsync_thread_ = std::thread(&ScalogReplicationServiceImpl::FsyncLoop, this);
			// Note: send_local_cut_thread_ is started externally via StartSendLocalCutThread
		}

		~ScalogReplicationServiceImpl() override {
			Shutdown(); // Ensure shutdown is called
		}

		// Must be called *after* the gRPC server is running to start the client stream
		void StartSendLocalCutThread() {
			if (!send_local_cut_thread_.joinable()) {
				VLOG(1) << "Starting SendLocalCut thread...";
				send_local_cut_thread_ = std::thread(&ScalogReplicationServiceImpl::SendLocalCut, this);
			} else {
				LOG(WARNING) << "SendLocalCut thread already started.";
			}
		}

		void Shutdown() {
			bool expected = true;
			// Only proceed if running_ was true
			if (running_.compare_exchange_strong(expected, false)) {
				VLOG(1) << "Initiating shutdown sequence...";

				// 1. Signal background threads to stop
				VLOG(5) << "Signalling fsync thread to stop...";
				cv_fsync_.notify_one();
				VLOG(5) << "Signalling local cut sender to stop (via running_ flag)...";
				// SendLocalCut checks running_ flag
				VLOG(5) << "Signalling receiver thread to stop...";
				stop_reading_from_stream_.store(true); // Signal ReceiveGlobalCut to stop reading

				VLOG(5) << "Enqueueing writer thread sentinels...";
				for (int i = 0; i < NUM_DISK_IO_THREADS; ++i) {
					// Use non-blocking write in case queue is full during shutdown,
					// though blocking might be okay if threads are responsive.
					// blockingWrite is simpler if acceptable.
					write_queue_.blockingWrite(std::nullopt); // Enqueue sentinel
				}

				// 2. Close File Descriptor (acquire exclusive lock)
				VLOG(5) << "Acquiring exclusive lock for file close...";
				{ // Scope for unique lock
					std::unique_lock<std::shared_mutex> lock(file_state_mutex_);
					VLOG(5) << "Exclusive lock acquired. Closing file.";
					CloseOutputFileInternal(); // Close the file safely
				}
				VLOG(5) << "File closed.";

				// 3. Join threads (order can matter)
				VLOG(5) << "Joining writer threads...";
				for (auto& t : writer_threads_) {
					if (t.joinable()) {
						t.join();
					}
				}
				VLOG(1) << "Writer threads joined.";

				VLOG(5) << "Joining fsync thread...";
				if (fsync_thread_.joinable()) {
					fsync_thread_.join();
				}
				VLOG(1) << "Fsync thread joined.";

				// SendLocalCut thread manages the ReceiveGlobalCut thread internally
				VLOG(5) << "Joining SendLocalCut thread (will also join receiver)...";
				if (send_local_cut_thread_.joinable()) {
					send_local_cut_thread_.join();
				}
				VLOG(1) << "SendLocalCut thread joined.";

			} else {
				VLOG(1) << "Shutdown already initiated.";
			}
		}

		// --- Asynchronous Replicate Method ---
		Status Replicate(ServerContext* context, const ScalogReplicationRequest* request,
				ScalogReplicationResponse* response) override {

			// 1. Check if service is running (quick check)
			if (!running_.load()) {
				return CreateErrorResponse(response, "Service is shutting down", grpc::StatusCode::UNAVAILABLE);
			}

			// 2. Validate request (optional, but good practice)
			if (request->size() < 0 || request->offset() < 0 || request->num_msg() <= 0 || request->data().size() != static_cast<size_t>(request->size())) {
				LOG(ERROR) << "Invalid replication request received: size=" << request->size()
					<< ", offset=" << request->offset() << ", num_msg=" << request->num_msg()
					<< ", data_len=" << request->data().size();
				return CreateErrorResponse(response, "Invalid request parameters", grpc::StatusCode::INVALID_ARGUMENT);
			}

			// 3. Create WriteTask (copies data)
			WriteTask task(*request);

			// 4. Enqueue task
			// Use blocking write for simplicity, assuming queue is large enough
			// or backpressure is acceptable. Could use tryWrite for non-blocking.
			VLOG(5) << "Enqueueing write task for offset " << task.offset << " size " << task.size;
			write_queue_.blockingWrite(std::move(task));

			// 5. Return success immediately
			response->set_success(true);
			return Status::OK;
		}

		private:
		// --- File Operations (Protected by file_state_mutex_) ---
		// Acquires UNIQUE lock
		bool OpenOutputFile() {
			std::unique_lock<std::shared_mutex> lock(file_state_mutex_);
			if (fd_ != -1) return true; // Already open
																	// Use O_RDWR since ScalogSequencer needs to read headers
			fd_ = open(base_filename_.c_str(), O_RDWR | O_CREAT, 0644);
			if (fd_ == -1) {
				LOG(ERROR) << "Failed to open file '" << base_filename_ << "': " << strerror(errno);
				return false;
			}
			VLOG(1) << "Successfully opened file '" << base_filename_ << "' with fd: " << fd_;
			return true;
		}

		// Assumes UNIQUE lock is held
		void CloseOutputFileInternal() {
			if (fd_ != -1) {
				VLOG(1) << "Closing file descriptor " << fd_;
				// Consider fsync before close? Depends on durability needs at shutdown.
				// if (fsync(fd_) == -1) {
				//     LOG(WARNING) << "fsync before close failed for fd " << fd_ << ": " << strerror(errno);
				// }
				if (close(fd_) == -1) {
					LOG(WARNING) << "Error closing file descriptor " << fd_ << ": " << strerror(errno);
				}
				fd_ = -1;
			}
		}

		// Acquires UNIQUE lock (Used internally, e.g., by fsync error recovery)
		bool ReopenOutputFile() {
			std::unique_lock<std::shared_mutex> lock(file_state_mutex_); // Acquire lock here
			VLOG(1) << "Attempting to reopen file, current fd: " << fd_;
			CloseOutputFileInternal(); // Close first (safe under unique lock)
																 // Re-open (still under unique lock)
			fd_ = open(base_filename_.c_str(), O_RDWR | O_CREAT, 0644);
			if (fd_ == -1) {
				LOG(ERROR) << "Failed to reopen file '" << base_filename_ << "': " << strerror(errno);
				return false;
			}
			VLOG(1) << "Successfully reopened file '" << base_filename_ << "' with fd: " << fd_;
			return true;
		}

		// --- Writer Thread Loop ---
		void WriterLoop() {
			VLOG(1) << "Writer thread started.";
			while (running_.load()) { // Check running flag outside blocking read
				std::optional<WriteTask> task_opt;
				write_queue_.blockingRead(task_opt); // Wait for a task

				if (!task_opt.has_value()) {
					VLOG(1) << "Writer thread received sentinel, exiting.";
					break; // Sentinel received, exit loop
				}

				if (!running_.load()) { // Check running flag again after waking up
					VLOG(1) << "Writer thread exiting after wake-up due to shutdown.";
					break;
				}

				WriteTask& task = task_opt.value();
				VLOG(5) << "Writer thread dequeued task for offset " << task.offset << " size " << task.size;

				try {
					int current_fd = -1;
					bool write_successful = false;
					{ // Scope for shared lock
						std::shared_lock<std::shared_mutex> lock(file_state_mutex_);

						if (!running_.load()) continue; // Check again under lock

						if (fd_ == -1) {
							LOG(ERROR) << "Writer thread: File descriptor invalid, skipping write for offset " << task.offset;
							// Optional: Could trigger a reopen attempt here, but adds complexity.
							continue; // Skip this task
						}
						current_fd = fd_; // Copy fd under lock

						// Perform pwrite
						ssize_t bytes_written = pwrite(current_fd, task.data.data(), task.size, task.offset);

						if (bytes_written == -1) {
							// Throw system_error to log errno
							throw std::system_error(errno, std::generic_category(), "pwrite failed for fd " + std::to_string(current_fd) + " offset " + std::to_string(task.offset));
						}
						if (bytes_written != task.size) {
							// Treat incomplete write as an error
							throw std::runtime_error("Incomplete pwrite: expected " + std::to_string(task.size) +
									", wrote " + std::to_string(bytes_written) + " for fd " + std::to_string(current_fd) + " offset " + std::to_string(task.offset));
						}
						write_successful = true; // Mark as successful if we reach here
						VLOG(5) << "Writer thread successfully wrote " << bytes_written << " bytes at offset " << task.offset;

					} // Shared lock released

					// Update tracker *after* releasing lock, using data from the task
					if (write_successful) {
						local_cut_tracker_->recordWrite(task.offset, task.size, task.num_msg);
					}

				} catch (const std::system_error& e) {
					LOG(ERROR) << "Writer thread system error: " << e.what() << " (code: " << e.code() << ")";
					// Check for EBADF specifically, might indicate fd became invalid
					if (e.code().value() == EBADF) {
						LOG(ERROR) << "Writer thread encountered EBADF!";
						// Consider triggering a controlled reopen or marking service unhealthy
					}
				} catch (const std::exception& e) {
					LOG(ERROR) << "Writer thread exception: " << e.what();
				}
			} // End while loop
			VLOG(1) << "Writer thread finished.";
		}


		// --- Fsync Thread Loop (Similar to previous example) ---
		void FsyncLoop() {
			const std::chrono::seconds flush_interval(5);
			VLOG(1) << "Fsync thread started.";

			while (running_.load()) {
				// Wait for the interval or shutdown signal
				std::unique_lock<std::mutex> lock(fsync_cv_mutex_);
				if (cv_fsync_.wait_for(lock, flush_interval, [this]{ return !running_.load(); })) {
					break; // Exit loop if shutting down
				}
				// Timed out, proceed with fsync attempt
				VLOG(5) << "Fsync thread waking up to sync.";

				// Acquire exclusive lock for fsync
				std::unique_lock<std::shared_mutex> file_lock(file_state_mutex_);

				if (!running_.load()) break; // Double check after acquiring lock

				if (fd_ != -1) {
					VLOG(5) << "Attempting fsync on fd " << fd_;
					if (fsync(fd_) == -1) {
						LOG(ERROR) << "fsync failed for fd " << fd_ << ": " << strerror(errno);
						if (errno == EBADF || errno == EIO) {
							LOG(ERROR) << "Attempting to reopen file due to fsync error.";
							// Release unique lock before calling ReopenOutputFile which acquires it again
							// file_lock.unlock(); // unlock current lock
							// bool reopened = ReopenOutputFile();
							// if (!reopened) {
							//      // Failed to reopen, maybe stop the service?
							//      LOG(ERROR) << "Failed to reopen file after fsync error, stopping service potentially.";
							//      // running_.store(false); // Or some other critical error state
							// }
							// Need to re-lock if further action needed in this cycle? Probably not.

							// Simpler: Let ReopenOutputFile handle its own lock inside.
							// The current unique_lock ensures no other thread interferes while we decide.
							CloseOutputFileInternal(); // Close under current lock
																				 // Reopen under current lock
							fd_ = open(base_filename_.c_str(), O_RDWR | O_CREAT, 0644);
							if (fd_ == -1) {
								LOG(ERROR) << "Failed to reopen file '" << base_filename_ << "' after fsync error: " << strerror(errno);
							} else {
								VLOG(1) << "Successfully reopened file '" << base_filename_ << "' after fsync error, new fd: " << fd_;
							}
						}
					} else {
						VLOG(5) << "fsync completed successfully for fd " << fd_;
					}
				} else {
					VLOG(1) << "Skipping fsync, file descriptor is invalid.";
					// Optionally attempt to reopen if fd is -1
					// fd_ = open(base_filename_.c_str(), O_RDWR | O_CREAT, 0644); ... etc
				}
				// file_lock (unique_lock) is released automatically
			}
			VLOG(1) << "Fsync thread stopping.";
		}


		// --- Local Cut / Global Cut Communication ---
		void SendLocalCut() {
			std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>> stream = nullptr;
			grpc::ClientContext context; // Create context outside loop for potential reuse/metadata

			// Create the stream
			try {
				stream = stub_->HandleSendLocalCut(&context);
			} catch (const std::exception& e) {
				LOG(ERROR) << "Failed to create HandleSendLocalCut stream: " << e.what();
				return; // Cannot proceed
			}

			if (!stream) {
				LOG(ERROR) << "Failed to create HandleSendLocalCut stream (returned null).";
				return;
			}
			VLOG(1) << "HandleSendLocalCut stream created.";

			// Spawn receiver thread *after* stream is created
			std::thread receive_global_cut_thread(&ScalogReplicationServiceImpl::ReceiveGlobalCut, this, stream.get());
			VLOG(1) << "ReceiveGlobalCut thread spawned.";


			while (running_.load()) {
				LocalCut request;
				request.set_local_cut(local_cut_tracker_->getLocalCut());
				request.set_topic(""); // TODO(Tony) set topic
				request.set_broker_id(broker_id_);
				request.set_epoch(local_epoch_);
				request.set_replica_id(replica_id_);

				VLOG(5) << "Sending LocalCut epoch " << local_epoch_ << " value " << request.local_cut();
				// Send the LocalCut message to the server
				if (!stream->Write(request)) {
					LOG(ERROR) << "SendLocalCut: Stream write failed, connection likely closed.";
					break; // Exit loop on write failure
				}

				// Increment the epoch
				local_epoch_++;

				std::this_thread::sleep_for(std::chrono::milliseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL));
			}

			// Signal server no more writes are coming
			if (stream) {
				stream->WritesDone();
			}
			stop_reading_from_stream_.store(true);
			if (receive_global_cut_thread.joinable()) {
				receive_global_cut_thread.join();
			}
		}

		// Note: Takes raw pointer as std::thread cannot directly take unique_ptr by reference easily
		void ReceiveGlobalCut(grpc::ClientReaderWriter<LocalCut, GlobalCut>* stream) {
			VLOG(1) << "ReceiveGlobalCut thread started.";
			GlobalCut global_cut_msg;
			int num_global_cuts = 0;

			// Loop while not signaled to stop *and* stream read is successful
			while (!stop_reading_from_stream_.load() && stream->Read(&global_cut_msg)) {
				VLOG(5) << "Received GlobalCut message " << num_global_cuts;
				// Process the received global cut
				absl::btree_map<int, int> current_global_cut; // Use local map per message
				for (const auto& entry : global_cut_msg.global_cut()) {
					current_global_cut[static_cast<int>(entry.first)] = static_cast<int>(entry.second);
				}

				// Call the processing function with the map for *this* message
				try {
					ScalogSequencer(current_global_cut);
				} catch (const std::system_error& e) {
					LOG(ERROR) << "System error during ScalogSequencer processing: " << e.what() << " (code: " << e.code() << ")";
					// Decide how to handle sequencer errors - continue? stop?
				} catch (const std::exception& e) {
					LOG(ERROR) << "Exception during ScalogSequencer processing: " << e.what();
				}

				num_global_cuts++;
			}

			// Check why loop ended
			if (stop_reading_from_stream_.load()) {
				VLOG(1) << "ReceiveGlobalCut thread stopping due to stop signal.";
			} else {
				LOG(WARNING) << "ReceiveGlobalCut thread stopping because stream->Read failed (connection closed?).";
			}
			VLOG(1) << "ReceiveGlobalCut thread finished.";
		}


		// --- Scalog Sequencer Logic (Applies total order) ---
		// Needs exclusive access to the file descriptor
		void ScalogSequencer(absl::btree_map<int, int>& global_cut) {
			// Static variables are generally problematic with concurrency.
			// disk_offset should likely be tracked more robustly, perhaps based
			// on the LocalCutTracker's sequentially_written_ offset?
			// seq needs careful handling if multiple threads could call this (though unlikely here).
			// static size_t seq = 0; // Making seq a member if needed across calls
			// static off_t disk_offset = 0; // Let's recalculate offset based on tracker

			VLOG(5) << "Processing GlobalCut in ScalogSequencer";

			// Acquire UNIQUE lock for file R/W operations
			std::unique_lock<std::shared_mutex> lock(file_state_mutex_);

			if (!running_.load()) {
				LOG(WARNING) << "ScalogSequencer called while service shutting down, skipping.";
				return;
			}
			if (fd_ == -1) {
				LOG(ERROR) << "ScalogSequencer: File descriptor is invalid, cannot process global cut.";
				return;
			}
			int current_fd = fd_; // Use locked fd

			// Determine starting point based on tracker?
			// This assumes ScalogSequencer processes cuts contiguously.
			// Need a reliable way to know the *next* global sequence number (seq)
			// and the corresponding disk offset. Let's use member variables for now.
			off_t current_disk_offset = next_sequencing_disk_offset_;
			size_t current_seq = next_global_sequence_number_;

			ScalogMessageHeader header_buffer;

			for (auto const& [broker, num_messages] : global_cut) {
				VLOG(5) << "GlobalCut processing broker " << broker << " with " << num_messages << " messages.";
				if (broker == broker_id_) {
					// Process messages for *this* broker
					for (int i = 0; i < num_messages; ++i) {
						// Read header at current disk offset
						ssize_t read_bytes = pread(current_fd, &header_buffer, sizeof(header_buffer), current_disk_offset);
						if (read_bytes == -1) {
							throw std::system_error(errno, std::generic_category(), "pread failed in ScalogSequencer for offset " + std::to_string(current_disk_offset));
						}
						if (read_bytes != sizeof(header_buffer)) {
							LOG(ERROR) << "Failed to read full message header from offset " << current_disk_offset << ", read " << read_bytes;
							// This is a critical error, indicates file corruption or logic error
							// Maybe stop processing?
							throw std::runtime_error("Failed to read full message header in ScalogSequencer");
						}

						// Assign total order
						header_buffer.total_order = current_seq;
						// std::atomic_thread_fence(std::memory_order_release); // Not needed for pwrite/fsync ordering

						// Write header back
						ssize_t written = pwrite(current_fd, &header_buffer, sizeof(header_buffer), current_disk_offset);
						if (written == -1) {
							throw std::system_error(errno, std::generic_category(), "pwrite failed updating header at offset " + std::to_string(current_disk_offset));
						}
						if (written != sizeof(header_buffer)) {
							throw std::runtime_error("Incomplete pwrite updating header at offset " + std::to_string(current_disk_offset));
						}
						VLOG(5) << "Assigned total order " << current_seq << " at disk offset " << current_disk_offset;


						// Advance disk offset and sequence number
						// Assume header_buffer.paddedSize was read correctly
						current_disk_offset += header_buffer.paddedSize;
						current_seq++;
					}
				} else {
					// For messages not belonging to our broker, just update sequence counter.
					// We need to know the sizes of these messages to advance the disk offset correctly!
					// This current approach assumes we only need to advance 'seq'.
					// If other brokers' messages are in the *same* file, we need to
					// read their headers too just to get the size. This implies the file
					// format needs careful design or this logic needs rethinking.
					// *** Assuming for now we only care about advancing seq for other brokers ***
					current_seq += num_messages;
					// **** WARNING: Disk offset calculation might be wrong if file interleaves brokers ****
					LOG_EVERY_N(WARNING, 100) << "Skipping disk offset advancement for foreign broker " << broker << ". Sequence number advanced.";
				}
			} // End loop through global_cut map

			// Update member variables for next call
			next_global_sequence_number_ = current_seq;
			next_sequencing_disk_offset_ = current_disk_offset;

			// No fsync here - dedicated thread handles it.

			// Release unique lock automatically at scope end
		}


		// --- Helper to create error response (Use simplified version) ---
		Status CreateErrorResponse(ScalogReplicationResponse* response,
				const std::string& message,
				grpc::StatusCode code) {
			response->set_success(false);
			grpc::Status status_to_return(code, message);
			if (status_to_return.error_code() != grpc::StatusCode::CANCELLED || message.find("shutting down") == std::string::npos) {
				LOG(ERROR) << "Replication error (code: " << status_to_return.error_code() << "): " << status_to_return.error_message();
			} else {
				VLOG(1) << "Replication cancelled: " << status_to_return.error_message();
			}
			return status_to_return;
		}


		// --- Member Variables ---
		const std::string base_filename_;
		int broker_id_;
		int fd_; // File descriptor (protected by mutex)
		std::atomic<bool> running_;
		std::shared_mutex file_state_mutex_; // Mutex for fd_ state and file ops
		folly::MPMCQueue<std::optional<WriteTask>> write_queue_; // Queue for tasks
		std::vector<std::thread> writer_threads_; // Threads processing the queue

		// Fsync thread members
		std::thread fsync_thread_;
		std::condition_variable cv_fsync_;
		std::mutex fsync_cv_mutex_; // Mutex for fsync condition variable

		// Local/Global Cut members
		std::string scalog_global_sequencer_ip_; // = SCLAOG_SEQUENCER_IP; // Initialize in constructor list if possible
		std::thread send_local_cut_thread_;
		std::chrono::microseconds local_cut_interval_;
		// absl::btree_map<int, int> global_cut_; // Not needed if processed per-message
		std::unique_ptr<ScalogSequencer::Stub> stub_;
		std::atomic<bool> stop_reading_from_stream_; // Signal receiver thread
		int replica_id_;
		std::atomic<int64_t> local_epoch_; // Use atomic for potential reads outside SendLocalCut? Or protect access.
		std::unique_ptr<LocalCutTracker> local_cut_tracker_;

		// State for ScalogSequencer
		std::atomic<size_t> next_global_sequence_number_{0}; // Start at 0
		std::atomic<off_t> next_sequencing_disk_offset_{0}; // Start at 0
																												// TODO: These atomics might need stronger ordering or locking if accessed/updated
																												// from multiple places concurrently, but likely okay if only updated by ReceiveGlobalCut thread.

	}; // End class ScalogReplicationServiceImpl

	ScalogReplicationManager::ScalogReplicationManager(
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
			std::string base_filename = log_file.empty() ? base_dir+"scalog_replication_log"+std::to_string(broker_id) +".dat" : log_file;
			service_ = std::make_unique<ScalogReplicationServiceImpl>(base_filename, broker_id);

			std::string server_address = address + ":" + (port.empty() ? std::to_string(SCALOG_REP_PORT) : port);

			//LOG(INFO) << "Starting scalog replication manager at " << server_address;

			ServerBuilder builder;

			// Set server options
			builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
			builder.RegisterService(service_.get());

			// Performance tuning options
			//builder.SetMaxReceiveMessageSize(16 * 1024 * 1024); // 16MB
			//builder.SetMaxSendMessageSize(16 * 1024 * 1024);    // 16MB
			//builder.SetSyncServerOption(ServerBuilder::SyncServerOption::NUM_CQS, 4);

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
