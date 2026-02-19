#include "publisher.h"
#include "publisher_profile.h"
#include "common/config.h"
#include "common/scoped_fd.h"
#include "absl/container/flat_hash_map.h"
#include "absl/cleanup/cleanup.h"
#include <cstring>
#include <random>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>  // For getenv, atoi
#include <fstream>
#include <iomanip>
#include <numeric>
#include <thread>

// [[CODE_CLEANUP]] Removed no-op profiling stubs - if profiling is needed, implement properly

/** EAGAIN epoll wait timeout (ms). 0 = busy poll; 1 = yield 1ms. Env EMBARCADERO_EPOLL_WAIT_WRITABLE_MS (default 1). */
static int GetEpollWaitWritableMs() {
	static int cached = -1;
	if (cached >= 0) return cached;
	const char* env = std::getenv("EMBARCADERO_EPOLL_WAIT_WRITABLE_MS");
	cached = (env && *env && std::atoi(env) == 0) ? 0 : 1;
	return cached;
}

static bool QueueDiagEnabled() {
	static const bool enabled = []() {
		const char* env = std::getenv("EMBARCADERO_QUEUE_DIAG");
		return env && env[0] && env[0] != '0';
	}();
	return enabled;
}

static uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void UpdateMax(std::atomic<uint64_t>& dst, uint64_t value) {
	uint64_t prev = dst.load(std::memory_order_relaxed);
	while (value > prev && !dst.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {}
}

namespace {
constexpr int kAckPortMin = 10000;
constexpr int kAckPortMax = 65535;
constexpr int kAckPortRange = kAckPortMax - kAckPortMin + 1;
}  // namespace

Publisher::Publisher(char topic[TOPIC_NAME_SIZE], std::string head_addr, std::string port, 
		int num_threads_per_broker, size_t message_size, size_t queueSize, 
		int order, SequencerType seq_type)
	: head_addr_(head_addr),
	port_(port),
	client_id_(GenerateRandomNum()),
	num_threads_per_broker_(num_threads_per_broker),
	message_size_(message_size),
	queueSize_((num_threads_per_broker > 0) ? (queueSize / static_cast<size_t>(num_threads_per_broker)) : queueSize),
		// [[NEW_BUFFER_FIX]] Start with reasonable initial queue count, dynamically grow as brokers are added.
		// Avoid massive 128-queue allocation when only 4-8 threads are needed initially.
		// See docs/NEW_BUFFER_BANDWIDTH_INVESTIGATION.md.
		pubQue_(num_threads_per_broker_ * 4, num_threads_per_broker_, client_id_, message_size, order),
	seq_type_(seq_type),
	broker_stats_(NUM_MAX_BROKERS),
	start_time_(std::chrono::steady_clock::now())  // Initialize immediately
#ifdef COLLECT_LATENCY_STATS
	,send_records_per_broker_(NUM_MAX_BROKERS),
	send_records_mutexes_(NUM_MAX_BROKERS)
#endif
	{

		// Copy topic name
		memcpy(topic_, topic, TOPIC_NAME_SIZE);

		// Create gRPC stub for head broker
		std::string addr = head_addr + ":" + port;
		stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));

		// Initialize first broker
		nodes_[0] = head_addr + ":" + std::to_string(PORT);
		brokers_.emplace_back(0);

		VLOG(3) << "Publisher constructed with client_id: " << client_id_ 
			<< ", topic: " << topic 
			<< ", num_threads_per_broker: " << num_threads_per_broker_;
	}

Publisher::~Publisher() {
	VLOG(3) << "Publisher destructor called, cleaning up resources";

	// Signal all threads to terminate [[RELAXED: Simple flags don't need ordering]]
	publish_finished_.store(true, std::memory_order_relaxed);
	shutdown_.store(true, std::memory_order_relaxed);
	consumer_should_exit_.store(true, std::memory_order_relaxed);
	// Cancel current gRPC SubscribeToCluster call so cluster_probe_thread_ can exit (reader->Read() unblocks).
	if (grpc::ClientContext* ctx = subscribe_context_.exchange(nullptr)) {
		ctx->TryCancel();
	}

	// Wait for all threads to complete (only if not already joined)
	for (auto& t : threads_) {
		if(t.joinable()){
			try {
				t.join();
			} catch (const std::exception& e) {
				LOG(ERROR) << "Exception in destructor joining thread: " << e.what();
			}
		}
	}

	if(cluster_probe_thread_.joinable()){
		cluster_probe_thread_.join();
	}

	if (ack_thread_.joinable()) {
		ack_thread_.join();
	}

	if (real_time_throughput_measure_thread_.joinable()) {
		real_time_throughput_measure_thread_.join();
	}

	if (kill_brokers_thread_.joinable()) {
		kill_brokers_thread_.join();
	}

	VLOG(3) << "Publisher destructor return";
}

#ifdef COLLECT_LATENCY_STATS
void Publisher::RecordPublishSend(int broker_id, size_t end_count) {
	if (!record_results_ || ack_level_ < 1 || end_count == 0) {
		return;
	}
	BatchSendRecord record{end_count, std::chrono::steady_clock::now()};
	{
		std::lock_guard<std::mutex> lock(send_records_mutexes_[broker_id]);
		send_records_per_broker_[broker_id].push_back(record);
	}
}
#endif

#ifdef COLLECT_LATENCY_STATS
void Publisher::ProcessPublishAckLatency(int broker_id, size_t acked_msg) {
	if (!record_results_ || ack_level_ < 1) {
		return;
	}
	std::vector<long long> local_latencies;
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(send_records_mutexes_[broker_id]);
		auto& records = send_records_per_broker_[broker_id];
		// Multiple PublishThreads can push to the same broker, so records are not
		// necessarily ordered by end_count. Remove all records with end_count <= acked_msg.
		std::deque<BatchSendRecord> remaining;
		for (auto& record : records) {
			if (record.end_count <= acked_msg) {
				auto latency = std::chrono::duration_cast<std::chrono::microseconds>(now - record.sent_time).count();
				local_latencies.push_back(latency);
			} else {
				remaining.push_back(std::move(record));
			}
		}
		records = std::move(remaining);
	}
	if (!local_latencies.empty()) {
		std::lock_guard<std::mutex> lock(publish_latency_mutex_);
		publish_latencies_us_.insert(publish_latencies_us_.end(), local_latencies.begin(), local_latencies.end());
	}
}
#endif

#ifdef COLLECT_LATENCY_STATS
void Publisher::WritePublishLatencyResults() {
	if (!record_results_ || ack_level_ < 1) {
		return;
	}
	std::vector<long long> latencies_copy;
	{
		std::lock_guard<std::mutex> lock(publish_latency_mutex_);
		latencies_copy = publish_latencies_us_;
	}
	if (latencies_copy.empty()) {
		LOG(WARNING) << "No publish latency values could be calculated.";
		return;
	}

	std::sort(latencies_copy.begin(), latencies_copy.end());
	const size_t count = latencies_copy.size();
	const long double sum = std::accumulate(latencies_copy.begin(), latencies_copy.end(), 0.0L);
	const long long min_us = latencies_copy.front();
	const long long max_us = latencies_copy.back();
	const long long median_us = latencies_copy[count / 2];
	const long long p99_us = latencies_copy[static_cast<size_t>(std::floor(0.99 * count))];
	const long long p999_us = latencies_copy[static_cast<size_t>(std::floor(0.999 * count))];
	const long double avg_us = sum / static_cast<long double>(count);

	LOG(INFO) << "Publish Latency Statistics (us):";
	LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << avg_us;
	LOG(INFO) << "  Min:     " << min_us;
	LOG(INFO) << "  Median:  " << median_us;
	LOG(INFO) << "  99th P:  " << p99_us;
	LOG(INFO) << "  99.9th P:" << p999_us;
	LOG(INFO) << "  Max:     " << max_us;

	const std::string latency_filename = "pub_latency_stats.csv";
	std::ofstream latency_file(latency_filename);
	if (!latency_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << latency_filename;
	} else {
		latency_file << "Average,Min,Median,p99,p999,Max\n";
		latency_file << std::fixed << std::setprecision(3) << avg_us
			<< "," << min_us
			<< "," << median_us
			<< "," << p99_us
			<< "," << p999_us
			<< "," << max_us << "\n";
		latency_file.close();
	}

	const std::string cdf_filename = "pub_cdf_latency_us.csv";
	std::ofstream cdf_file(cdf_filename);
	if (!cdf_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << cdf_filename;
	} else {
		cdf_file << "Latency_us,CumulativeProbability\n";
		for (size_t i = 0; i < count; ++i) {
			const long long current_latency = latencies_copy[i];
			const double cumulative_probability = static_cast<double>(i + 1) / count;
			cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability << "\n";
		}
		cdf_file.close();
	}
}
#endif


void Publisher::Init(int ack_level) {
	ack_level_ = ack_level;

	// When set, PublishThread updates total_batches_attempted_ so ACK timeout log shows attempted count.
	const char* ack_debug = std::getenv("EMBARCADERO_ACK_TIMEOUT_DEBUG");
	enable_batch_attempted_for_timeout_log_ = (ack_debug && ack_debug[0] && (ack_debug[0] == '1' || ack_debug[0] == 'y' || ack_debug[0] == 'Y'));

	// Generate unique port for acknowledgment server with retry logic
	// Ensure port is always in safe range 10000-65535 (avoid privileged ports < 1024)
	// Use modulo to ensure it fits in valid port range
	ack_port_ = (GenerateRandomNum() % (65535 - 10000 + 1)) + 10000;

	// Start acknowledgment thread if needed
	if (ack_level >= 1) {
		ack_thread_ = std::thread([this]() {
				this->EpollAckThread();
				});

		// Wait for acknowledgment thread to initialize (with timeout — EpollAckThread may fail to start)
		constexpr auto ACK_THREAD_INIT_TIMEOUT = std::chrono::seconds(30);
		auto ack_wait_start = std::chrono::steady_clock::now();
		while (thread_count_.load(std::memory_order_acquire) != 1) {
			auto elapsed = std::chrono::steady_clock::now() - ack_wait_start;
			if (elapsed >= ACK_THREAD_INIT_TIMEOUT) {
				LOG(ERROR) << "Publisher::Init() timed out after " << ACK_THREAD_INIT_TIMEOUT.count()
				           << "s waiting for ACK thread. EpollAckThread may have failed (e.g. bind/listen).";
				break;
			}
			std::this_thread::yield();
		}
		thread_count_.store(0, std::memory_order_release);
	}

	// Start cluster status monitoring thread
	cluster_probe_thread_ = std::thread([this]() {
			this->SubscribeToClusterStatus();
			});

	// Wait for connection to be established with timeout and logging
	auto connection_start = std::chrono::steady_clock::now();
	auto last_log_time = connection_start;
	constexpr auto CONNECTION_TIMEOUT = std::chrono::seconds(60);
	constexpr auto LOG_INTERVAL = std::chrono::seconds(5);

	while (!connected_.load(std::memory_order_acquire)) {  // [[CRITICAL_FIX: Atomic load with acquire semantics]]
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - connection_start);

		// Check for timeout
		if (elapsed >= CONNECTION_TIMEOUT) {
			LOG(ERROR) << "Publisher::Init() timed out waiting for cluster connection after "
			           << elapsed.count() << " seconds. This indicates gRPC SubscribeToCluster is failing.";
			LOG(ERROR) << "Check broker gRPC service availability and network connectivity.";
			break; // Exit to avoid infinite hang
		}

		// Log progress every 5 seconds
		if (now - last_log_time >= LOG_INTERVAL) {
			LOG(WARNING) << "Publisher::Init() waiting for cluster connection... ("
			            << elapsed.count() << "s elapsed)";
			last_log_time = now;
		}

		Embarcadero::CXL::cpu_pause();
	}

	if (!connected_.load(std::memory_order_acquire)) {  // [[CRITICAL_FIX: Atomic load]]
		LOG(ERROR) << "Publisher::Init() failed - cluster connection was not established. "
		          << "Publisher will not be able to send messages.";
	}

	// Initialize Corfu sequencer if needed
	if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
		corfu_client_ = std::make_unique<CorfuSequencerClient>(
				CORFU_SEQUENCER_ADDR + std::to_string(CORFU_SEQ_PORT));
	}

	// [[Issue 6]] Wait for all publisher threads to initialize with timeout
	constexpr auto THREAD_INIT_TIMEOUT = std::chrono::seconds(60);
	auto thread_wait_start = std::chrono::steady_clock::now();
	while (thread_count_.load(std::memory_order_acquire) != num_threads_.load(std::memory_order_acquire)) {
		auto elapsed = std::chrono::steady_clock::now() - thread_wait_start;
		if (elapsed >= THREAD_INIT_TIMEOUT) {
			LOG(ERROR) << "Publisher::Init() timed out after " << THREAD_INIT_TIMEOUT.count()
			           << "s waiting for thread_count_ (" << thread_count_.load(std::memory_order_relaxed)
			           << ") == num_threads_ (" << num_threads_.load(std::memory_order_relaxed) << ")";
			break;
		}
		std::this_thread::yield();
	}

	// [[FIX: B3=0 ACKs]] Wait for all expected broker ACK connections to be established
	// This prevents the race where publishing completes before all ACK connections are up
	if (ack_level_ >= 1) {
		constexpr auto ACK_CONNECTION_TIMEOUT = std::chrono::seconds(30);
		auto ack_wait_start = std::chrono::steady_clock::now();
		auto last_log_time = ack_wait_start;
		int expected = expected_ack_brokers_.load(std::memory_order_acquire);

		while (expected > 0) {
			int connected_count;
			{
				absl::MutexLock lock(&mutex_);
				connected_count = static_cast<int>(brokers_with_ack_connection_.size());
			}

			if (connected_count >= expected) {
				LOG(INFO) << "Publisher::Init() All " << expected << " broker ACK connections established";
				break;
			}

			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - ack_wait_start);

			// Log progress every 2 seconds
			if (now - last_log_time >= std::chrono::seconds(2)) {
				VLOG(1) << "Publisher::Init() Waiting for broker ACK connections: "
				         << connected_count << " / " << expected << " (elapsed: " << elapsed.count() << "s)";
				last_log_time = now;
			}

			if (elapsed >= ACK_CONNECTION_TIMEOUT) {
				LOG(WARNING) << "Publisher::Init() ACK connection timeout after " << ACK_CONNECTION_TIMEOUT.count()
				           << "s. Only " << connected_count << " of " << expected << " brokers connected. "
				           << "Some brokers may not send ACKs (B*=0 ACK issue).";
				// Log which brokers are missing
				{
					absl::MutexLock lock(&mutex_);
					std::string connected_str, missing_str;
					for (int bid : brokers_with_ack_connection_) {
						if (!connected_str.empty()) connected_str += ", ";
						connected_str += "B" + std::to_string(bid);
					}
					for (int bid : brokers_) {
						if (brokers_with_ack_connection_.find(bid) == brokers_with_ack_connection_.end()) {
							if (!missing_str.empty()) missing_str += ", ";
							missing_str += "B" + std::to_string(bid);
						}
					}
					LOG(WARNING) << "  Connected brokers: " << (connected_str.empty() ? "(none)" : connected_str);
					LOG(WARNING) << "  Missing brokers: " << (missing_str.empty() ? "(none)" : missing_str);
				}
				break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}
}

void Publisher::WarmupBuffers() {
	// Delegate to the Buffer class which has access to private members
	pubQue_.WarmupBuffers();
}

void Publisher::Publish(char* message, size_t len) {
	const bool queue_diag = QueueDiagEnabled();
	uint64_t write_start_ns = 0;
	if (queue_diag) {
		write_start_ns = NowNs();
		diag_publish_calls_.fetch_add(1, std::memory_order_relaxed);
		uint64_t prev_end_ns = diag_last_publish_end_ns_.load(std::memory_order_relaxed);
		if (prev_end_ns != 0 && write_start_ns > prev_end_ns) {
			uint64_t gap_ns = write_start_ns - prev_end_ns;
			diag_publish_gap_ns_.fetch_add(gap_ns, std::memory_order_relaxed);
			UpdateMax(diag_publish_gap_ns_max_, gap_ns);
			if (gap_ns >= 10'000) diag_publish_gap_over_10us_.fetch_add(1, std::memory_order_relaxed);
			if (gap_ns >= 50'000) diag_publish_gap_over_50us_.fetch_add(1, std::memory_order_relaxed);
			if (gap_ns >= 200'000) diag_publish_gap_over_200us_.fetch_add(1, std::memory_order_relaxed);
		}
	}

	const static size_t header_size = sizeof(Embarcadero::MessageHeader);
	// Padding and total size are cached per-thread for the hot path.
	size_t padded_total;
	{
		static thread_local size_t cached_len = 0;
		static thread_local size_t cached_padded_total = 0;
		if (cached_len == len && cached_padded_total != 0) {
			padded_total = cached_padded_total;
		} else {
			size_t padded = len % 64;
			if (padded) padded = 64 - padded;
			padded_total = len + padded + header_size;
			cached_len = len;
			cached_padded_total = padded_total;
		}
	}

	// [[PERF]] Per-message order for header only (subscriber ordering). client_order_ updated per batch when sealed.
	size_t my_order = next_publish_order_++;
	auto [ok, sealed] = pubQue_.Write(my_order, message, len, padded_total);
	if (!ok) {
		LOG(ERROR) << "Failed to write message to queue (client_order=" << my_order << ")";
	} else if (sealed > 0) {
		client_order_.fetch_add(sealed, std::memory_order_release);
	}
	if (queue_diag) {
		uint64_t write_end_ns = NowNs();
		uint64_t write_ns = write_end_ns - write_start_ns;
		diag_publish_write_ns_.fetch_add(write_ns, std::memory_order_relaxed);
		UpdateMax(diag_publish_write_ns_max_, write_ns);
		diag_last_publish_end_ns_.store(write_end_ns, std::memory_order_relaxed);
	}

}

bool Publisher::Poll(size_t n) {
	// [[LAST_PERCENT_ACK_FIX]] Seal and return reads before signaling finished.
	// If we set publish_finished_ first, threads that get nullptr from Read() may exit
	// before we've called SealAll(), dropping the last batches.
	WriteFinishedOrPaused();

	// Signal threads before releasing queue resources [[RELAXED]]
	publish_finished_.store(true, std::memory_order_relaxed);
	consumer_should_exit_.store(true, std::memory_order_relaxed);
	pubQue_.ReturnReads();

	constexpr auto SPIN_DURATION = std::chrono::milliseconds(1);
	while (client_order_.load(std::memory_order_acquire) < n) {
		auto spin_start = std::chrono::steady_clock::now();
		const auto spin_end = spin_start + SPIN_DURATION;
		while (std::chrono::steady_clock::now() < spin_end && client_order_.load(std::memory_order_acquire) < n) {
			Embarcadero::CXL::cpu_pause();
		}
		if (client_order_.load(std::memory_order_acquire) < n) {
			std::this_thread::yield();
		}
	}

	// All messages queued, waiting for transmission to complete

	// CRITICAL FIX: Use atomic flag to prevent double-join race conditions
	if (!threads_joined_.exchange(true)) {
		// Only join threads once
		for (size_t i = 0; i < threads_.size(); ++i) {
			if (threads_[i].joinable()) {
				try {
				// Joining publisher thread
				threads_[i].join();
				// Successfully joined publisher thread
				} catch (const std::exception& e) {
					LOG(ERROR) << "Exception joining publisher thread " << i << ": " << e.what();
				}
			}
			// Publisher thread not joinable (already joined or detached)
		}
		// All publisher threads completed transmission
	}

	if (QueueDiagEnabled()) {
		LogPublisherQueueDiagnostics();
		pubQue_.LogQueueDiagnostics();
	}
	
	// If acknowledgments are enabled, wait for all acks
	if (ack_level_ >= 1) {
		auto wait_start_time = std::chrono::steady_clock::now();
		auto last_log_time = wait_start_time;
		// [[CONFIG: Ack-wait spin]] 500µs spin when waiting for acks (burst-friendly); was 1ms.
		constexpr auto SPIN_DURATION = std::chrono::microseconds(500);
		
		// Configurable timeout for ACK waits. Override via EMBARCADERO_ACK_TIMEOUT_SEC (0 = no timeout).
		// ORDER=2/5 need sequencer + CXL propagation; 10s is too short for 8GB+. Default 120s when ack>=1.
		const char* timeout_env = std::getenv("EMBARCADERO_ACK_TIMEOUT_SEC");
		int timeout_seconds = timeout_env ? std::atoi(timeout_env) : 120;  // 120s default for ack_level>=1
		const auto timeout_duration = std::chrono::seconds(timeout_seconds);
		// [[FIX: ACK Race Condition]] Capture target ONCE - never reload inside loop
		// Reloading allowed concurrent Publish() calls to move the target, causing potential infinite wait
		const size_t target_acks = client_order_.load(std::memory_order_acquire);

		while (ack_received_.load(std::memory_order_acquire) < target_acks) {
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - wait_start_time);

			// Check timeout
			if (timeout_seconds > 0 && elapsed >= timeout_duration) {
				LOG(ERROR) << "[Publisher ACK Timeout]: Waited " << elapsed.count()
					<< " seconds for ACKs, received " << ack_received_ << " out of " << target_acks
					<< " (timeout=" << timeout_seconds << "s)";
				LOG(ERROR) << "[Publisher ACK Diagnostics]: ack_level=" << ack_level_
					<< ", last_ack_received=" << ack_received_ << ", client_order=" << target_acks;
				LOG(ERROR) << "[Publisher Batch Stats]: total_batches_sent=" << total_batches_sent_.load(std::memory_order_relaxed)
					<< " attempted=" << total_batches_attempted_.load(std::memory_order_relaxed)
					<< " failed=" << total_batches_failed_.load(std::memory_order_relaxed)
					<< " (sent_all=" << (total_batches_failed_.load(std::memory_order_relaxed) == 0 ? "yes" : "no") << ")";
				// Per-broker counts to pinpoint which broker(s) are short (sent vs acked)
				std::string per_broker;
				for (size_t i = 0; i < broker_stats_.size(); i++) {
					size_t sent = broker_stats_[i].sent_messages.load(std::memory_order_relaxed);
					size_t acked = broker_stats_[i].acked_messages;
					if (i) per_broker += " ";
					per_broker += "B" + std::to_string(i) + "=" + std::to_string(acked);
					if (sent != 0 || acked != 0) {
						per_broker += "(sent=" + std::to_string(sent);
						if (sent > acked) per_broker += ",short=" + std::to_string(sent - acked);
						per_broker += ")";
					}
				}
				LOG(ERROR) << "[Publisher ACK Per-Broker]: " << per_broker;
				// Return failure - caller should handle timeout appropriately
				return false;  // Exit early on timeout
			}

			if(kill_brokers_){
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count() >= 100) {
					break;
				}
			}
			if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 1) {
				std::string per_broker;
				for (size_t i = 0; i < broker_stats_.size(); i++) {
					if (i) per_broker += " ";
					per_broker += "B" + std::to_string(i) + "=" + std::to_string(broker_stats_[i].acked_messages);
				}
				VLOG(1) << "Waiting for acknowledgments, received " << ack_received_.load(std::memory_order_relaxed) << " out of " << target_acks
					<< " (elapsed: " << elapsed.count() << "s"
					<< (timeout_seconds > 0 ? ", timeout: " + std::to_string(timeout_seconds) + "s" : "") << ") [" << per_broker << "]";
				last_log_time = now;
			}

			// [[REMOVED: co = client_order_.load()]] - This caused the race condition!
			auto spin_start = std::chrono::steady_clock::now();
			const auto spin_end = spin_start + SPIN_DURATION;
			while (std::chrono::steady_clock::now() < spin_end && ack_received_.load(std::memory_order_acquire) < target_acks) {
				Embarcadero::CXL::cpu_pause();
			}
			if (ack_received_.load(std::memory_order_acquire) < target_acks) {
				std::this_thread::yield();
		}
	}
		// Only treat as success if we actually received ACKs for all messages
		const size_t received = ack_received_.load(std::memory_order_relaxed);
		if (received < target_acks) {
			LOG(ERROR) << "[Publisher ACK Failure]: Did not receive ACKs for all messages. received="
			           << received << " target=" << target_acks << " short=" << (target_acks - received);
			std::string per_broker;
			for (size_t i = 0; i < broker_stats_.size(); i++) {
				size_t sent = broker_stats_[i].sent_messages.load(std::memory_order_relaxed);
				size_t acked = broker_stats_[i].acked_messages;
				if (i) per_broker += " ";
				per_broker += "B" + std::to_string(i) + "=" + std::to_string(acked);
				if (sent != 0 || acked != 0) {
					per_broker += "(sent=" + std::to_string(sent);
					if (sent > acked) per_broker += ",short=" + std::to_string(sent - acked);
					per_broker += ")";
				}
			}
			LOG(ERROR) << "[Publisher ACK Per-Broker]: " << per_broker;
			return false;
		}
		LOG(INFO) << "[ACK_VERIFY] received=" << received << " target=" << target_acks << " 100%";
		// [[ORDER_0_TAIL_ACK]] Drain so EpollAckThread can read in-flight ACKs before we return.
		const char* drain_env = std::getenv("EMBARCADERO_ACK_DRAIN_MS");
		int drain_ms = drain_env ? std::atoi(drain_env) : 50;
		if (drain_ms > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(drain_ms));
		}
	}

	// IMPROVED: Graceful disconnect - keep gRPC context alive for subscriber
	// Only set publish_finished flag, don't shutdown entire system
	// The gRPC context remains active to support subscriber cluster management
	// Publisher data connections are already closed by joined threads
#ifdef COLLECT_LATENCY_STATS
	WritePublishLatencyResults();
#endif
	
	LOG(INFO) << "Publisher finished sending " << client_order_.load(std::memory_order_relaxed) << " messages, keeping cluster context alive for subscriber";
	
	// NOTE: We do NOT set shutdown_=true or cancel context here
	// This allows the subscriber to continue using the cluster management infrastructure
	// The context will be cleaned up when the Publisher object is destroyed
	return true;
}

void Publisher::DEBUG_check_send_finish() {
	// [[ACK_TIMEOUT_FIX]] Do NOT set publish_finished_ here. Poll() sets it after SealAll().
	// Setting it here lets PublishThreads exit on nullptr+publish_finished before all batches
	// are sealed and read, causing ~828 batches to never be sent and ACK timeout.
	WriteFinishedOrPaused();
	pubQue_.ReturnReads();

	// CRITICAL FIX: Don't join threads here as Poll() will handle thread cleanup
	// This prevents double-join issues and race conditions
	// DEBUG_check_send_finish: Seal and return reads only; Poll() will set publish_finished_ and join
}

void Publisher::FailBrokers(size_t total_message_size, size_t message_size,
		double failure_percentage, 
		std::function<bool()> killbrokers) {
	kill_brokers_.store(true, std::memory_order_release);

	measure_real_time_throughput_ = true;
	size_t num_brokers = nodes_.size();

	// Initialize counters for sent bytes and sent messages
	for (size_t i = 0; i < num_brokers; i++) {
		broker_stats_[i].sent_bytes.store(0);
		broker_stats_[i].sent_messages.store(0);
		broker_stats_[i].acked_messages = 0;
	}

	// Start thread to monitor progress and kill brokers at specified percentage
	kill_brokers_thread_ = std::thread([=, this]() {
		size_t bytes_to_kill_brokers = total_message_size * failure_percentage;

		while (!shutdown_.load(std::memory_order_relaxed) && total_sent_bytes_.load(std::memory_order_acquire) < bytes_to_kill_brokers) {
			std::this_thread::yield();
		}

		if (!shutdown_.load(std::memory_order_relaxed)) {
			killbrokers();
		}
	});

	// Start thread to measure real-time throughput
	real_time_throughput_measure_thread_ = std::thread([=, this]() {
		std::vector<size_t> prev_throughputs(num_brokers, 0);

		// Open file for writing throughput data
		//TODO(Jae) Rewrite this to be relative path
		std::string home_dir = getenv("HOME") ? getenv("HOME") : "."; // Get home dir or use current
		std::string filename = home_dir + "/Embarcadero/data/failure/real_time_acked_throughput.csv";
		std::ofstream throughputFile(filename);
		if (!throughputFile.is_open()) {
		LOG(ERROR) << "Failed to open file for writing throughput data: " << filename;
		return;
		}

		// Write CSV header
		throughputFile << "Timestamp(ms)"; // Add timestamp column
		for (size_t i = 0; i < num_brokers; i++) {
		throughputFile << ",Broker_" << i << "_GBps";
		}
		throughputFile << ",Total_GBps\n";

		// Measuring loop
		const int measurement_interval_ms = 5;
		const double time_factor_gbps = (1000.0 / measurement_interval_ms) / (1024.0 * 1024.0 * 1024.0); // Factor to get GB/s

		while (!shutdown_.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(measurement_interval_ms));
			size_t sum = 0;
			auto now = std::chrono::steady_clock::now();
			auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
			throughputFile << timestamp_ms; // Write timestamp

			for (size_t i = 0; i < num_brokers; i++) {
				size_t bytes = broker_stats_[i].acked_messages * message_size;
				size_t real_time_throughput = (bytes - prev_throughputs[i]);

				// Convert to GB/s for CSV
				double gbps = real_time_throughput * time_factor_gbps;
				throughputFile << "," << gbps;

				sum += real_time_throughput;
				prev_throughputs[i] = bytes;
			}

			// Convert total to GB/s
			double total_gbps = (sum * time_factor_gbps); 
			throughputFile << "," << total_gbps << "\n";
		}

		throughputFile.flush();
		throughputFile.close();
	});
}

void Publisher::WriteFinishedOrPaused() {
	size_t sealed = pubQue_.SealAll();
	if (sealed > 0) {
		client_order_.fetch_add(sealed, std::memory_order_release);
	}
}

void Publisher::LogPublisherQueueDiagnostics() const {
	if (!QueueDiagEnabled()) {
		return;
	}

	const uint64_t publish_calls = diag_publish_calls_.load(std::memory_order_relaxed);
	const uint64_t publish_write_ns = diag_publish_write_ns_.load(std::memory_order_relaxed);
	const uint64_t publish_write_ns_max = diag_publish_write_ns_max_.load(std::memory_order_relaxed);
	const uint64_t publish_gap_ns = diag_publish_gap_ns_.load(std::memory_order_relaxed);
	const uint64_t publish_gap_ns_max = diag_publish_gap_ns_max_.load(std::memory_order_relaxed);
	const uint64_t gap_over_10us = diag_publish_gap_over_10us_.load(std::memory_order_relaxed);
	const uint64_t gap_over_50us = diag_publish_gap_over_50us_.load(std::memory_order_relaxed);
	const uint64_t gap_over_200us = diag_publish_gap_over_200us_.load(std::memory_order_relaxed);
	const uint64_t empty_reads = diag_consumer_empty_reads_.load(std::memory_order_relaxed);
	const uint64_t consumer_wait_ns = diag_consumer_wait_ns_.load(std::memory_order_relaxed);
	const uint64_t consumer_wait_ns_max = diag_consumer_wait_ns_max_.load(std::memory_order_relaxed);
	const uint64_t consumer_yields = diag_consumer_yields_.load(std::memory_order_relaxed);
	const uint64_t consumer_batches = diag_consumer_batches_.load(std::memory_order_relaxed);
	const uint64_t consumer_messages = diag_consumer_messages_.load(std::memory_order_relaxed);
	const uint64_t send_eagain_events = diag_send_eagain_events_.load(std::memory_order_relaxed);
	const uint64_t send_eagain_wait_ns = diag_send_eagain_wait_ns_.load(std::memory_order_relaxed);
	const uint64_t send_eagain_wait_ns_max = diag_send_eagain_wait_ns_max_.load(std::memory_order_relaxed);

	LOG(INFO) << "[ORDER0_ACK1_PUBLISHER_QUEUE]"
	          << " publish_calls=" << publish_calls
	          << " publish_write_us=" << (publish_write_ns / 1000)
	          << " publish_write_us_avg=" << (publish_calls ? (publish_write_ns / publish_calls) / 1000 : 0)
	          << " publish_write_us_max=" << (publish_write_ns_max / 1000)
	          << " publish_gap_us=" << (publish_gap_ns / 1000)
	          << " publish_gap_us_avg=" << (publish_calls > 1 ? (publish_gap_ns / (publish_calls - 1)) / 1000 : 0)
	          << " publish_gap_us_max=" << (publish_gap_ns_max / 1000)
	          << " publish_gap_over_10us=" << gap_over_10us
	          << " publish_gap_over_50us=" << gap_over_50us
	          << " publish_gap_over_200us=" << gap_over_200us
	          << " empty_reads=" << empty_reads
	          << " consumer_wait_us=" << (consumer_wait_ns / 1000)
	          << " consumer_wait_us_max=" << (consumer_wait_ns_max / 1000)
	          << " consumer_yields=" << consumer_yields
	          << " consumer_batches=" << consumer_batches
	          << " consumer_messages=" << consumer_messages
	          << " send_eagain_events=" << send_eagain_events
	          << " send_eagain_wait_us=" << (send_eagain_wait_ns / 1000)
	          << " send_eagain_wait_us_max=" << (send_eagain_wait_ns_max / 1000);
}

void Publisher::EpollAckThread() {
	if (ack_level_ < 1) {
		return;
	}

	// Create server socket
	int server_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (server_sock < 0) {
		LOG(ERROR) << "Socket creation failed: " << strerror(errno);
		return;
	}

	// Configure socket options
	int flag = 1;
	if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
		LOG(ERROR) << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno);
		close(server_sock);
		return;
	}

	// Disable Nagle's algorithm for better latency
	if (setsockopt(server_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
		LOG(ERROR) << "setsockopt(TCP_NODELAY) failed: " << strerror(errno);
	}

	// Enable TCP_QUICKACK for low-latency ACKs
	if (setsockopt(server_sock, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag)) < 0) {
		LOG(WARNING) << "setsockopt(TCP_QUICKACK) failed: " << strerror(errno);
		// Non-fatal, continue
	}

	// Increase socket buffers for high-throughput (32MB)
	const int buffer_size = 32 * 1024 * 1024;  // 32 MB
	if (setsockopt(server_sock, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_SNDBUF) failed: " << strerror(errno);
		// Non-fatal, continue
	} else {
		int actual = 0;
		socklen_t len = sizeof(actual);
		if (getsockopt(server_sock, SOL_SOCKET, SO_SNDBUF, &actual, &len) == 0 &&
		    actual < buffer_size) {
			LOG(WARNING) << "SO_SNDBUF capped: requested " << buffer_size << " got " << actual
			             << ". Raise net.core.wmem_max (e.g. scripts/tune_kernel_buffers.sh)";
		}
	}
	if (setsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
		LOG(WARNING) << "setsockopt(SO_RCVBUF) failed: " << strerror(errno);
		// Non-fatal, continue
	} else {
		int actual = 0;
		socklen_t len = sizeof(actual);
		if (getsockopt(server_sock, SOL_SOCKET, SO_RCVBUF, &actual, &len) == 0 &&
		    actual < buffer_size) {
			LOG(WARNING) << "SO_RCVBUF capped: requested " << buffer_size << " got " << actual
			             << ". Raise net.core.rmem_max (e.g. scripts/tune_kernel_buffers.sh)";
		}
	}

	// Set up server address
	sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(ack_port_);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	// Bind the socket with retry logic for port conflicts
	int bind_attempts = 0;
	const int max_bind_attempts = 10;
	while (bind_attempts < max_bind_attempts) {
		if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == 0) {
			break; // Bind successful
		}
		
		if (errno == EADDRINUSE) {
			// Port in use, try a different port
			bind_attempts++;
			ack_port_ = kAckPortMin + (GenerateRandomNum() % kAckPortRange);
			server_addr.sin_port = htons(ack_port_);
			LOG(WARNING) << "Port " << (ack_port_ - 1) << " in use, trying port " << ack_port_ 
			             << " (attempt " << bind_attempts << "/" << max_bind_attempts << ")";
		} else {
			// Other bind error
			LOG(ERROR) << "Bind failed: " << strerror(errno);
			close(server_sock);
			return;
		}
	}
	
	if (bind_attempts >= max_bind_attempts) {
		LOG(ERROR) << "Failed to bind after " << max_bind_attempts << " attempts";
		close(server_sock);
		return;
	}

	// Start listening
	if (listen(server_sock, SOMAXCONN) < 0) {
		LOG(ERROR) << "Listen failed: " << strerror(errno);
		close(server_sock);
		return;
	}

	// Create epoll instance
	int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		LOG(ERROR) << "Failed to create epoll file descriptor: " << strerror(errno);
		close(server_sock);
		return;
	}

	// Add server socket to epoll
	epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = server_sock;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &event) == -1) {
		LOG(ERROR) << "Failed to add server socket to epoll: " << strerror(errno);
		close(server_sock);
		close(epoll_fd);
		return;
	}

	// Variables for epoll event handling
	const int max_events =  NUM_MAX_BROKERS > 0 ? NUM_MAX_BROKERS * 2 : 64;
	std::vector<epoll_event> events(max_events);
	// [[PERF: 1ms epoll timeout]] - Wake often to process incoming acks; 10ms added latency.
	constexpr int EPOLL_TIMEOUT_MS = 1;
	// [[P2.2]] flat_hash_map for O(1) lookup and better cache locality than std::map.
	absl::flat_hash_map<int, int> client_sockets; // fd -> broker_id

	// Map to track the last received cumulative ACK per socket for calculating increments
	// Initializing with -1 assumes ACK IDs (logical_offset) start >= 0.
	// The first calculation becomes ack - (size_t)-1 which equals ack + 1.
	absl::flat_hash_map<int, size_t> prev_ack_per_sock;

	// Track state for reading initial broker ID
	enum class ConnState { WAITING_FOR_ID, READING_ACKS };
	absl::flat_hash_map<int, ConnState> socket_state;
	absl::flat_hash_map<int, std::pair<int, size_t>> partial_id_reads; // fd -> {partial_id, bytes_read}
	// Buffer partial ACK reads (size_t) so we don't discard bytes when recv returns < 8 bytes
	absl::flat_hash_map<int, std::pair<size_t, size_t>> partial_ack_reads; // fd -> {ack_buffer, bytes_read}

thread_count_.fetch_add(1, std::memory_order_release);  // Signal that epoll loop is ready; Init loads with acquire

// Main epoll loop
while (!shutdown_.load(std::memory_order_relaxed)) {
	int num_events = epoll_wait(epoll_fd, events.data(), max_events, EPOLL_TIMEOUT_MS);

	if (num_events < 0) {
		if (errno == EINTR) {
			continue; // Interrupted, just retry
		}
		LOG(ERROR) << "AckThread: epoll_wait failed: " << strerror(errno);
		break; // Exit loop on unrecoverable error
	}

	for (int i = 0; i < num_events; i++) {
		int current_fd = events[i].data.fd;
		if (current_fd == server_sock) {
			// Handle new connection
			sockaddr_in client_addr;
			socklen_t client_addr_len = sizeof(client_addr);
			int client_sock = accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);

			if (client_sock == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// This can happen with level-triggered accept if already handled? Should be rare.
					VLOG(2) << "AckThread: accept returned EAGAIN/EWOULDBLOCK";
				} else {
					LOG(ERROR) << "AckThread: Accept failed: " << strerror(errno);
				}
				continue;
			}

			// Set client socket to non-blocking mode
			int flags = fcntl(client_sock, F_GETFL, 0);
			if (flags == -1 || fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
				LOG(ERROR) << "Failed to set client socket to non-blocking: " << strerror(errno);
				close(client_sock);
				continue;
			}

			// Add client socket to epoll
			event.events = EPOLLIN | EPOLLET;  // Edge-triggered mode
			event.data.fd = client_sock;

			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &event) == -1) {
				LOG(ERROR) << "Failed to add client socket to epoll: " << strerror(errno);
				close(client_sock);
			} else {
				client_sockets[client_sock] = -1; // Temporarily store fd, broker_id is unknown (-1)
				socket_state[client_sock] = ConnState::WAITING_FOR_ID; // Expect Broker ID first
				partial_id_reads[client_sock] = {0, 0};
				prev_ack_per_sock[client_sock] = (size_t)-1;
				// [[EPOLLET_RACE_FIX]] With EPOLLET, data that arrives *before* EPOLL_CTL_ADD never
				// triggers EPOLLIN. On loopback (B0, same machine) the broker can send broker_id
				// before we register the fd → socket stuck in WAITING_FOR_ID forever → B0=0 ACKs.
				// Drain any data already present by processing this fd immediately (same path as below).
				current_fd = client_sock;
				goto process_client_fd;
			}
		} else {
			// Handle data from existing connection (also reached via goto from EPOLLET race fix)
process_client_fd:;
			int client_sock = current_fd;
			// [[DEFENSIVE]] Stale event for fd we already closed (e.g. EPOLL_CTL_DEL race).
			if (!client_sockets.contains(client_sock)) {
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr);
				continue;
			}
			ConnState current_state = socket_state[client_sock];
			bool connection_error_or_closed = false;

			while (!connection_error_or_closed) {
				if (current_state == ConnState::WAITING_FOR_ID){
					// --- Try to Read Broker ID ---
					int broker_id_buffer;
					auto& partial_read = partial_id_reads[client_sock];
					size_t needed = sizeof(broker_id_buffer) - partial_read.second;
					ssize_t recv_ret = recv(client_sock,
							(char*)&partial_read.first + partial_read.second, // Read into partial buffer
							needed, 0);

					if (recv_ret == 0) { connection_error_or_closed = true; break; }
					if (recv_ret < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) break; // No more data now
						if (errno == EINTR) continue; // Retry read
						LOG(ERROR) << "AckThread: recv error reading broker ID on fd " << client_sock << ": " << strerror(errno);
						connection_error_or_closed = true; break;
					}

					partial_read.second += recv_ret; // Increment bytes read for ID

					if (partial_read.second == sizeof(broker_id_buffer)) {
						// Full ID received
						broker_id_buffer = partial_read.first; // Get the ID
						if (broker_id_buffer < 0 || broker_id_buffer >= (int)broker_stats_.size()) {
							LOG(ERROR) << "AckThread: Received invalid broker_id " << broker_id_buffer << " on fd " << client_sock;
							connection_error_or_closed = true; break; // Invalid ID, close connection
						}
						// [[B1_ACK_ZERO]] INFO so we can see which brokers' ACK connections reached the client
						LOG(INFO) << "AckThread: Received Broker ID " << broker_id_buffer << " from fd=" << client_sock;
						client_sockets[client_sock] = broker_id_buffer; // Update map value
						socket_state[client_sock] = ConnState::READING_ACKS; // Transition state
						current_state = ConnState::READING_ACKS; // Update local state for this loop
						// [[FIX: B3=0 ACKs]] Track that this broker has an ACK connection
						{
							absl::MutexLock lock(&mutex_);
							brokers_with_ack_connection_.insert(broker_id_buffer);
							VLOG(1) << "AckThread: Broker " << broker_id_buffer << " ACK connection tracked. "
							        << "Total ACK connections: " << brokers_with_ack_connection_.size()
							        << " / expected: " << expected_ack_brokers_.load(std::memory_order_relaxed);
						}
						// Clear partial read state for this FD
						partial_id_reads.erase(client_sock);
						partial_ack_reads[client_sock] = {0, 0}; // Init ACK read buffer for this connection
						// Continue reading potential ACK data in the same loop iteration
					}
					// If ID still not complete, loop will try recv() again if more data indicated by epoll
				}else if(current_state == ConnState::READING_ACKS){
					// [[CRITICAL_FIX: Buffer partial ACK reads]] - Don't discard bytes when recv returns < sizeof(size_t).
					// Otherwise we can lose ACK data and ack_received_ never reaches client_order_ (test hangs).
					auto& partial = partial_ack_reads[client_sock];
					size_t needed = sizeof(size_t) - partial.second;
					ssize_t recv_ret = recv(client_sock,
							reinterpret_cast<char*>(&partial.first) + partial.second,
							needed, 0);
					if (recv_ret == 0) { connection_error_or_closed = true; break; }
					if (recv_ret < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) break; // No more data now
						if (errno == EINTR) continue; // Retry read
						LOG(ERROR) << "AckThread: recv error reading ACK bytes on fd " << client_sock << ": " << strerror(errno);
						connection_error_or_closed = true; break;
					}
					partial.second += static_cast<size_t>(recv_ret);
					if (partial.second != sizeof(size_t)) {
						// Partial ACK still in progress, wait for more data
						break;
					}

					// --- Process Full ACK Value ---
					size_t acked_msg = partial.first;
					partial_ack_reads[client_sock] = {0, 0}; // Reset for next ACK
					int broker_id = client_sockets[client_sock]; // Get broker ID

					// [[CRITICAL FIX: Validate broker_id bounds]] Check for FD reuse corruption
					if (broker_id < 0 || broker_id >= (int)broker_stats_.size()) {
						LOG(ERROR) << "AckThread: Invalid broker_id=" << broker_id << " for fd=" << client_sock
						           << " (FD reuse or corruption). Closing connection.";
						connection_error_or_closed = true; break;
					}

					size_t prev_acked = prev_ack_per_sock[client_sock]; // Assumes key exists

					if (acked_msg >= prev_acked || prev_acked == (size_t)-1) { // Check for valid cumulative value
						// [[CRITICAL_FIX: Handle first ACK correctly to avoid unsigned underflow]]
						// If prev_acked == (size_t)-1, this is the first ACK from this broker
						// Direct subtraction would underflow: acked_msg - (size_t)-1 = huge number
						// We must handle first ACK specially: new_acked_msgs = acked_msg (not acked_msg - (-1))
						size_t new_acked_msgs;
						if (prev_acked == (size_t)-1) {
							// First ACK from this broker - use value directly (no previous to subtract)
							new_acked_msgs = acked_msg;
						} else {
							// Subsequent ACK - calculate increment from previous
							new_acked_msgs = acked_msg - prev_acked;
						}
						if (new_acked_msgs > 0) {
							// [[DIAGNOSTIC: Log ACK processing]]
							static thread_local size_t ack_process_count = 0;
							size_t prev_total = ack_received_.load(std::memory_order_relaxed);
							if (++ack_process_count % 100 == 0 || prev_total % 10000 == 0) {
								VLOG(2) << "EpollAckThread: B" << broker_id << " acked_msg=" << acked_msg 
								        << " prev_acked=" << (prev_acked == (size_t)-1 ? -1 : (int64_t)prev_acked)
								        << " new_acked=" << new_acked_msgs 
								        << " total_ack_received=" << (prev_total + new_acked_msgs)
								        << " (count=" << ack_process_count << ")";
							}
							
#ifdef COLLECT_LATENCY_STATS
								ProcessPublishAckLatency(broker_id, acked_msg);
#endif
							broker_stats_[broker_id].acked_messages += new_acked_msgs;  // Not atomic - single writer
							ack_received_.fetch_add(new_acked_msgs, std::memory_order_release);
							prev_ack_per_sock[client_sock] = acked_msg; // Update last value for this socket
							VLOG(4) << "AckThread: fd=" << client_sock << " (Broker " << broker_id << ") ACK messages: " 
								<< acked_msg << " (+" << new_acked_msgs << ")";
						} else {
							// Duplicate cumulative value, ignore.
							VLOG(5) << "AckThread: fd=" << client_sock << " (Broker " << broker_id << 
								") Duplicate ACK messages received: " << acked_msg;
						}
					} else {
						LOG(WARNING) << "AckThread: Received non-monotonic ACK bytes on fd " << client_sock
							<< " (Broker " << broker_id << "). Received: " << acked_msg << ", Previous: " << prev_acked;
					}
					// Continue loop to read potentially more data from this socket event
				}else{
					LOG(ERROR) << "AckThread: Invalid state for fd " << client_sock;
					connection_error_or_closed = true; 
					break;
				}
			} // End outer `while (!connection_error_or_closed)` loop for EPOLLET
			if(connection_error_or_closed){
				VLOG(3) << "AckThread: Cleaning up connection fd=" << client_sock;
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, nullptr); // Ignore error
				close(client_sock);
				client_sockets.erase(client_sock);
				prev_ack_per_sock.erase(client_sock);
				socket_state.erase(client_sock);
				partial_id_reads.erase(client_sock); // Clean up partial ID state too
				partial_ack_reads.erase(client_sock); // Clean up partial ACK state too
			}
		}//end else (handle data from existing connection)
	}// End for loop through epoll events
}// End while(!shutdown_)

// [[CRITICAL FIX: Clean up all ACK state]] Prevent FD reuse corruption in future runs
for (auto const& [sock_fd, broker_id] : client_sockets) {
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sock_fd, nullptr);
	close(sock_fd);
}
client_sockets.clear();
prev_ack_per_sock.clear();
socket_state.clear();
partial_id_reads.clear();
partial_ack_reads.clear();

// Clean up epoll and server socket
close(epoll_fd);
close(server_sock);
}

void Publisher::PublishThread(int broker_id, int pubQuesIdx) {
	ScopedFd sock, efd;  // [[Phase 2.2]] RAII: closed when thread returns or on reassignment
	int slow_consumer_queue = -1;
	int slow_consumer_ms = 1;
	size_t sent_msgs = 0;
	size_t sent_batches = 0;
	const bool queue_diag = QueueDiagEnabled();
	uint64_t local_empty_reads = 0;
	uint64_t local_empty_wait_ns = 0;
	uint64_t local_empty_wait_ns_max = 0;
	uint64_t local_empty_yields = 0;
	uint64_t local_batches = 0;
	uint64_t local_messages = 0;
	uint64_t local_send_eagain_events = 0;
	uint64_t local_send_eagain_wait_ns = 0;
	uint64_t local_send_eagain_wait_ns_max = 0;
	auto queue_diag_cleanup = absl::MakeCleanup([&]() {
		if (!queue_diag) {
			return;
		}
		diag_consumer_empty_reads_.fetch_add(local_empty_reads, std::memory_order_relaxed);
		diag_consumer_wait_ns_.fetch_add(local_empty_wait_ns, std::memory_order_relaxed);
		UpdateMax(diag_consumer_wait_ns_max_, local_empty_wait_ns_max);
		diag_consumer_yields_.fetch_add(local_empty_yields, std::memory_order_relaxed);
		diag_consumer_batches_.fetch_add(local_batches, std::memory_order_relaxed);
		diag_consumer_messages_.fetch_add(local_messages, std::memory_order_relaxed);
		diag_send_eagain_events_.fetch_add(local_send_eagain_events, std::memory_order_relaxed);
		diag_send_eagain_wait_ns_.fetch_add(local_send_eagain_wait_ns, std::memory_order_relaxed);
		UpdateMax(diag_send_eagain_wait_ns_max_, local_send_eagain_wait_ns_max);
	});
	{
		const char* env = std::getenv("EMBARCADERO_SLOW_CONSUMER_QUEUE");
		if (env && *env) {
			slow_consumer_queue = std::atoi(env);
			const char* ms_env = std::getenv("EMBARCADERO_SLOW_CONSUMER_MS");
			if (ms_env && *ms_env) slow_consumer_ms = std::max(1, std::atoi(ms_env));
			if (slow_consumer_queue >= 0 && static_cast<int>(pubQuesIdx) == slow_consumer_queue) {
				LOG(INFO) << "PublishThread[" << pubQuesIdx << "]: slow consumer injection active ("
				          << slow_consumer_ms << "ms per batch)";
			}
		}
	}

	// Lambda function to establish connection to a broker
	auto connect_to_server = [&](size_t brokerId) -> bool {
		// Reassigning sock/efd closes previous fds via ScopedFd move assignment

		// Get broker address
		std::string addr;
		size_t num_brokers;
		{
			absl::MutexLock lock(&mutex_);
			auto it = nodes_.find(brokerId);
			if (it == nodes_.end()) {
				LOG(ERROR) << "Broker ID " << brokerId << " not found in nodes map";
				return false;
			}

			try {
				auto [_addr, _port] = ParseAddressPort(it->second);
				addr = _addr;
			} catch (const std::exception& e) {
				LOG(ERROR) << "Failed to parse address for broker " << brokerId 
				           << ": " << it->second << " - " << e.what();
				return false;
			}
			num_brokers = nodes_.size();
		}

		// Create socket
		VLOG(1) << "PublishThread: Connecting to broker " << brokerId << " at " << addr << ":" << (PORT + brokerId);
		sock = ScopedFd(GetNonblockingSock(const_cast<char*>(addr.c_str()), PORT + brokerId));
		if (sock.get() < 0) {
			LOG(ERROR) << "PublishThread: Failed to create socket to broker " << brokerId << " at " << addr << ":" << (PORT + brokerId);
			return false;
		}

		// Create epoll instance
		efd = ScopedFd(epoll_create1(0));
		if (efd.get() < 0) {
			LOG(ERROR) << "epoll_create1 failed: " << strerror(errno);
			sock = ScopedFd();
			return false;
		}

		// Register socket with epoll
		struct epoll_event event;
		event.data.fd = sock.get();
		event.events = EPOLLOUT;
		if (epoll_ctl(efd.get(), EPOLL_CTL_ADD, sock.get(), &event) != 0) {
			LOG(ERROR) << "epoll_ctl failed: " << strerror(errno);
			sock = ScopedFd();
			efd = ScopedFd();
			return false;
		}

		// Prepare handshake message
		Embarcadero::EmbarcaderoReq shake;
		shake.client_req = Embarcadero::Publish;
		shake.client_id = client_id_;
		memset(shake.topic, 0, sizeof(shake.topic));
		memcpy(shake.topic, topic_, std::min<size_t>(TOPIC_NAME_SIZE - 1, sizeof(shake.topic) - 1));
		shake.ack = ack_level_;
		shake.port = ack_port_;
		shake.num_msg = num_brokers;  // Using num_msg field to indicate number of brokers

		// Send handshake with epoll for non-blocking
		// [[FIX: Throughput]] Increased event array, reduced timeout for high-throughput
		struct epoll_event events[64];
		bool running = true;
		size_t sent_bytes = 0;

		while (!shutdown_.load(std::memory_order_relaxed) && running) {
			// [[FIX: Throughput]] 1ms timeout instead of 1000ms for fast response
			int n = epoll_wait(efd.get(), events, 64, 1);
			if (n == 0) {
				// Timeout - check if we should continue
				if (shutdown_.load(std::memory_order_relaxed) || publish_finished_.load(std::memory_order_relaxed)) {
				// PublishThread: Handshake interrupted by shutdown
				break;
				}
				continue;
			}
			if (n < 0) {
				if (errno == EINTR) continue;
				LOG(ERROR) << "PublishThread: epoll_wait failed during handshake: " << strerror(errno);
				break;
			}
			for (int i = 0; i < n; i++) {
				if (events[i].events & EPOLLOUT) {
					ssize_t bytesSent = send(sock.get(), 
							reinterpret_cast<int8_t*>(&shake) + sent_bytes, 
							sizeof(shake) - sent_bytes, 
							0);

					if (bytesSent <= 0) {
						if (errno != EAGAIN && errno != EWOULDBLOCK) {
							LOG(ERROR) << "Handshake send failed: " << strerror(errno);
							running = false;
							sock = ScopedFd();
							efd = ScopedFd();
							return false;
						}
						// EAGAIN/EWOULDBLOCK are expected in non-blocking mode
					} else {
						sent_bytes += bytesSent;
						if (sent_bytes == sizeof(shake)) {
							LOG(INFO) << "PublishThread: Handshake sent successfully to broker " << brokerId 
							         << " (client_id=" << client_id_ << ", topic=" << topic_ << ")";
							running = false;
							break;
						}
					}
				}
			}
		}

		if (sent_bytes != sizeof(shake)) {
			LOG(ERROR) << "PublishThread: Handshake incomplete - sent " << sent_bytes 
			          << " of " << sizeof(shake) << " bytes to broker " << brokerId;
			return false;
		}

		return true;
	};

	// Connect to initial broker
	VLOG(1) << "PublishThread[" << pubQuesIdx << "]: Starting connection to broker " << broker_id;
	if (!connect_to_server(broker_id)) {
		LOG(ERROR) << "PublishThread[" << pubQuesIdx << "]: Failed to connect to broker " << broker_id;
		return;
	}
	VLOG(1) << "PublishThread[" << pubQuesIdx << "]: Successfully connected to broker " << broker_id;

	// Signal thread is initialized
	thread_count_.fetch_add(1);
	VLOG(1) << "PublishThread[" << pubQuesIdx << "]: Thread initialized, thread_count=" << thread_count_.load();

	// Track if we've sent at least one batch (to ensure connection is used)
	bool has_sent_batch = false;
	size_t consecutive_empty_reads = 0;

	// Main publishing loop. [[CRITICAL: DRAIN_BEFORE_EXIT]] Do NOT break at loop top on consumer_should_exit_.
	// Doing so would exit without draining the queue, leaving batches unsent and causing ACK timeout (~0.03% shortfall).
	// Only exit when we get nullptr from Read() AND consumer_should_exit_ is set, after draining any remaining batches.
	while (true) {
		size_t len;
		int bytesSent = 0;

		// Read a batch from the queue (QueueBuffer)
		Embarcadero::BatchHeader* batch_header =
			static_cast<Embarcadero::BatchHeader*>(pubQue_.Read(pubQuesIdx));

		// No batch available: exit only if shutdown requested and queue is drained.
		if (batch_header == nullptr || batch_header->total_size == 0) {
			if (consumer_should_exit_.load(std::memory_order_relaxed)) {
				// CRITICAL: Don't exit immediately if we haven't sent any batches yet
				// This ensures the connection stays alive even if this thread got no batches
				// NetworkManager expects to receive at least one batch header per connection
				if (!has_sent_batch) {
					// [[FIX: B3=0 ACKs]] CRITICAL diagnostic - this thread got NO batches!
					// This is the likely cause of B*=0 ACK issues
					LOG(ERROR) << "PublishThread[" << pubQuesIdx << "] for BROKER " << broker_id
					          << ": EXITING WITH ZERO BATCHES SENT! "
					          << "This thread read from buffer " << pubQuesIdx << " but got no data. "
					          << "publish_finished=" << publish_finished_.load(std::memory_order_relaxed)
					          << ", shutdown=" << shutdown_.load(std::memory_order_relaxed);
					// Wait a bit to see if batches arrive, then exit gracefully
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
				// Drain remaining batches before exit.
				while ((batch_header = static_cast<Embarcadero::BatchHeader*>(pubQue_.Read(pubQuesIdx))) != nullptr
				       && batch_header->total_size != 0) {
					has_sent_batch = true;
					goto process_batch;
				}
				// PublishThread exiting
				VLOG(1) << "PublishThread[" << pubQuesIdx << "]: Exiting - broker=" << broker_id
				        << ", publish_finished=" << publish_finished_.load(std::memory_order_relaxed)
				        << ", shutdown=" << shutdown_.load(std::memory_order_relaxed)
				        << ", has_sent_batch=" << has_sent_batch;
				break;
			} else {
				// [[PERF]] spin 128x before yield when waiting for batch.
				uint64_t wait_start_ns = 0;
				if (queue_diag) {
					local_empty_reads++;
					wait_start_ns = NowNs();
				}
				static constexpr int kConsumerSpinCount = 128;
				for (int s = 0; s < kConsumerSpinCount; s++) {
					Embarcadero::CXL::cpu_pause();
				}
				consecutive_empty_reads++;
				bool yielded = false;
				// Reduce scheduler churn when producer is far behind: after sustained empties,
				// sleep briefly instead of yielding every poll loop.
				if (consecutive_empty_reads >= 2048) {
					std::this_thread::sleep_for(std::chrono::microseconds(2));
				} else {
					std::this_thread::yield();
					yielded = true;
				}
				if (queue_diag) {
					if (yielded) {
						local_empty_yields++;
					}
					uint64_t waited = NowNs() - wait_start_ns;
					local_empty_wait_ns += waited;
					if (waited > local_empty_wait_ns_max) {
						local_empty_wait_ns_max = waited;
					}
				}
				continue;
			}
		}

	process_batch:
		consecutive_empty_reads = 0;
		static thread_local size_t batch_count = 0;
		++batch_count;
		if (queue_diag) {
			local_batches++;
			local_messages += batch_header->num_msg;
		}
		
		if (enable_batch_attempted_for_timeout_log_) {
			total_batches_attempted_.fetch_add(1, std::memory_order_relaxed);
		}

		batch_header->client_id = client_id_;
		batch_header->broker_id = broker_id;

		// Get pointer to message data
		void* msg = reinterpret_cast<uint8_t*>(batch_header) + sizeof(Embarcadero::BatchHeader);
		len = batch_header->total_size;

		// Function to send batch header
		auto send_batch_header = [&]() -> void {
			// Handle sequencer-specific batch header processing
			if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
				batch_header->broker_id = broker_id;
				corfu_client_->GetTotalOrder(batch_header);

			VLOG(2) << "Publisher: Got total_order=" << batch_header->total_order
			        << " for batch with " << batch_header->num_msg << " messages";

				// Update total order for each message in the batch
				Embarcadero::MessageHeader* header = static_cast<Embarcadero::MessageHeader*>(msg);
				size_t total_order = batch_header->total_order;

				for (size_t i = 0; i < batch_header->num_msg; i++) {
					header->total_order = total_order++;
					// Move to next message
					header = reinterpret_cast<Embarcadero::MessageHeader*>(
							reinterpret_cast<uint8_t*>(header) + header->paddedSize);
				}
			}

			// Send batch header with retry logic
			size_t total_sent = 0;
			const size_t header_size = sizeof(Embarcadero::BatchHeader);

			while (total_sent < header_size) {
				bytesSent = send(sock.get(), 
						reinterpret_cast<uint8_t*>(batch_header) + total_sent, 
						header_size - total_sent, 
						0);

				if (bytesSent < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
						// Wait for socket to become writable so broker can recv() and drain kernel buffer.
						// [[ROOT_CAUSE_FIX]] 0ms caused busy-loop; brokers blocked in recv(), ACK stall.
						// Use 1ms so we yield and broker gets CPU; epoll returns when EPOLLOUT (writable).
						static constexpr int EPOLL_WAIT_WRITABLE_MS = 1;
						struct epoll_event events[64];
						int n = epoll_wait(efd.get(), events, 64, EPOLL_WAIT_WRITABLE_MS);

						if (n == -1) {
							LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
							throw std::runtime_error("epoll_wait failed");
						}
					} else {
						// Fatal error
						LOG(ERROR) << "Failed to send batch header: " << strerror(errno);
						throw std::runtime_error("send failed");
					}
				} else {
					total_sent += bytesSent;
				}
			}
		};

		// Try to send batch header, handle failures
		try {
			send_batch_header();
			if (batch_count % 100 == 0 || batch_count == 1) {
				VLOG(2) << "PublishThread[" << pubQuesIdx << "]: Sent batch header for batch " 
				        << batch_count << " to broker " << broker_id;
			}
		} catch (const std::exception& e) {
			total_batches_failed_.fetch_add(1, std::memory_order_relaxed);
			pubQue_.ReleaseBatch(batch_header);
			LOG(ERROR) << "Exception sending batch header: " << e.what();
			std::string fail_msg = "Header Send Fail Broker " + std::to_string(broker_id) + " (" + e.what() + ")";
			RecordFailureEvent(fail_msg); // Record event

			// Handle broker failure by finding another broker
			int new_broker_id;
			{
				absl::MutexLock lock(&mutex_);

				// Remove the failed broker
				auto it = std::find(brokers_.begin(), brokers_.end(), broker_id);
				if (it != brokers_.end()) {
					brokers_.erase(it);
					nodes_.erase(broker_id);
				}

				// No brokers left
				if (brokers_.empty()) {
					pubQue_.ReleaseBatch(batch_header);
					LOG(ERROR) << "No brokers available, thread exiting";
					return;
				}

				// Select replacement broker
				new_broker_id = brokers_[(pubQuesIdx % num_threads_per_broker_) % brokers_.size()];
			}

			// Connect to new broker
			if (!connect_to_server(new_broker_id)) {
				pubQue_.ReleaseBatch(batch_header);
				RecordFailureEvent("Reconnect Fail Broker " + std::to_string(new_broker_id));
				LOG(ERROR) << "Failed to connect to replacement broker " << new_broker_id;
				return;
			}

			std::string reconn_msg = "Reconnect Success Broker " + std::to_string(new_broker_id) + " (from " + std::to_string(broker_id) + ")";
			RecordFailureEvent(reconn_msg);

			try {
				send_batch_header();
			} catch (const std::exception& e) {
				total_batches_failed_.fetch_add(1, std::memory_order_relaxed);
				pubQue_.ReleaseBatch(batch_header);
				LOG(ERROR) << "Failed to send batch header to replacement broker: " << e.what();
				std::string fail_msg2 = "Header Send Fail (Post-Reconnect) Broker " + std::to_string(new_broker_id) + " (" + e.what() + ")";
				RecordFailureEvent(fail_msg2);
				return;
			}

			// Thread redirected from broker to new broker after failure

			broker_id = new_broker_id;
		}

		// Send message data
		size_t sent_bytes = 0;
		size_t zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;

		// CRITICAL: Ensure all batch data is sent before checking publish_finished_
		// This prevents premature thread exit while data is still in flight
		while (sent_bytes < len) {
			// Check for shutdown but don't exit mid-send - finish sending current batch
			if (shutdown_.load(std::memory_order_relaxed) && !publish_finished_.load(std::memory_order_relaxed)) {
				LOG(WARNING) << "PublishThread[" << pubQuesIdx << "]: Shutdown requested but batch not fully sent ("
				           << sent_bytes << " of " << len << " bytes). Completing send...";
			}
			size_t remaining_bytes = len - sent_bytes;
			size_t to_send = std::min(remaining_bytes, zero_copy_send_limit);

		// Use MSG_ZEROCOPY for large sends to reduce CPU overhead.
		int send_flags = 0;

			if (to_send >= (1UL << 16)) {
				send_flags = MSG_ZEROCOPY;
			} else {
				send_flags = 0;
			}
			bytesSent = send(sock.get(), 
					static_cast<uint8_t*>(msg) + sent_bytes, 
					to_send, 
					send_flags);

			if (bytesSent > 0) {
				// Update statistics
				broker_stats_[broker_id].sent_bytes.fetch_add(bytesSent, std::memory_order_relaxed);
				total_sent_bytes_.fetch_add(bytesSent, std::memory_order_relaxed);
				sent_bytes += bytesSent;

				// Reset backoff after successful send
				zero_copy_send_limit = ZERO_COPY_SEND_LIMIT;
			} else if (bytesSent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
				// Socket buffer full; wait for writable so broker can recv() and drain.
				// [[ROOT_CAUSE_FIX]] 0ms caused client busy-loop while brokers blocked in recv() → ACK stall.
				// Default 1ms yields to broker. Env EMBARCADERO_EPOLL_WAIT_WRITABLE_MS=0 to test busy poll.
				uint64_t eagain_wait_start_ns = 0;
				if (queue_diag) {
					local_send_eagain_events++;
					eagain_wait_start_ns = NowNs();
				}
				int wait_ms = GetEpollWaitWritableMs();
				struct epoll_event events[64];
				int n = epoll_wait(efd.get(), events, 64, wait_ms);
				if (queue_diag) {
					uint64_t waited = NowNs() - eagain_wait_start_ns;
					local_send_eagain_wait_ns += waited;
					if (waited > local_send_eagain_wait_ns_max) {
						local_send_eagain_wait_ns_max = waited;
					}
				}

				if (n == -1) {
					LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
					pubQue_.ReleaseBatch(batch_header);
					break;
				}

				// OPTIMIZATION: Less aggressive backoff (25%) to maintain higher throughput (old behavior)
				zero_copy_send_limit = std::max(zero_copy_send_limit * 3 / 4, 1UL << 16); // Reduce by 25%, min 64KB
			} else if (bytesSent < 0) {
				// Connection failure, switch to a different broker
				LOG(WARNING) << "Send failed to broker " << broker_id << ": " << strerror(errno);
				std::string fail_msg = "Data Send Fail Broker " + std::to_string(broker_id) + " errno=" + std::to_string(errno);
				RecordFailureEvent(fail_msg);

				int new_broker_id;
				{
					absl::MutexLock lock(&mutex_);

					// Remove the failed broker
					auto it = std::find(brokers_.begin(), brokers_.end(), broker_id);
					if (it != brokers_.end()) {
						brokers_.erase(it);
						nodes_.erase(broker_id);
					}

					// No brokers left
					if (brokers_.empty()) {
						pubQue_.ReleaseBatch(batch_header);
						LOG(ERROR) << "No brokers available, thread exiting";
						return;
					}

					// Select replacement broker
					new_broker_id = brokers_[(pubQuesIdx % num_threads_per_broker_) % brokers_.size()];
				}

				// Connect to new broker
				if (!connect_to_server(new_broker_id)) {
					pubQue_.ReleaseBatch(batch_header);
					RecordFailureEvent("Reconnect Fail Broker " + std::to_string(new_broker_id));
					LOG(ERROR) << "Failed to connect to replacement broker " << new_broker_id;
					return;
				}

				std::string reconn_msg = "Reconnect Success Broker " + std::to_string(new_broker_id) + " (from " + std::to_string(broker_id) + ")";
				RecordFailureEvent(reconn_msg);
				// Reset and try again with new broker
				try {
					send_batch_header();
				} catch (const std::exception& e) {
					pubQue_.ReleaseBatch(batch_header);
					LOG(ERROR) << "Failed to send batch header to replacement broker: " << e.what();
					RecordFailureEvent("Header Send Fail (Post-Reconnect) Broker " + std::to_string(new_broker_id) + " (" + e.what() + ")");
					return;
				}

				// Thread redirected from broker to new broker after failure

				broker_id = new_broker_id;
				sent_bytes = 0;
			}
		}

		// Mark that we've sent at least one batch
		has_sent_batch = true;
		total_batches_sent_.fetch_add(1, std::memory_order_relaxed);
		total_messages_sent_.fetch_add(batch_header->num_msg, std::memory_order_relaxed);
		size_t prev_sent = broker_stats_[broker_id].sent_messages.fetch_add(
			batch_header->num_msg, std::memory_order_relaxed);
		size_t end_count = prev_sent + batch_header->num_msg;
#ifdef COLLECT_LATENCY_STATS
		RecordPublishSend(broker_id, end_count);
#else
		(void)end_count;
#endif
		sent_batches += 1;
		sent_msgs += batch_header->num_msg;

		

		// Verify all data was sent
		if (sent_bytes != len) {
			LOG(ERROR) << "PublishThread[" << pubQuesIdx << "]: Batch send incomplete! Sent " 
			          << sent_bytes << " of " << len << " bytes for batch " << batch_count 
			          << " to broker " << broker_id;
		}

		// Return batch to pool (QueueBuffer).
		pubQue_.ReleaseBatch(batch_header);

		// Controlled experiment: simulate slow consumer (one queue blocks entire producer).
		if (slow_consumer_queue >= 0 && static_cast<int>(pubQuesIdx) == slow_consumer_queue) {
			std::this_thread::sleep_for(std::chrono::milliseconds(slow_consumer_ms));
		}
	}

	// IMPROVED: Keep connections alive for subscriber
	// Don't close data connections when publisher finishes - this would cause brokers to shutdown
	// The connections will be cleaned up when the Publisher object is destroyed
	// 
	// NOTE: We intentionally do NOT close sock and efd here to keep broker connections alive
	// This allows the subscriber to continue working after publisher finishes
	// Resources will be cleaned up in the Publisher destructor
	VLOG(1) << "PublishThread[" << pubQuesIdx << "]: Exiting main loop. Socket " << sock.get()
	        << " kept open for ACKs. publish_finished=" << publish_finished_.load(std::memory_order_relaxed)
	        << ", shutdown=" << shutdown_.load(std::memory_order_relaxed);
}

void Publisher::SubscribeToClusterStatus() {
	heartbeat_system::ClusterStatus cluster_status;
	read_fail_count_ = 0;

	while (!shutdown_.load(std::memory_order_relaxed)) {
		heartbeat_system::ClientInfo client_info;
		{
			absl::MutexLock lock(&mutex_);
			for (const auto& it : nodes_) {
				client_info.add_nodes_info(it.first);
			}
		}

		LOG(INFO) << "SubscribeToCluster: Creating gRPC reader for cluster status subscription...";
		// Use a fresh ClientContext per reader; gRPC forbids reusing a context for a new call.
		grpc::ClientContext ctx;
		subscribe_context_.store(&ctx);
		std::unique_ptr<grpc::ClientReader<ClusterStatus>> reader(
				stub_->SubscribeToCluster(&ctx, client_info));

		if (!reader) {
			LOG(ERROR) << "SubscribeToCluster: Failed to create gRPC reader. Check broker gRPC service availability.";
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}

		VLOG(1) << "SubscribeToCluster: gRPC reader created successfully, waiting for cluster status...";

		// Inner loop: process reads until Read() fails or shutdown
		while (!shutdown_.load(std::memory_order_relaxed)) {
			if (reader->Read(&cluster_status)) {
			// Use broker_info if available (includes accepts_publishes)
			// Fall back to new_nodes for backward compatibility with older brokers
			bool use_broker_info = cluster_status.broker_info_size() > 0;
			LOG(INFO) << "SubscribeToCluster: Received cluster status update with "
			         << (use_broker_info ? cluster_status.broker_info_size() : cluster_status.new_nodes_size())
			         << " brokers (using " << (use_broker_info ? "broker_info" : "new_nodes") << ")";

			// [[DIAGNOSTIC: Log each broker's accepts_publishes status]]
			if (use_broker_info) {
				for (const auto& bi : cluster_status.broker_info()) {
					VLOG(1) << "  Broker " << bi.broker_id() << ": accepts_publishes=" << bi.accepts_publishes();
				}
			}

			if (use_broker_info) {
				// Treat broker_info as authoritative: set brokers_ and nodes_ from brokers with accepts_publishes=true
				absl::MutexLock lock(&mutex_);

				brokers_.clear();
				for (const auto& bi : cluster_status.broker_info()) {
					int broker_id = bi.broker_id();
					if (bi.accepts_publishes()) {
						nodes_[broker_id] = bi.network_mgr_addr();
						brokers_.emplace_back(broker_id);
						LOG(INFO) << "SubscribeToCluster: Added broker " << broker_id
						         << " (accepts_publishes=true)";
					} else {
						VLOG(1) << "SubscribeToCluster: Skipping broker " << broker_id
						         << " (accepts_publishes=false)";
					}
				}
				std::sort(brokers_.begin(), brokers_.end());

				int publishable_brokers = static_cast<int>(brokers_.size());
				if (!connected_.load(std::memory_order_acquire) && publishable_brokers > 0) {
					queueSize_ /= publishable_brokers;
				}
			} else if (!cluster_status.new_nodes().empty()) {
				// Backward compatibility: use new_nodes if broker_info not available
				const auto& new_nodes = cluster_status.new_nodes();
				absl::MutexLock lock(&mutex_);

				// Adjust queue size based on number of brokers on first connection
				if (!connected_.load(std::memory_order_acquire)) {
					int num_brokers = 1 + new_nodes.size();
					queueSize_ /= num_brokers;
				}

				// Add new brokers
				for (const auto& addr : new_nodes) {
					int broker_id = GetBrokerId(addr);
					nodes_[broker_id] = addr;
					brokers_.emplace_back(broker_id);
				}

				// Sort brokers for deterministic round-robin assignment
				std::sort(brokers_.begin(), brokers_.end());
			}

			// [[FIX: B2=0 ACKs]] Add publisher threads for brokers that don't have them yet
			// This handles both initial connection AND late-registering brokers
			{
				size_t qsize;
				std::vector<int> brokers_needing_threads;
				{
					absl::MutexLock lock(&mutex_);
					qsize = queueSize_;
					for (int broker_id : brokers_) {
						if (brokers_with_threads_.find(broker_id) == brokers_with_threads_.end()) {
							brokers_needing_threads.push_back(broker_id);
						}
					}
				}

				if (!brokers_needing_threads.empty()) {
					VLOG(1) << "SubscribeToCluster: Adding publisher threads for "
					         << brokers_needing_threads.size() << " broker(s)";
					// [[FIX: B3=0 ACKs]] Set expected ACK brokers count for tracking
					expected_ack_brokers_.store(static_cast<int>(brokers_needing_threads.size()), std::memory_order_release);
					bool all_connected = true;
					for (int broker_id : brokers_needing_threads) {
						LOG(INFO) << "SubscribeToCluster: Adding publisher threads for broker " << broker_id;
						if (!AddPublisherThreads(num_threads_per_broker_, broker_id, qsize)) {
							LOG(ERROR) << "Failed to add publisher threads for broker " << broker_id;
							all_connected = false;
							break;
						}
						// Track that this broker now has threads
						{
							absl::MutexLock lock(&mutex_);
							brokers_with_threads_.insert(broker_id);
						}
					}

					// Signal that we're connected (only on first successful connection)
					if (all_connected && !connected_.load(std::memory_order_acquire)) {
						connected_.store(true, std::memory_order_release);
						LOG(INFO) << "SubscribeToCluster: Connection established successfully. connected_=true";
					}
				} else if (!connected_.load(std::memory_order_acquire) && brokers_.empty()) {
					// Fallback: no publishable brokers discovered, use head broker (0)
					LOG(WARNING) << "SubscribeToCluster: No publishable brokers discovered, using head broker (0) as fallback";
					if (!AddPublisherThreads(num_threads_per_broker_, 0, qsize)) {
						LOG(ERROR) << "Failed to add publisher threads for head broker";
					} else {
						absl::MutexLock lock(&mutex_);
						brokers_.push_back(0);
						brokers_with_threads_.insert(0);
						connected_.store(true, std::memory_order_release);
						VLOG(1) << "SubscribeToCluster: Connection established with fallback broker 0";
					}
				}
			}
			} else {
				// [[Issue 4]] Read failed – break inner loop, Finish(), then outer loop re-establishes reader
				auto now = std::chrono::steady_clock::now();
				if (read_fail_count_ == 0) last_read_warning_ = now;
				read_fail_count_++;
				if (now - last_read_warning_ > std::chrono::seconds(5)) {
					LOG(WARNING) << "SubscribeToCluster: reader->Read() returned false. Failure count: " << read_fail_count_
					            << ". Re-establishing gRPC reader.";
					if (!connected_.load(std::memory_order_acquire)) {
						LOG(ERROR) << "SubscribeToCluster: Initial connection not established after " << read_fail_count_ << " read attempts.";
					}
					last_read_warning_ = now;
				}
				break;  // Exit inner loop → Finish() → outer loop creates new reader
			}
		}

		grpc::Status status = reader->Finish();
		subscribe_context_.store(nullptr);  // Done with this context; next iteration uses a new one.
		if (!status.ok() && !shutdown_) {
			LOG(ERROR) << "SubscribeToCluster stream ended: " << status.error_message() << ". Re-establishing.";
		}
		if (shutdown_) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Back off before re-connect
	}
}

bool Publisher::AddPublisherThreads(size_t num_threads, int broker_id, size_t queue_size) {
	// Use queue_size parameter (caller reads under mutex)
	if (!pubQue_.AddBuffers(queue_size)) {
		LOG(ERROR) << "Failed to add buffers for broker " << broker_id;
		return false;
	}

	// Create threads with cleanup on partial failure
	size_t created = 0;
	try {
		for (size_t i = 0; i < num_threads; i++) {
			int thread_idx = num_threads_.fetch_add(1);
			threads_.emplace_back(&Publisher::PublishThread, this, broker_id, thread_idx);
			created++;
		}
		// So producer round-robins only over queues that have consumers (no ghost queues).
		pubQue_.SetActiveQueues(static_cast<size_t>(num_threads_.load(std::memory_order_relaxed)));
	} catch (const std::exception& e) {
		LOG(ERROR) << "AddPublisherThreads: failed after " << created << " threads: " << e.what();
		// Rollback: join created threads and revert num_threads_
		for (size_t j = 0; j < created; j++) {
			if (threads_.back().joinable()) threads_.back().join();
			threads_.pop_back();
			num_threads_.fetch_sub(1);
		}
		return false;
	}
	return true;
}
