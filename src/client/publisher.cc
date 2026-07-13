
#include "common/env_flags.h"
#include "common/stage_trace.h"
#include <array>
#include <iomanip>
#include "publisher.h"
#include "publisher_profile.h"
#include "latency_stats.h"
#include "common/config.h"
#include "common/order_level.h"
#include "common/scoped_fd.h"
#include "session.pb.h"
#include "absl/container/flat_hash_map.h"
#include <cstring>
#include <random>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <limits>
#include <mutex>
#include <netdb.h>
#include <numeric>
#include <set>
#include <stdexcept>

namespace {
constexpr uint32_t kSessionControlMagic = 0x53455346U;  // "SESF"
constexpr uint32_t kMaxSessionControlPayload = 64 * 1024;

struct SessionControlHeader {
	uint32_t magic;
	uint32_t length;
};

uint32_t ReadSessionEpochOverride() {
	const char* env = std::getenv("EMBARCADERO_SESSION_EPOCH");
	if (!env || env[0] == '\0') return 0;
	char* end = nullptr;
	unsigned long parsed = std::strtoul(env, &end, 10);
	if (end == env || (end && *end != '\0')) return 0;
	return static_cast<uint32_t>(parsed);
}

bool SendAllBlocking(int fd, const void* data, size_t len) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (n == 0) return false;
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool RecvAllBlocking(int fd, void* data, size_t len) {
	uint8_t* p = static_cast<uint8_t*>(data);
	size_t got = 0;
	while (got < len) {
		ssize_t n = recv(fd, p + got, len - got, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (n == 0) return false;
		got += static_cast<size_t>(n);
	}
	return true;
}

void SetSocketBlockingTemporarily(int fd, bool blocking, int* old_flags) {
	if (old_flags == nullptr) return;
	if (*old_flags < 0) *old_flags = fcntl(fd, F_GETFL, 0);
	if (*old_flags < 0) return;
	int flags = blocking ? (*old_flags & ~O_NONBLOCK) : *old_flags;
	fcntl(fd, F_SETFL, flags);
}
}


bool ShouldEnableNetworkPathProfile() {
	static const bool enabled =
		Embarcadero::ReadEnvBoolLenient("EMBAR_PROFILE_NETWORK_PATH", false);
	return enabled;
}

uint64_t NsSince(const std::chrono::steady_clock::time_point& start) {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - start).count());
}

double NsToMs(uint64_t ns) {
	return static_cast<double>(ns) / 1e6;
}

double DurationMs(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
	return static_cast<double>(
		std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) / 1000.0;
}

double BytesToMiB(uint64_t bytes) {
	return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

int64_t SteadyNowNs() {
	return static_cast<int64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

size_t RuntimePayloadSendChunkBytes() {
	static const size_t value = []() {
		const char* env = std::getenv("EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES");
		if (!env || env[0] == '\0') {
			return static_cast<size_t>(ZERO_COPY_SEND_LIMIT);
		}
		char* end = nullptr;
		unsigned long long parsed = std::strtoull(env, &end, 10);
		if (end == env || (end && *end != '\0') || parsed == 0) {
			LOG(WARNING) << "Ignoring invalid EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES='"
			             << env << "'; using " << ZERO_COPY_SEND_LIMIT;
			return static_cast<size_t>(ZERO_COPY_SEND_LIMIT);
		}
		return static_cast<size_t>(parsed);
	}();
	return value;
}

std::string GetThroughputTimeseriesFilePath() {
	const char* explicit_file = std::getenv("EMBARCADERO_THROUGHPUT_TIMESERIES_FILE");
	if (explicit_file && explicit_file[0] != '\0') {
		return std::string(explicit_file);
	}
	return "";
}

int GetThroughputTimeseriesIntervalMs() {
	const char* env = std::getenv("EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS");
	if (!env || env[0] == '\0') return 100;
	char* end = nullptr;
	long parsed = std::strtol(env, &end, 10);
	if (end == env || (end && *end != '\0') || parsed <= 0) return 100;
	if (parsed > 5000) parsed = 5000;
	return static_cast<int>(parsed);
}

bool GetThroughputTimeseriesOriginMs(int64_t& origin_ms_out) {
	const char* env = std::getenv("EMBARCADERO_THROUGHPUT_TIMESERIES_ORIGIN_MS");
	if (!env || env[0] == '\0') return false;
	char* end = nullptr;
	long long parsed = std::strtoll(env, &end, 10);
	if (end == env || (end && *end != '\0')) return false;
	origin_ms_out = static_cast<int64_t>(parsed);
	return true;
}

bool ShouldEnablePayloadMsgMore() {
	static const bool enabled =
		Embarcadero::ReadEnvBoolLenient("EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE", false);
	return enabled;
}

struct ClientBrokerPathProfile {
	std::atomic<uint64_t> payload_bytes{0};
	std::atomic<uint64_t> payload_send_calls{0};
	std::atomic<uint64_t> payload_eagain_events{0};
	std::atomic<uint64_t> payload_wait_ns{0};
	std::atomic<uint64_t> acked_messages{0};
};

struct ClientNetworkPathProfile {
	std::atomic<bool> logged{false};
	std::atomic<uint64_t> header_loop_ns{0};
	std::atomic<uint64_t> header_send_calls{0};
	std::atomic<uint64_t> header_send_bytes{0};
	std::atomic<uint64_t> header_send_syscall_ns{0};
	std::atomic<uint64_t> header_eagain_events{0};
	std::atomic<uint64_t> header_wait_calls{0};
	std::atomic<uint64_t> header_wait_ns{0};
	std::atomic<uint64_t> header_wait_timeouts{0};

	std::atomic<uint64_t> payload_loop_ns{0};
	std::atomic<uint64_t> payload_send_calls{0};
	std::atomic<uint64_t> payload_send_bytes{0};
	std::atomic<uint64_t> payload_send_syscall_ns{0};
	std::atomic<uint64_t> payload_eagain_events{0};
	std::atomic<uint64_t> payload_wait_calls{0};
	std::atomic<uint64_t> payload_wait_ns{0};
	std::atomic<uint64_t> payload_wait_timeouts{0};
	std::atomic<uint64_t> batches_completed{0};

	std::atomic<uint64_t> ack_recv_calls{0};
	std::atomic<uint64_t> ack_recv_syscall_ns{0};
	std::atomic<uint64_t> ack_values_processed{0};
	std::atomic<uint64_t> ack_epoll_calls{0};
	std::atomic<uint64_t> ack_epoll_wait_ns{0};
	std::atomic<uint64_t> ack_epoll_timeouts{0};

	std::array<ClientBrokerPathProfile, NUM_MAX_BROKERS> per_broker{};
};

ClientNetworkPathProfile& GetClientNetworkPathProfile() {
	static ClientNetworkPathProfile profile;
	return profile;
}

void LogClientNetworkPathProfile() {
	if (!ShouldEnableNetworkPathProfile()) return;
	auto& profile = GetClientNetworkPathProfile();
	bool expected = false;
	if (!profile.logged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		return;
	}

	const uint64_t total_payload_bytes = profile.payload_send_bytes.load(std::memory_order_relaxed);
	const uint64_t total_payload_calls = profile.payload_send_calls.load(std::memory_order_relaxed);
	const uint64_t total_payload_syscall_ns = profile.payload_send_syscall_ns.load(std::memory_order_relaxed);
	const uint64_t total_payload_loop_ns = profile.payload_loop_ns.load(std::memory_order_relaxed);
	const uint64_t total_payload_wait_ns = profile.payload_wait_ns.load(std::memory_order_relaxed);
	const uint64_t total_payload_wait_calls = profile.payload_wait_calls.load(std::memory_order_relaxed);
	const uint64_t total_payload_wait_timeouts = profile.payload_wait_timeouts.load(std::memory_order_relaxed);
	const uint64_t total_payload_eagain = profile.payload_eagain_events.load(std::memory_order_relaxed);
	const uint64_t total_batches = profile.batches_completed.load(std::memory_order_relaxed);

	const uint64_t total_header_calls = profile.header_send_calls.load(std::memory_order_relaxed);
	const uint64_t total_header_bytes = profile.header_send_bytes.load(std::memory_order_relaxed);
	const uint64_t total_header_syscall_ns = profile.header_send_syscall_ns.load(std::memory_order_relaxed);
	const uint64_t total_header_loop_ns = profile.header_loop_ns.load(std::memory_order_relaxed);
	const uint64_t total_header_wait_ns = profile.header_wait_ns.load(std::memory_order_relaxed);
	const uint64_t total_header_wait_calls = profile.header_wait_calls.load(std::memory_order_relaxed);
	const uint64_t total_header_wait_timeouts = profile.header_wait_timeouts.load(std::memory_order_relaxed);
	const uint64_t total_header_eagain = profile.header_eagain_events.load(std::memory_order_relaxed);

	const uint64_t total_ack_recv_calls = profile.ack_recv_calls.load(std::memory_order_relaxed);
	const uint64_t total_ack_recv_syscall_ns = profile.ack_recv_syscall_ns.load(std::memory_order_relaxed);
	const uint64_t total_ack_values = profile.ack_values_processed.load(std::memory_order_relaxed);
	const uint64_t total_ack_epoll_calls = profile.ack_epoll_calls.load(std::memory_order_relaxed);
	const uint64_t total_ack_epoll_wait_ns = profile.ack_epoll_wait_ns.load(std::memory_order_relaxed);
	const uint64_t total_ack_epoll_timeouts = profile.ack_epoll_timeouts.load(std::memory_order_relaxed);

	LOG(INFO) << "[NET_PROFILE][CLIENT] payload_bytes_mib=" << std::fixed << std::setprecision(2)
	          << BytesToMiB(total_payload_bytes)
	          << " payload_send_calls=" << total_payload_calls
	          << " payload_send_syscall_ms=" << NsToMs(total_payload_syscall_ns)
	          << " payload_loop_ms=" << NsToMs(total_payload_loop_ns)
	          << " payload_wait_ms=" << NsToMs(total_payload_wait_ns)
	          << " payload_wait_calls=" << total_payload_wait_calls
	          << " payload_wait_timeouts=" << total_payload_wait_timeouts
	          << " payload_eagain=" << total_payload_eagain
	          << " batches=" << total_batches;
	LOG(INFO) << "[NET_PROFILE][CLIENT] header_bytes=" << total_header_bytes
	          << " header_send_calls=" << total_header_calls
	          << " header_send_syscall_ms=" << NsToMs(total_header_syscall_ns)
	          << " header_loop_ms=" << NsToMs(total_header_loop_ns)
	          << " header_wait_ms=" << NsToMs(total_header_wait_ns)
	          << " header_wait_calls=" << total_header_wait_calls
	          << " header_wait_timeouts=" << total_header_wait_timeouts
	          << " header_eagain=" << total_header_eagain;
	LOG(INFO) << "[NET_PROFILE][CLIENT] ack_values=" << total_ack_values
	          << " ack_recv_calls=" << total_ack_recv_calls
	          << " ack_recv_syscall_ms=" << NsToMs(total_ack_recv_syscall_ns)
	          << " ack_epoll_calls=" << total_ack_epoll_calls
	          << " ack_epoll_wait_ms=" << NsToMs(total_ack_epoll_wait_ns)
	          << " ack_epoll_timeouts=" << total_ack_epoll_timeouts;

	for (size_t broker_id = 0; broker_id < profile.per_broker.size(); ++broker_id) {
		const uint64_t broker_payload = profile.per_broker[broker_id].payload_bytes.load(std::memory_order_relaxed);
		const uint64_t broker_calls = profile.per_broker[broker_id].payload_send_calls.load(std::memory_order_relaxed);
		const uint64_t broker_eagain = profile.per_broker[broker_id].payload_eagain_events.load(std::memory_order_relaxed);
		const uint64_t broker_wait_ns = profile.per_broker[broker_id].payload_wait_ns.load(std::memory_order_relaxed);
		const uint64_t broker_acks = profile.per_broker[broker_id].acked_messages.load(std::memory_order_relaxed);
		if (broker_payload == 0 && broker_calls == 0 && broker_eagain == 0 && broker_acks == 0) {
			continue;
		}
		LOG(INFO) << "[NET_PROFILE][CLIENT][BROKER " << broker_id << "] payload_bytes_mib="
		          << std::fixed << std::setprecision(2) << BytesToMiB(broker_payload)
		          << " send_calls=" << broker_calls
		          << " wait_ms=" << NsToMs(broker_wait_ns)
		          << " eagain=" << broker_eagain
		          << " acked_messages=" << broker_acks;
	}
}

namespace {
constexpr int kAckPortMin = 10000;
constexpr int kAckPortMax = 65535;
constexpr int kAckPortRange = kAckPortMax - kAckPortMin + 1;

int GetFailureMeasureIntervalMs() {
	if (const char* env = std::getenv("EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS")) {
		char* end = nullptr;
		long parsed = std::strtol(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			return static_cast<int>(parsed);
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS='" << env
		             << "'; using default 100 ms";
	}
	return 100;
}

size_t GetOrder5HomeBrokers() {
	const char* env = std::getenv("EMBARCADERO_ORDER5_HOME_BROKERS");
	if (!env || env[0] == '\0') return 0;
	char* end = nullptr;
	unsigned long long parsed = std::strtoull(env, &end, 10);
	if (end == env || (end && *end != '\0')) {
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_ORDER5_HOME_BROKERS='" << env << "'";
		return 0;
	}
	return static_cast<size_t>(parsed);
}

// Multi-client SessionOpen storms can exceed the old 1s reply window (N=2×24
// threads). Override with EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC.
int GetSessionOpenTimeoutSec() {
	if (const char* env = std::getenv("EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC")) {
		char* end = nullptr;
		long parsed = std::strtol(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0 && parsed <= 60) {
			return static_cast<int>(parsed);
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC='" << env
		             << "'; using default 5s";
	}
	return 5;
}

// Override with EMBARCADERO_PUBLISH_CONNECT_ATTEMPTS (default 8).
int GetInitialPublishConnectAttempts() {
	if (const char* env = std::getenv("EMBARCADERO_PUBLISH_CONNECT_ATTEMPTS")) {
		char* end = nullptr;
		long parsed = std::strtol(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0 && parsed <= 30) {
			return static_cast<int>(parsed);
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_PUBLISH_CONNECT_ATTEMPTS='" << env
		             << "'; using default 8";
	}
	return 8;
}

// Negative-test hook (opt-in): require EMBARCADERO_ENABLE_CONNECT_FAIL_SIM=1
// plus EMBARCADERO_SIMULATE_CONNECT_FAIL_MOD. Fails queues where (idx % mod) == rem.
bool ShouldSimulateInitialConnectFail(size_t pubQuesIdx) {
	const char* enable = std::getenv("EMBARCADERO_ENABLE_CONNECT_FAIL_SIM");
	if (!enable || enable[0] != '1') return false;
	const char* mod_env = std::getenv("EMBARCADERO_SIMULATE_CONNECT_FAIL_MOD");
	if (!mod_env || !*mod_env) return false;
	char* end = nullptr;
	long mod = std::strtol(mod_env, &end, 10);
	if (end == mod_env || *end != '\0' || mod <= 1) return false;
	long rem = 1;
	if (const char* rem_env = std::getenv("EMBARCADERO_SIMULATE_CONNECT_FAIL_REM")) {
		char* rend = nullptr;
		long parsed = std::strtol(rem_env, &rend, 10);
		if (rend != rem_env && *rend == '\0' && parsed >= 0 && parsed < mod) {
			rem = parsed;
		}
	}
	return static_cast<long>(pubQuesIdx % static_cast<size_t>(mod)) == rem;
}

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
	order_level_(order),
	// [[NEW_BUFFER_FIX]] Start with reasonable initial queue count, dynamically grow as brokers are added.
	// Avoid massive 128-queue allocation when only 4-8 threads are needed initially.
	// See docs/NEW_BUFFER_BANDWIDTH_INVESTIGATION.md.
	pubQue_(num_threads_per_broker_ * 4, num_threads_per_broker_, client_id_, message_size, order),
	seq_type_(seq_type),
	broker_stats_(NUM_MAX_BROKERS),
	start_time_(std::chrono::steady_clock::now()),  // Initialize immediately
	order5_home_brokers_(GetOrder5HomeBrokers()),
	expected_num_brokers_(0)
#ifdef COLLECT_LATENCY_STATS
	,send_records_per_broker_(NUM_MAX_BROKERS),
	send_records_mutexes_(NUM_MAX_BROKERS)
#endif
{
	// Initialize expected_num_brokers_ from environment variable if provided
	if (const char* env_num_brokers = std::getenv("NUM_BROKERS")) {
		try {
			expected_num_brokers_ = std::stoi(env_num_brokers);
			LOG(INFO) << "Publisher: Expecting " << expected_num_brokers_ << " brokers (from NUM_BROKERS env)";
		} catch (...) {
			LOG(WARNING) << "Publisher: Invalid NUM_BROKERS environment variable: " << env_num_brokers;
		}
	} else {
		// Fallback to config if env not set
		expected_num_brokers_ = Embarcadero::GetConfig().config().broker.max_brokers.get();
		LOG(INFO) << "Publisher: Expecting " << expected_num_brokers_ << " brokers (from config max_brokers)";
	}

	// Copy topic name
	memcpy(topic_, topic, TOPIC_NAME_SIZE);

	// Create gRPC stub for head broker
	std::string addr = head_addr + ":" + port;
	stub_ = HeartBeat::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));

	// Initialize first broker
	nodes_[0] = head_addr + ":" + std::to_string(PORT);
	brokers_.emplace_back(0);
	const uint32_t initial_epoch = InitialSessionEpochRequest();
	requested_session_epoch_.store(initial_epoch, std::memory_order_release);
	session_epoch_.store(ReadSessionEpochOverride(), std::memory_order_release);

	VLOG(3) << "Publisher constructed with client_id: " << client_id_ 
		<< ", topic: " << topic 
		<< ", num_threads_per_broker: " << num_threads_per_broker_;
}

std::vector<int> Publisher::Order5HomeBrokerIdsLocked() const {
	if (order5_home_brokers_ == 0 || brokers_.empty()) {
		return brokers_;
	}

	auto mixed_client = static_cast<uint64_t>(client_id_);
	mixed_client += 0x9e3779b97f4a7c15ULL;
	mixed_client = (mixed_client ^ (mixed_client >> 30)) * 0xbf58476d1ce4e5b9ULL;
	mixed_client = (mixed_client ^ (mixed_client >> 27)) * 0x94d049bb133111ebULL;
	mixed_client ^= (mixed_client >> 31);

	const size_t home_size = std::min(order5_home_brokers_, brokers_.size());
	const size_t home_base = static_cast<size_t>(mixed_client % brokers_.size());
	std::vector<int> homes;
	homes.reserve(home_size);
	for (size_t offset = 0; offset < home_size; ++offset) {
		homes.push_back(brokers_[(home_base + offset) % brokers_.size()]);
	}
	return homes;
}

bool Publisher::ShouldConnectPublishThreadsToBrokerLocked(int broker_id) const {
	if (!IsOrder5SessionMode() || order5_home_brokers_ == 0) {
		return true;
	}
	const std::vector<int> homes = Order5HomeBrokerIdsLocked();
	return std::find(homes.begin(), homes.end(), broker_id) != homes.end();
}

void Publisher::RefreshOrder5PreferredQueuesLocked() {
	if (seq_type_ != heartbeat_system::SequencerType::EMBARCADERO ||
	    order_level_ != Embarcadero::kOrderStrong ||
	    order5_home_brokers_ == 0 ||
	    brokers_.empty()) {
		pubQue_.ClearPreferredQueues();
		return;
	}

	const std::vector<int> homes = Order5HomeBrokerIdsLocked();
	std::vector<size_t> preferred_queue_indices;
	preferred_queue_indices.reserve(homes.size() * num_threads_per_broker_);

	for (int broker_id : homes) {
		auto it = broker_queue_indices_.find(broker_id);
		if (it == broker_queue_indices_.end()) continue;
		for (size_t qidx : it->second) {
			// Skip queues whose PublishThread never connected — sealing into them
			// creates permanent ORDER=5 batch_seq holes (N=2 dead-queue failure).
			if (!pubQue_.IsQueueActive(qidx)) continue;
			preferred_queue_indices.push_back(qidx);
		}
	}

	if (preferred_queue_indices.empty()) {
		pubQue_.ClearPreferredQueues();
		return;
	}
	pubQue_.SetPreferredQueues(preferred_queue_indices);
}

void Publisher::ReassignQueueBrokerLocked(size_t queue_idx, int old_broker_id, int new_broker_id) {
	if (old_broker_id == new_broker_id) return;
	auto old_it = broker_queue_indices_.find(old_broker_id);
	if (old_it != broker_queue_indices_.end()) {
		auto& queues = old_it->second;
		queues.erase(std::remove(queues.begin(), queues.end(), queue_idx), queues.end());
	}
	broker_queue_indices_[new_broker_id].push_back(queue_idx);
	RefreshOrder5PreferredQueuesLocked();
}

void Publisher::LogOrder5RoutingSummary() const {
	if (seq_type_ != heartbeat_system::SequencerType::EMBARCADERO ||
	    order_level_ != Embarcadero::kOrderStrong) {
		return;
	}
	std::ostringstream oss;
	oss << "[ORDER5_ROUTING] client_id=" << client_id_
	    << " home_brokers=" << order5_home_brokers_;
	for (size_t broker_id = 0; broker_id < broker_stats_.size(); ++broker_id) {
		const size_t sent = broker_stats_[broker_id].sent_messages.load(std::memory_order_relaxed);
		if (sent == 0) continue;
		oss << " broker" << broker_id << "_msgs=" << sent;
	}
	LOG(INFO) << oss.str();
}

bool Publisher::IsOrder5SessionMode() const {
	return seq_type_ == heartbeat_system::SequencerType::EMBARCADERO &&
	       order_level_ == Embarcadero::kOrderStrong;
}

uint32_t Publisher::InitialSessionEpochRequest() const {
	const uint32_t override_epoch = ReadSessionEpochOverride();
	return override_epoch != 0 ? override_epoch : 1U;
}

uint64_t Publisher::SessionLeaseNs() const {
	if (const char* env = std::getenv("EMBARCADERO_SESSION_LEASE_MS")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			return static_cast<uint64_t>(parsed) * 1000ULL * 1000ULL;
		}
	}
	const bool replicated = ack_level_ == 2;
	return (replicated ? 2000ULL : 1000ULL) * 1000ULL * 1000ULL;
}

bool Publisher::IsMemoryEmulatedAck2() const {
	if (ack_level_ < 2) return false;
	if (const char* sink = std::getenv("EMBARCADERO_CHAIN_REPLICATION_SINK")) {
		if (std::strcmp(sink, "memory-copy") == 0 ||
		    std::strcmp(sink, "memory_copy") == 0 ||
		    std::strcmp(sink, "memory-accounting") == 0 ||
		    std::strcmp(sink, "memory_accounting") == 0 ||
		    std::strcmp(sink, "accounting") == 0 ||
		    std::strcmp(sink, "copy") == 0) {
			return true;
		}
		if (std::strcmp(sink, "disk-durable") == 0 || std::strcmp(sink, "disk") == 0) {
			return false;
		}
	}
	if (const char* inmem = std::getenv("EMBARCADERO_CHAIN_REPLICATION_INMEM")) {
		if (inmem[0] == '1' || inmem[0] == 'y' || inmem[0] == 'Y') {
			return true;
		}
	}
	return false;
}

bool Publisher::Ack2UsesOwnedRtoCopy() const {
	if (ack_level_ < 2) return false;
	if (const char* retention = std::getenv("EMBARCADERO_ACK2_RETENTION")) {
		if (std::strcmp(retention, "owned_rto_copy") == 0 ||
		    std::strcmp(retention, "owned") == 0) {
			return true;
		}
		if (std::strcmp(retention, "pool_pin") == 0 ||
		    std::strcmp(retention, "pin") == 0) {
			return false;
		}
	}
	// Disk-durable ACK2 keeps an owned RTO copy so slow fdatasync cannot pin the
	// hugepage send pool. Memory-emulated ACK2 pins like ACK1 (credit capped).
	return !IsMemoryEmulatedAck2();
}

size_t Publisher::ComputeUnackedByteCap() const {
	const uint64_t lease_ns = SessionLeaseNs();
	// ACK-class offered rates: ACK1 / memory-emulated ACK2 track CXL/GOI BDP;
	// disk ACK2 tracks media-durable BDP so credit cannot outrun fdatasync.
	const bool memory_ack2 = IsMemoryEmulatedAck2();
	uint64_t offered_rate = (ack_level_ >= 2 && !memory_ack2)
		? (1ULL * 1024ULL * 1024ULL * 1024ULL)   // 1 GiB/s durable default
		: (12ULL * 1024ULL * 1024ULL * 1024ULL); // 12 GiB/s CXL / mem-ACK2 default
	if (ack_level_ >= 2) {
		if (const char* env = std::getenv("EMBARCADERO_ACK2_OFFERED_RATE_BYTES_PER_SEC")) {
			char* end = nullptr;
			unsigned long long parsed = std::strtoull(env, &end, 10);
			if (end != env && *end == '\0' && parsed > 0) {
				offered_rate = parsed;
			}
		}
	}
	if (const char* env = std::getenv("EMBARCADERO_OFFERED_RATE_BYTES_PER_SEC")) {
		char* end = nullptr;
		unsigned long long parsed = std::strtoull(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			offered_rate = parsed;
		}
	}
	const long double cap = static_cast<long double>(offered_rate) *
	                        static_cast<long double>(lease_ns) / 1.0e9L;
	const long double max_size = static_cast<long double>(std::numeric_limits<size_t>::max());
	return static_cast<size_t>(std::max<long double>(BATCH_SIZE, std::min(cap, max_size)));
}

void Publisher::ApplyUnackedByteCapBounds() {
	unacked_byte_cap_ = ComputeUnackedByteCap();
	// Only memory-emulated ACK2 pool-pin needs credit ≤ pool (minus seal reserve).
	// ACK1 also pool-pins, but must keep BDP-sized credit: blocking pool acquire
	// already backpressures. Capping ACK1 to ~pool bytes caused N=3 session-fence
	// stalls in AcquireNextBatchFromPool (seen 20260713T175917Z n3 trial3).
	if (IsMemoryEmulatedAck2() && !Ack2UsesOwnedRtoCopy()) {
		const size_t pool_bytes = pubQue_.PoolBytes();
		if (pool_bytes > 0) {
			const size_t reserve = std::min(pool_bytes / 8, 64UL * 1024UL * 1024UL);
			const size_t pin_cap = std::max<size_t>(BATCH_SIZE, pool_bytes - reserve);
			unacked_byte_cap_ = std::min(unacked_byte_cap_, pin_cap);
		}
	}
}

bool Publisher::SendSessionOpenOnSocket(int sock_fd, int, size_t broker_id) {
	if (!IsOrder5SessionMode()) return true;
	uint32_t requested = requested_session_epoch_.load(std::memory_order_acquire);
	if (requested == 0) {
		requested = InitialSessionEpochRequest();
		uint32_t expected = 0;
		requested_session_epoch_.compare_exchange_strong(expected, requested);
	}

	embarcadero::session::SessionOpen open;
	open.set_client_id(static_cast<uint32_t>(client_id_));
	open.set_requested_session_epoch(requested);
	open.set_topic(std::string(topic_, strnlen(topic_, TOPIC_NAME_SIZE)));
	std::string payload;
	if (!open.SerializeToString(&payload)) return false;
	if (payload.size() > kMaxSessionControlPayload) return false;
	const SessionControlHeader header{kSessionControlMagic, static_cast<uint32_t>(payload.size())};

	int old_flags = -1;
	SetSocketBlockingTemporarily(sock_fd, true, &old_flags);
	struct timeval tv;
	tv.tv_sec = GetSessionOpenTimeoutSec();
	tv.tv_usec = 0;
	setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	const bool sent = SendAllBlocking(sock_fd, &header, sizeof(header)) &&
	                  SendAllBlocking(sock_fd, payload.data(), payload.size());
	if (!sent) {
		if (old_flags >= 0) fcntl(sock_fd, F_SETFL, old_flags);
		return false;
	}

	SessionControlHeader ack_header{};
	if (!RecvAllBlocking(sock_fd, &ack_header, sizeof(ack_header)) ||
	    ack_header.magic != kSessionControlMagic ||
	    ack_header.length == 0 ||
	    ack_header.length > kMaxSessionControlPayload) {
		if (old_flags >= 0) fcntl(sock_fd, F_SETFL, old_flags);
		return false;
	}
	std::string ack_payload(ack_header.length, '\0');
	if (!RecvAllBlocking(sock_fd, ack_payload.data(), ack_payload.size())) {
		if (old_flags >= 0) fcntl(sock_fd, F_SETFL, old_flags);
		return false;
	}
	if (old_flags >= 0) fcntl(sock_fd, F_SETFL, old_flags);

	embarcadero::session::SessionOpenAck ack;
	if (!ack.ParseFromString(ack_payload) || ack.assigned_session_epoch() == 0) {
		return false;
	}
	session_epoch_.store(ack.assigned_session_epoch(), std::memory_order_release);
	requested_session_epoch_.store(ack.assigned_session_epoch(), std::memory_order_release);
	LOG(INFO) << "[SESSION_OPEN_ACK]"
	          << " broker_id=" << broker_id
	          << " client_id=" << client_id_
	          << " assigned_session_epoch=" << ack.assigned_session_epoch()
	          << " committed_hwm=" << ack.committed_hwm()
	          << " has_committed_prefix=" << (ack.has_committed_prefix() ? 1 : 0)
	          << " status=" << ack.status();
	if (ack.status() == embarcadero::session::SessionOpenAck::FENCED ||
	    ack.status() == embarcadero::session::SessionOpenAck::EPOCH_STALE) {
		embarcadero::session::SessionFenced fenced;
		if (ack.has_committed_prefix()) {
			fenced.set_committed_batch_seq(ack.committed_hwm());
			fenced.set_has_committed_prefix(true);
		} else {
			fenced.set_committed_batch_seq(0);
			fenced.set_has_committed_prefix(false);
		}
		fenced.set_committed_msg_hwm(0);
		fenced.set_control_epoch(0);
		fenced.set_reason(ack.status() == embarcadero::session::SessionOpenAck::EPOCH_STALE
			? embarcadero::session::SessionFenced::EPOCH_STALE
			: embarcadero::session::SessionFenced::HOLD_EXPIRY);
		HandleSessionFenced(fenced, static_cast<int>(broker_id));
	} else if (ack.has_committed_prefix() && IsOrder5SessionMode() && ack_level_ >= 1) {
		// Trim optimistic unacked prefix through the inclusive reconnect HWM.
		embarcadero::session::SessionFenced trim;
		trim.set_committed_batch_seq(ack.committed_hwm());
		trim.set_has_committed_prefix(true);
		trim.set_committed_msg_hwm(0);
		trim.set_control_epoch(0);
		trim.set_reason(embarcadero::session::SessionFenced::HOLD_EXPIRY);
		std::vector<Embarcadero::BatchHeader*> release_after_unlock;
		{
			std::lock_guard<std::mutex> lock(unacked_mu_);
			for (auto it = unacked_batches_.begin(); it != unacked_batches_.end();) {
				if (it->original_batch_seq <= ack.committed_hwm()) {
					if (it->pool_batch != nullptr) {
						release_after_unlock.push_back(it->pool_batch);
					}
					unacked_bytes_ -= it->wire_bytes;
					it = unacked_batches_.erase(it);
				} else {
					++it;
				}
			}
			unacked_cv_.notify_all();
		}
		for (auto* batch : release_after_unlock) {
			pubQue_.ReleaseBatch(batch);
		}
	}
	return true;
}

void Publisher::WaitForUnackedCapacity(size_t bytes) {
	if (!IsOrder5SessionMode() || ack_level_ < 1 || unacked_byte_cap_ == 0) return;
	std::unique_lock<std::mutex> lock(unacked_mu_);
	unacked_cv_.wait(lock, [&]() {
		return shutdown_.load(std::memory_order_relaxed) ||
		       unacked_bytes_ + bytes <= unacked_byte_cap_;
	});
}

bool Publisher::RecordUnackedBatch(const Embarcadero::BatchHeader& header,
                                   const void* batch_bytes,
                                   size_t wire_bytes,
                                   int broker_id,
                                   size_t) {
	if (!IsOrder5SessionMode() || ack_level_ < 1 || header.session_epoch32 == 0) return false;
	if (batch_bytes == nullptr || wire_bytes < sizeof(Embarcadero::BatchHeader)) return false;
	Embarcadero::StageTrace::Record(
		Embarcadero::StageTrace::Stage::ClientSendDone,
		client_id_,
		header.batch_seq,
		static_cast<uint64_t>(SteadyNowNs()),
		unacked_bytes_);
	UnackedBatch rec;
	rec.original_batch_seq = header.batch_seq;
	rec.current_batch_seq = header.batch_seq;
	rec.num_msg = header.num_msg;
	rec.session_epoch = header.session_epoch32;
	rec.broker_id = broker_id;
	rec.last_send_ns = SteadyNowNs();
	last_unacked_send_ns_.store(rec.last_send_ns, std::memory_order_release);
	rec.wire_bytes = wire_bytes;
	// Disk ACK2: own an RTO copy and release the hugepage send slot immediately
	// so durable latency cannot exhaust the send pool.
	// ACK1 / memory-emulated ACK2: pin the pool slot until ACK (credit capped to
	// pool/2) to avoid a line-rate DRAM memcpy on the fast path.
	const bool owned_rto_copy = Ack2UsesOwnedRtoCopy();
	if (owned_rto_copy) {
		const auto* bytes = static_cast<const uint8_t*>(batch_bytes);
		rec.wire.assign(bytes, bytes + wire_bytes);
	} else {
		rec.pool_batch = const_cast<Embarcadero::BatchHeader*>(
			static_cast<const Embarcadero::BatchHeader*>(batch_bytes));
	}
	std::lock_guard<std::mutex> lock(unacked_mu_);
	session_sent_hwm_ += rec.num_msg;
	rec.broker_ack_end = std::numeric_limits<size_t>::max();
	unacked_bytes_ += rec.wire_bytes;
	if (unacked_batches_.empty() ||
	    rec.current_batch_seq >= unacked_batches_.back().current_batch_seq) {
		unacked_batches_.push_back(std::move(rec));
	} else {
		auto it = std::upper_bound(unacked_batches_.begin(), unacked_batches_.end(),
			rec.current_batch_seq,
			[](uint64_t target_batch_seq, const UnackedBatch& candidate) {
				return target_batch_seq < candidate.current_batch_seq;
			});
		unacked_batches_.insert(it, std::move(rec));
	}
	return !owned_rto_copy;
}

void Publisher::CompleteUnackedThrough(int, size_t broker_ack_hwm) {
	if (!IsOrder5SessionMode() || ack_level_ < 1) return;
	const int64_t now_ns = SteadyNowNs();
	std::vector<Embarcadero::BatchHeader*> release_after_unlock;
	{
		std::lock_guard<std::mutex> lock(unacked_mu_);
		for (auto it = unacked_batches_.begin(); it != unacked_batches_.end();) {
			size_t candidate_hwm = 0;
			if (!SessionPrefixAckEnd(it->current_batch_seq,
			                         session_next_retire_batch_seq_,
			                         session_retire_prefix_hwm_,
			                         it->num_msg,
			                         &candidate_hwm)) {
				break;
			}
			it->broker_ack_end = candidate_hwm;
			if (!SessionGlobalUnackedRetired(candidate_hwm, broker_ack_hwm)) {
				break;
			}
			{
				std::lock_guard<std::mutex> delta_lock(delta_mu_);
				delta_estimator_.sample(static_cast<uint64_t>(std::max<int64_t>(0, now_ns - it->last_send_ns)),
				                        it->attempt);
			}
			session_retire_prefix_hwm_ = candidate_hwm;
			session_next_retire_batch_seq_++;
			unacked_bytes_ -= it->wire_bytes;
			if (it->pool_batch != nullptr) {
				release_after_unlock.push_back(it->pool_batch);
			}
			it = unacked_batches_.erase(it);
			unacked_cv_.notify_all();
		}
	}
	for (auto* batch : release_after_unlock) {
		pubQue_.ReleaseBatch(batch);
	}
}

bool Publisher::EnsureRetransmitChannel(int broker_id, int* out_fd) {
	if (out_fd == nullptr) return false;
	{
		std::lock_guard<std::mutex> lock(retransmit_channel_mu_);
		auto it = retransmit_channels_.find(broker_id);
		if (it != retransmit_channels_.end() && it->second >= 0) {
			*out_fd = it->second;
			return true;
		}
	}

	std::string addr;
	size_t num_brokers = 0;
	{
		absl::MutexLock lock(&mutex_);
		auto it = nodes_.find(broker_id);
		if (it == nodes_.end()) return false;
		try {
			auto parsed = ParseAddressPort(it->second);
			addr = parsed.first;
		} catch (const std::exception& e) {
			LOG(ERROR) << "EnsureRetransmitChannel: invalid broker address " << it->second
			           << ": " << e.what();
			return false;
		}
		num_brokers = nodes_.size();
	}

	ScopedFd sock(GetNonblockingSock(const_cast<char*>(addr.c_str()), PORT + broker_id));
	if (sock.get() < 0) return false;
	ScopedFd efd(epoll_create1(0));
	if (efd.get() < 0) return false;
	epoll_event event;
	event.data.fd = sock.get();
	event.events = EPOLLOUT | EPOLLRDHUP;
	if (epoll_ctl(efd.get(), EPOLL_CTL_ADD, sock.get(), &event) != 0) return false;
	epoll_event events[4];
	int ready = epoll_wait(efd.get(), events, 4, 1000);
	if (ready <= 0) return false;
	int sock_err = 0;
	socklen_t sock_err_len = sizeof(sock_err);
	if (getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len) != 0 ||
	    sock_err != 0) {
		return false;
	}
	int old_flags = -1;
	SetSocketBlockingTemporarily(sock.get(), true, &old_flags);

	Embarcadero::EmbarcaderoReq shake;
	std::memset(&shake, 0, sizeof(shake));
	shake.client_req = Embarcadero::Publish;
	shake.client_id = client_id_;
	std::memcpy(shake.topic, topic_, std::min<size_t>(TOPIC_NAME_SIZE - 1, sizeof(shake.topic) - 1));
	shake.ack = ack_level_;
	shake.port = ack_port_;
	shake.num_msg = num_brokers;
	if (!SendAllBlocking(sock.get(), &shake, sizeof(shake))) return false;
	if (!SendSessionOpenOnSocket(sock.get(), efd.get(), static_cast<size_t>(broker_id))) return false;
	if (old_flags >= 0) fcntl(sock.get(), F_SETFL, old_flags);

	const int fd = sock.release();
	{
		std::lock_guard<std::mutex> lock(retransmit_channel_mu_);
		auto it = retransmit_channels_.find(broker_id);
		if (it != retransmit_channels_.end() && it->second >= 0) {
			close(fd);
			*out_fd = it->second;
			return true;
		}
		retransmit_channels_[broker_id] = fd;
		*out_fd = fd;
	}
	return true;
}

void Publisher::CloseRetransmitChannel(int broker_id) {
	std::lock_guard<std::mutex> lock(retransmit_channel_mu_);
	auto it = retransmit_channels_.find(broker_id);
	if (it == retransmit_channels_.end()) return;
	if (it->second >= 0) {
		close(it->second);
	}
	retransmit_channels_.erase(it);
}

bool Publisher::SendRawBatchToBroker(const void* bytes, size_t wire_bytes, int broker_id) {
	if (bytes == nullptr || wire_bytes < sizeof(Embarcadero::BatchHeader)) return false;
	int fd = -1;
	if (!EnsureRetransmitChannel(broker_id, &fd) || fd < 0) return false;
	int old_flags = -1;
	SetSocketBlockingTemporarily(fd, true, &old_flags);
	const bool ok = SendAllBlocking(fd, bytes, wire_bytes);
	if (old_flags >= 0) fcntl(fd, F_SETFL, old_flags);
	if (!ok) {
		CloseRetransmitChannel(broker_id);
		return false;
	}
	return true;
}

void Publisher::WaitForSessionSendDrain(size_t target_messages) {
	const int64_t deadline_ns = SteadyNowNs() + static_cast<int64_t>(SessionLeaseNs());
	while (SteadyNowNs() < deadline_ns) {
		size_t sent_hwm = 0;
		{
			std::lock_guard<std::mutex> lock(unacked_mu_);
			sent_hwm = ack_message_base_.load(std::memory_order_acquire) + session_sent_hwm_;
		}
		if (sent_hwm >= target_messages) return;
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	LOG(WARNING) << "[SESSION_ROLLOVER_DRAIN_TIMEOUT]"
	             << " target_messages=" << target_messages;
}

void Publisher::HandleSessionFenced(const embarcadero::session::SessionFenced& fenced, int broker_id) {
	if (!IsOrder5SessionMode()) return;

	// During ACK2 durable drain, identical lease restates of a frozen committed
	// prefix must not reopen/resubmit (that re-burns BLog before ingest dedup).
	// A newer HWM with remaining unacked suffix still falls through to reopen so
	// held/purged batches can complete after force-expire fencing.
	if (publish_finished_.load(std::memory_order_acquire) && ack_level_ >= 2 &&
	    ack_drain_active_.load(std::memory_order_acquire) &&
	    fenced.has_committed_prefix()) {
		const uint64_t release_hwm = fenced.committed_batch_seq();
		std::vector<Embarcadero::BatchHeader*> release_after_unlock;
		size_t locally_committed_msgs = 0;
		size_t remaining_suffix = 0;
		{
			std::lock_guard<std::mutex> lock(unacked_mu_);
			for (auto it = unacked_batches_.begin(); it != unacked_batches_.end();) {
				if (it->original_batch_seq <= release_hwm) {
					locally_committed_msgs += it->num_msg;
					unacked_bytes_ -= it->wire_bytes;
					if (it->pool_batch != nullptr) {
						release_after_unlock.push_back(it->pool_batch);
					}
					it = unacked_batches_.erase(it);
				} else {
					++remaining_suffix;
					++it;
				}
			}
			if (locally_committed_msgs > 0) {
				unacked_cv_.notify_all();
			}
		}
		for (auto* batch : release_after_unlock) {
			pubQue_.ReleaseBatch(batch);
		}
		if (locally_committed_msgs > 0) {
			broker_stats_[0].acked_messages.fetch_add(locally_committed_msgs, std::memory_order_relaxed);
			ack_received_.fetch_add(locally_committed_msgs, std::memory_order_release);
			last_ack_progress_ns_.store(SteadyNowNs(), std::memory_order_release);
			LOG(WARNING) << "[SESSION_FENCE_ACK_DRAIN_CREDIT]"
			             << " broker_id=" << broker_id
			             << " committed_batch_seq=" << release_hwm
			             << " credited_msgs=" << locally_committed_msgs
			             << " remaining_suffix=" << remaining_suffix;
		}
		const uint64_t prev_hwm =
			last_ack_drain_fence_hwm_.load(std::memory_order_acquire);
		if (remaining_suffix == 0 || release_hwm == prev_hwm) {
			VLOG(1) << "[SESSION_FENCE_SUPPRESSED_DURING_ACK_DRAIN]"
			        << " broker_id=" << broker_id
			        << " committed_batch_seq=" << release_hwm
			        << " remaining_suffix=" << remaining_suffix
			        << " same_hwm=" << (release_hwm == prev_hwm ? 1 : 0);
			return;
		}
		last_ack_drain_fence_hwm_.store(release_hwm, std::memory_order_release);
		LOG(WARNING) << "[SESSION_FENCE_ACK_DRAIN_RESUBMIT]"
		             << " broker_id=" << broker_id
		             << " committed_batch_seq=" << release_hwm
		             << " remaining_suffix=" << remaining_suffix;
		// Fall through to reopen + suffix resubmit.
	}

	const uint32_t old_epoch = session_epoch_.load(std::memory_order_acquire);
	const uint32_t new_epoch = NextSessionEpochAfterFence(old_epoch);
	session_fenced_observed_.fetch_add(1, std::memory_order_relaxed);
	const uint64_t release_hwm = fenced.has_committed_prefix() ? fenced.committed_batch_seq() : UINT64_MAX;
	// UINT64_MAX sentinel means empty prefix: credit nothing by batch_seq compare.
	session_fenced_committed_batch_seq_.store(
		fenced.has_committed_prefix() ? fenced.committed_batch_seq() : 0,
		std::memory_order_release);
	session_fenced_reopen_pending_.store(true, std::memory_order_release);
	pubQue_.PauseSessionRollover();
	struct RolloverResumeGuard {
		Publisher* self;
		bool active{true};
		~RolloverResumeGuard() {
			if (!active) return;
			self->session_fenced_reopen_pending_.store(false, std::memory_order_release);
			self->pubQue_.ResumeSessionRollover();
		}
		void dismiss() { active = false; }
	} rollover_guard{this};
	const size_t sealed = pubQue_.SealAllForSessionRollover();
	if (sealed > 0) {
		client_order_.fetch_add(sealed, std::memory_order_release);
	}
	WaitForSessionSendDrain(client_order_.load(std::memory_order_acquire));
	requested_session_epoch_.store(new_epoch, std::memory_order_release);
	session_epoch_.store(new_epoch, std::memory_order_release);
	last_ack_progress_ns_.store(SteadyNowNs(), std::memory_order_release);

	std::vector<UnackedBatch> suffix;
	std::vector<Embarcadero::BatchHeader*> release_after_unlock;
	size_t locally_committed_msgs = 0;
	const size_t ack_base_before_local_credit = ack_received_.load(std::memory_order_acquire);
	{
		std::lock_guard<std::mutex> lock(unacked_mu_);
		for (auto& rec : unacked_batches_) {
			if (rec.session_epoch != old_epoch) continue;
			if (fenced.has_committed_prefix() && rec.original_batch_seq <= release_hwm) {
				locally_committed_msgs += rec.num_msg;
				if (rec.pool_batch != nullptr) {
					release_after_unlock.push_back(rec.pool_batch);
				}
			} else {
				suffix.push_back(std::move(rec));
			}
		}
		std::sort(suffix.begin(), suffix.end(), [](const UnackedBatch& a, const UnackedBatch& b) {
			return a.original_batch_seq < b.original_batch_seq;
		});
		unacked_batches_.clear();
		unacked_bytes_ = 0;
		session_sent_hwm_ = 0;
		session_retire_prefix_hwm_ = 0;
		session_next_retire_batch_seq_ = 0;
		unacked_cv_.notify_all();
	}
	for (auto* batch : release_after_unlock) {
		pubQue_.ReleaseBatch(batch);
	}
	if (locally_committed_msgs > 0) {
		broker_stats_[0].acked_messages.fetch_add(locally_committed_msgs, std::memory_order_relaxed);
		ack_received_.fetch_add(locally_committed_msgs, std::memory_order_release);
		LOG(WARNING) << "[SESSION_FENCE_LOCAL_COMMIT]"
		             << " old_epoch=" << old_epoch
		             << " committed_batch_seq=" << fenced.committed_batch_seq()
		             << " credited_msgs=" << locally_committed_msgs;
	}
	const size_t ack_after_local_credit = ack_received_.load(std::memory_order_acquire);
	const size_t rebased_ack_base =
		RebasedAckBaseAfterFenceCredit(ack_base_before_local_credit, locally_committed_msgs);
	CHECK_EQ(rebased_ack_base, ack_after_local_credit);
	ack_message_base_.store(rebased_ack_base, std::memory_order_release);
	order5_last_ack_hwm_.store(rebased_ack_base, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(unacked_mu_);
		session_retire_prefix_hwm_ = rebased_ack_base;
	}

	uint64_t new_seq = 0;
	for (auto& rec : suffix) {
		auto* header = rec.Header();
		if (header == nullptr) continue;
		header->batch_seq = new_seq++;
		header->session_epoch = static_cast<uint16_t>(new_epoch & 0xFFFFU);
		header->session_epoch32 = new_epoch;
		header->client_id = client_id_;
		std::vector<int> survivors;
		{
			absl::MutexLock lock(&mutex_);
			survivors = brokers_;
		}
		const int target = RendezvousBroker(static_cast<uint32_t>(client_id_),
		                                    header->batch_seq,
		                                    survivors,
		                                    -1);
		if (target < 0) continue;
		header->broker_id = static_cast<uint32_t>(target);
		SendRawBatchToBroker(rec.WireBytes(), rec.wire_bytes, target);
		rec.current_batch_seq = header->batch_seq;
		rec.session_epoch = new_epoch;
		rec.broker_id = target;
		rec.attempt = 1;
		rec.last_send_ns = SteadyNowNs();
		last_unacked_send_ns_.store(rec.last_send_ns, std::memory_order_release);
		std::lock_guard<std::mutex> lock(unacked_mu_);
		session_sent_hwm_ += rec.num_msg;
		rec.broker_ack_end = std::numeric_limits<size_t>::max();
		unacked_bytes_ += rec.wire_bytes;
		unacked_batches_.push_back(std::move(rec));
	}
	pubQue_.SetNextBatchSeqForNewSession(NextBatchSeqAfterSuffixResubmit(static_cast<size_t>(new_seq)));
	session_fenced_reopen_pending_.store(false, std::memory_order_release);
	pubQue_.ResumeSessionRollover();
	rollover_guard.dismiss();
	LOG(WARNING) << "[SESSION_REOPEN_RESUBMIT]"
	             << " old_epoch=" << old_epoch
	             << " new_epoch=" << new_epoch
	             << " committed_batch_seq=" << fenced.committed_batch_seq()
	             << " suffix_batches=" << suffix.size();
}

void Publisher::RetransmitThread() {
	while (!shutdown_.load(std::memory_order_relaxed)) {
		double delta_ms = DeltaEstimator::kDeltaFloorMs;
		{
			std::lock_guard<std::mutex> lock(delta_mu_);
			delta_ms = delta_estimator_.delta_ms();
		}
		const double sleep_ms = std::min(delta_ms / 2.0, 2.0);
		std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(sleep_ms * 1000.0)));

		// During durable ACK2 drain, suppress RTO storms that re-burn BLog for
		// already-ingested batches. Allow a slow backstop after prolonged stall so
		// truly-lost in-flight batches can still recover before the ACK timeout.
		{
			static thread_local int64_t drain_stall_started_ns = 0;
			const bool in_ack2_drain =
				ack_drain_active_.load(std::memory_order_acquire) && ack_level_ >= 2;
			if (!in_ack2_drain) {
				drain_stall_started_ns = 0;
			} else {
				const int64_t now_ns = SteadyNowNs();
				if (drain_stall_started_ns == 0) {
					drain_stall_started_ns = now_ns;
				}
				constexpr int64_t kAckDrainRtoBackstopNs =
					30LL * 1000LL * 1000LL * 1000LL;  // 30s
				if (now_ns - drain_stall_started_ns < kAckDrainRtoBackstopNs) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				// Fall through once for a backstop retransmit pass, then reset.
				drain_stall_started_ns = now_ns;
			}
		}

		const int64_t now_ns = SteadyNowNs();
		struct DueRetransmit {
			std::vector<uint8_t> bytes;
			int broker_id{-1};
			uint64_t current_batch_seq{0};
		};
		std::vector<DueRetransmit> due;
		{
			std::lock_guard<std::mutex> lock(unacked_mu_);
			for (auto& rec : unacked_batches_) {
				const double backoff_ms = delta_ms * static_cast<double>(1ULL << std::min<uint32_t>(rec.attempt, 10));
				const double backstop_ms = std::max(backoff_ms, DeltaEstimator::kDeltaCapMs);
				const int64_t due_ns = static_cast<int64_t>(backstop_ms * 1.0e6);
				const int64_t last_progress_ns = std::max(
					last_unacked_send_ns_.load(std::memory_order_acquire),
					last_ack_progress_ns_.load(std::memory_order_acquire));
				if (last_progress_ns > 0 && now_ns - last_progress_ns < due_ns) {
					continue;
				}
				if (now_ns - rec.last_send_ns >= due_ns) {
					DueRetransmit item;
					item.broker_id = rec.broker_id;
					item.current_batch_seq = rec.current_batch_seq;
					const void* src = rec.WireBytes();
					if (src != nullptr && rec.wire_bytes > 0) {
						item.bytes.resize(rec.wire_bytes);
						std::memcpy(item.bytes.data(), src, rec.wire_bytes);
						due.push_back(std::move(item));
					}
					rec.attempt++;
					rec.last_send_ns = now_ns;
				}
			}
		}
		for (auto& rec : due) {
			std::vector<int> survivors;
			{
				absl::MutexLock lock(&mutex_);
				survivors = brokers_;
			}
			const int target = RendezvousBroker(static_cast<uint32_t>(client_id_),
			                                    rec.current_batch_seq,
			                                    survivors,
			                                    rec.broker_id);
			if (target < 0) continue;
			auto* header = reinterpret_cast<Embarcadero::BatchHeader*>(rec.bytes.data());
			header->broker_id = static_cast<uint32_t>(target);
			header->session_epoch = static_cast<uint16_t>(session_epoch_.load(std::memory_order_acquire) & 0xFFFFU);
			header->session_epoch32 = session_epoch_.load(std::memory_order_acquire);
			if (SendRawBatchToBroker(rec.bytes.data(), rec.bytes.size(), target)) {
				retransmit_attempts_.fetch_add(1, std::memory_order_relaxed);
				last_unacked_send_ns_.store(SteadyNowNs(), std::memory_order_release);
			}
		}
	}
}

Publisher::~Publisher() {
	VLOG(3) << "Publisher destructor called, cleaning up resources";

	// Benchmark paths that rely on explicit sync barriers can destroy Publisher
	// without ever calling Poll(). Flush any partial batch and wake queue readers so
	// PublishThread instances can drain/exit before we join them here.
	WriteFinishedOrPaused();
	pubQue_.WriteFinished();
	pubQue_.ReturnReads();

	// Signal all threads to terminate [[RELAXED: Simple flags don't need ordering]]
	publish_finished_.store(true, std::memory_order_relaxed);
	shutdown_.store(true, std::memory_order_relaxed);
	consumer_should_exit_.store(true, std::memory_order_relaxed);
	unacked_cv_.notify_all();
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
	{
		std::lock_guard<std::mutex> lock(retransmit_channel_mu_);
		for (auto& [broker_id, fd] : retransmit_channels_) {
			(void)broker_id;
			if (fd >= 0) close(fd);
		}
		retransmit_channels_.clear();
	}

	if (retransmit_thread_.joinable()) {
		retransmit_thread_.join();
	}

	if (real_time_throughput_measure_thread_.joinable()) {
		real_time_throughput_measure_thread_.join();
	}

	if (kill_brokers_thread_.joinable()) {
		kill_brokers_thread_.join();
	}

	LogClientNetworkPathProfile();

	VLOG(3) << "Publisher destructor return";
}

#ifdef COLLECT_LATENCY_STATS
void Publisher::RecordPublishSend(
		int broker_id,
		size_t end_count,
		const std::chrono::steady_clock::time_point& submit_time,
		bool has_submit_time) {
	if (!record_results_ || ack_level_ < 1 || end_count == 0) {
		return;
	}
	BatchSendRecord record{end_count, std::chrono::steady_clock::now(), submit_time, has_submit_time};
	{
		std::lock_guard<std::mutex> lock(send_records_mutexes_[broker_id]);
		auto& records = send_records_per_broker_[broker_id];
		// Keep records sorted by end_count so ACK processing can pop_front in O(k).
		if (records.empty() || record.end_count >= records.back().end_count) {
			records.push_back(record);
		} else {
			publish_latency_out_of_order_inserts_.fetch_add(1, std::memory_order_relaxed);
			auto it = std::upper_bound(records.begin(), records.end(), record.end_count,
				[](size_t target_end_count, const BatchSendRecord& candidate) {
					return target_end_count < candidate.end_count;
				});
			records.insert(it, record);
		}
	}
}
#endif

#ifdef COLLECT_LATENCY_STATS
void Publisher::ProcessPublishAckLatency(int broker_id, size_t acked_msg) {
	if (!record_results_ || ack_level_ < 1) {
		return;
	}
	std::vector<long long> local_send_to_ack_latencies;
	std::vector<long long> local_submit_to_ack_latencies;
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(send_records_mutexes_[broker_id]);
		auto& records = send_records_per_broker_[broker_id];
		while (!records.empty() && records.front().end_count <= acked_msg) {
			const auto send_to_ack_latency = std::chrono::duration_cast<std::chrono::microseconds>(
					now - records.front().sent_time).count();
			local_send_to_ack_latencies.push_back(send_to_ack_latency);
			if (records.front().has_submit_time) {
				const auto submit_to_ack_latency = std::chrono::duration_cast<std::chrono::microseconds>(
						now - records.front().submit_time).count();
				local_submit_to_ack_latencies.push_back(submit_to_ack_latency);
			}
			records.pop_front();
		}
	}
	if (!local_send_to_ack_latencies.empty() || !local_submit_to_ack_latencies.empty()) {
		std::lock_guard<std::mutex> lock(publish_latency_mutex_);
		if (!local_send_to_ack_latencies.empty()) {
			publish_send_to_ack_latencies_us_.insert(
					publish_send_to_ack_latencies_us_.end(),
					local_send_to_ack_latencies.begin(),
					local_send_to_ack_latencies.end());
			publish_send_to_ack_batch_samples_recorded_.fetch_add(local_send_to_ack_latencies.size(), std::memory_order_relaxed);
		}
		if (!local_submit_to_ack_latencies.empty()) {
			publish_submit_to_ack_latencies_us_.insert(
					publish_submit_to_ack_latencies_us_.end(),
					local_submit_to_ack_latencies.begin(),
					local_submit_to_ack_latencies.end());
			publish_submit_to_ack_batch_samples_recorded_.fetch_add(local_submit_to_ack_latencies.size(), std::memory_order_relaxed);
		}
	}
}
#endif

#ifdef COLLECT_LATENCY_STATS
void Publisher::WritePublishLatencyResults() {
	if (!record_results_ || ack_level_ < 1) {
		return;
	}
	std::vector<long long> send_to_ack_latencies_copy;
	std::vector<long long> submit_to_ack_latencies_copy;
	{
		std::lock_guard<std::mutex> lock(publish_latency_mutex_);
		send_to_ack_latencies_copy = publish_send_to_ack_latencies_us_;
		submit_to_ack_latencies_copy = publish_submit_to_ack_latencies_us_;
	}
	if (send_to_ack_latencies_copy.empty() && submit_to_ack_latencies_copy.empty()) {
		LOG(WARNING) << "No publish ACK latency values could be calculated.";
		return;
	}

	const bool have_send_metric = !send_to_ack_latencies_copy.empty();
	const bool have_submit_metric = !submit_to_ack_latencies_copy.empty();
	const bool have_ordered_metric = (ack_level_ == 1) && have_send_metric;
	Embarcadero::LatencyStats::Summary send_to_ack_summary{};
	Embarcadero::LatencyStats::Summary submit_to_ack_summary{};
	Embarcadero::LatencyStats::Summary send_to_ordered_summary{};
	if (have_send_metric) {
		std::sort(send_to_ack_latencies_copy.begin(), send_to_ack_latencies_copy.end());
		send_to_ack_summary = Embarcadero::LatencyStats::ComputeSummary(send_to_ack_latencies_copy);
		if (have_ordered_metric) {
			send_to_ordered_summary = send_to_ack_summary;
		}
	}
	if (have_submit_metric) {
		std::sort(submit_to_ack_latencies_copy.begin(), submit_to_ack_latencies_copy.end());
		submit_to_ack_summary = Embarcadero::LatencyStats::ComputeSummary(submit_to_ack_latencies_copy);
	}
	const size_t send_to_ack_samples_recorded = publish_send_to_ack_batch_samples_recorded_.load(std::memory_order_relaxed);
	const size_t submit_to_ack_samples_recorded = publish_submit_to_ack_batch_samples_recorded_.load(std::memory_order_relaxed);
	const size_t acked_messages = ack_received_.load(std::memory_order_relaxed);
	const size_t total_batches_sent = total_batches_sent_.load(std::memory_order_relaxed);
	if ((have_send_metric && send_to_ack_summary.count > total_batches_sent) ||
		(have_submit_metric && submit_to_ack_summary.count > total_batches_sent)) {
		LOG(WARNING) << "Publish ACK batch latency samples exceed total sent batches: send_samples="
		             << (have_send_metric ? send_to_ack_summary.count : 0)
		             << " submit_samples=" << (have_submit_metric ? submit_to_ack_summary.count : 0)
		             << " total_batches_sent=" << total_batches_sent;
	}
	if (have_send_metric && send_to_ack_samples_recorded != send_to_ack_summary.count) {
		LOG(WARNING) << "Publish send->ack sample counter mismatch: counter=" << send_to_ack_samples_recorded
		             << " summarized=" << send_to_ack_summary.count;
	}
	if (have_submit_metric && submit_to_ack_samples_recorded != submit_to_ack_summary.count) {
		LOG(WARNING) << "Publish submit->ack sample counter mismatch: counter=" << submit_to_ack_samples_recorded
		             << " summarized=" << submit_to_ack_summary.count;
	}
	const size_t missing_submit_timestamps = publish_submit_time_missing_.load(std::memory_order_relaxed);
	if (missing_submit_timestamps > 0) {
		LOG(WARNING) << "Submit->ACK metric dropped " << missing_submit_timestamps
		             << " batch sample(s) due to missing submit timestamps.";
	}

	if (have_submit_metric) {
		LOG(INFO) << "Publish Submit->ACK Batch Latency Statistics (us):";
		LOG(INFO) << "  Count:   " << submit_to_ack_summary.count;
		LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << submit_to_ack_summary.average_us;
		LOG(INFO) << "  Min:     " << submit_to_ack_summary.min_us;
		LOG(INFO) << "  P50:     " << submit_to_ack_summary.p50_us;
		LOG(INFO) << "  P99:     " << submit_to_ack_summary.p99_us;
		LOG(INFO) << "  P99.9:   " << submit_to_ack_summary.p999_us;
		LOG(INFO) << "  Max:     " << submit_to_ack_summary.max_us;
		LOG(INFO) << "  Semantics: append_submit_to_ack batch latency (sample granularity=batch)";
		LOG(INFO) << "  Invariant: batch_samples <= total_batches_sent (samples=" << submit_to_ack_summary.count
		          << ", total_batches_sent=" << total_batches_sent << ")";
	} else {
		LOG(WARNING) << "Publish Submit->ACK Batch Latency Statistics unavailable (no valid submit timestamps).";
	}
	if (have_send_metric) {
		LOG(INFO) << "Publish Send->ACK Batch Latency Statistics (us):";
		LOG(INFO) << "  Count:   " << send_to_ack_summary.count;
		LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << send_to_ack_summary.average_us;
		LOG(INFO) << "  Min:     " << send_to_ack_summary.min_us;
		LOG(INFO) << "  P50:     " << send_to_ack_summary.p50_us;
		LOG(INFO) << "  P99:     " << send_to_ack_summary.p99_us;
		LOG(INFO) << "  P99.9:   " << send_to_ack_summary.p999_us;
		LOG(INFO) << "  Max:     " << send_to_ack_summary.max_us;
		LOG(INFO) << "  Semantics: append_send_to_ack batch latency (sample granularity=batch)";
	} else {
		LOG(WARNING) << "Publish Send->ACK Batch Latency Statistics unavailable.";
	}
	if (have_ordered_metric) {
		LOG(INFO) << "Publish Send->Ordered Batch Latency Statistics (us):";
		LOG(INFO) << "  Count:   " << send_to_ordered_summary.count;
		LOG(INFO) << "  Average: " << std::fixed << std::setprecision(3) << send_to_ordered_summary.average_us;
		LOG(INFO) << "  Min:     " << send_to_ordered_summary.min_us;
		LOG(INFO) << "  P50:     " << send_to_ordered_summary.p50_us;
		LOG(INFO) << "  P99:     " << send_to_ordered_summary.p99_us;
		LOG(INFO) << "  P99.9:   " << send_to_ordered_summary.p999_us;
		LOG(INFO) << "  Max:     " << send_to_ordered_summary.max_us;
		LOG(INFO) << "  Semantics: append_send_to_ordered batch latency (derived from ACK path at ack_level=1)";
	}
	LOG(INFO) << "  Submit timestamps missing (submit metric sample drops): " << missing_submit_timestamps;

	const std::string latency_filename = "pub_latency_stats.csv";
	std::ofstream latency_file(latency_filename);
	if (!latency_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << latency_filename;
	} else {
		latency_file << "Average,Min,Median,p90,p95,p99,p999,Max,Count,Metric,Unit,PercentileMethod,Granularity,SampleCountMeaning,AckedMessages,TotalBatchesSent,OutOfOrderInserts,MissingSubmitTimestamps\n";
		if (have_submit_metric) {
			latency_file << std::fixed << std::setprecision(3) << submit_to_ack_summary.average_us
				<< "," << submit_to_ack_summary.min_us
				<< "," << submit_to_ack_summary.p50_us
				<< "," << submit_to_ack_summary.p90_us
				<< "," << submit_to_ack_summary.p95_us
				<< "," << submit_to_ack_summary.p99_us
				<< "," << submit_to_ack_summary.p999_us
				<< "," << submit_to_ack_summary.max_us
				<< "," << submit_to_ack_summary.count
				<< ",append_submit_to_ack_batch_latency"
				<< ",us"
				<< "," << Embarcadero::LatencyStats::kPercentileMethod
				<< ",batch"
				<< ",samples=count_of_fully_acked_batches_with_submit_timestamp"
				<< "," << acked_messages
				<< "," << total_batches_sent
				<< "," << publish_latency_out_of_order_inserts_.load(std::memory_order_relaxed)
				<< "," << missing_submit_timestamps
				<< "\n";
		}
		if (have_send_metric) {
			latency_file << std::fixed << std::setprecision(3) << send_to_ack_summary.average_us
				<< "," << send_to_ack_summary.min_us
				<< "," << send_to_ack_summary.p50_us
				<< "," << send_to_ack_summary.p90_us
				<< "," << send_to_ack_summary.p95_us
				<< "," << send_to_ack_summary.p99_us
				<< "," << send_to_ack_summary.p999_us
				<< "," << send_to_ack_summary.max_us
				<< "," << send_to_ack_summary.count
				<< ",append_send_to_ack_batch_latency"
				<< ",us"
				<< "," << Embarcadero::LatencyStats::kPercentileMethod
				<< ",batch"
				<< ",samples=count_of_fully_acked_batches"
				<< "," << acked_messages
				<< "," << total_batches_sent
				<< "," << publish_latency_out_of_order_inserts_.load(std::memory_order_relaxed)
				<< "," << missing_submit_timestamps
				<< "\n";
		}
		if (have_ordered_metric) {
			latency_file << std::fixed << std::setprecision(3) << send_to_ordered_summary.average_us
				<< "," << send_to_ordered_summary.min_us
				<< "," << send_to_ordered_summary.p50_us
				<< "," << send_to_ordered_summary.p90_us
				<< "," << send_to_ordered_summary.p95_us
				<< "," << send_to_ordered_summary.p99_us
				<< "," << send_to_ordered_summary.p999_us
				<< "," << send_to_ordered_summary.max_us
				<< "," << send_to_ordered_summary.count
				<< ",append_send_to_ordered_batch_latency"
				<< ",us"
				<< "," << Embarcadero::LatencyStats::kPercentileMethod
				<< ",batch"
				<< ",samples=count_of_fully_ordered_batches_derived_from_ack_level_1"
				<< "," << acked_messages
				<< "," << total_batches_sent
				<< "," << publish_latency_out_of_order_inserts_.load(std::memory_order_relaxed)
				<< "," << missing_submit_timestamps
				<< "\n";
		}
		latency_file.close();
	}

	const std::string cdf_filename = "pub_cdf_latency_us.csv";
	std::ofstream cdf_file(cdf_filename);
	if (!cdf_file.is_open()) {
		LOG(ERROR) << "Failed to open file for writing: " << cdf_filename;
	} else {
		cdf_file << "Latency_us,CumulativeProbability,Metric\n";
		if (have_submit_metric) {
			for (size_t i = 0; i < submit_to_ack_summary.count; ++i) {
				const long long current_latency = submit_to_ack_latencies_copy[i];
				const double cumulative_probability = static_cast<double>(i + 1) / submit_to_ack_summary.count;
				cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability
				         << ",append_submit_to_ack_batch_latency\n";
			}
		}
		if (have_send_metric) {
			for (size_t i = 0; i < send_to_ack_summary.count; ++i) {
				const long long current_latency = send_to_ack_latencies_copy[i];
				const double cumulative_probability = static_cast<double>(i + 1) / send_to_ack_summary.count;
				cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability
				         << ",append_send_to_ack_batch_latency\n";
			}
		}
		if (have_ordered_metric) {
			for (size_t i = 0; i < send_to_ordered_summary.count; ++i) {
				const long long current_latency = send_to_ack_latencies_copy[i];
				const double cumulative_probability = static_cast<double>(i + 1) / send_to_ordered_summary.count;
				cdf_file << current_latency << "," << std::fixed << std::setprecision(8) << cumulative_probability
				         << ",append_send_to_ordered_batch_latency\n";
			}
		}
		cdf_file.close();
	}
}
#endif


bool Publisher::Init(int ack_level) {
	ack_level_ = ack_level;

	const auto& runtime_cfg = Embarcadero::GetConfig().config().client.runtime;
	runtime_mode_ = Embarcadero::GetConfig().getRuntimeMode();
	if (runtime_mode_ == "failure") {
		ack_drain_ms_success_ = runtime_cfg.ack_drain_ms_failure.get();
		ack_timeout_seconds_ = runtime_cfg.ack_timeout_sec_failure.get();
		epoll_wait_writable_ms_ = runtime_cfg.epoll_wait_writable_ms_failure.get();
	} else if (runtime_mode_ == "latency") {
		ack_drain_ms_success_ = runtime_cfg.ack_drain_ms_latency.get();
		ack_timeout_seconds_ = runtime_cfg.ack_timeout_sec_latency.get();
		epoll_wait_writable_ms_ = runtime_cfg.epoll_wait_writable_ms_latency.get();
	} else {
		ack_drain_ms_success_ = runtime_cfg.ack_drain_ms_throughput.get();
		ack_timeout_seconds_ = runtime_cfg.ack_timeout_sec_throughput.get();
		epoll_wait_writable_ms_ = runtime_cfg.epoll_wait_writable_ms_throughput.get();
	}
	ack_drain_ms_failure_ = runtime_cfg.ack_drain_ms_failure.get();

	// Backward-compatible global override.
	if (const char* drain_env = std::getenv("EMBARCADERO_ACK_DRAIN_MS")) {
		int drain_ms = std::atoi(drain_env);
		ack_drain_ms_success_ = drain_ms;
		ack_drain_ms_failure_ = drain_ms;
	}
	if (const char* timeout_env = std::getenv("EMBARCADERO_ACK_TIMEOUT_SEC")) {
		ack_timeout_seconds_ = std::atoi(timeout_env);
	}
	if (const char* epoll_wait_env = std::getenv("EMBARCADERO_EPOLL_WAIT_WRITABLE_MS")) {
		epoll_wait_writable_ms_ = std::atoi(epoll_wait_env);
	}
	if (epoll_wait_writable_ms_ < 0) {
		epoll_wait_writable_ms_ = 0;
	}

	LOG(INFO) << "Publisher runtime mode=" << runtime_mode_
	          << " ack_drain_ms_success=" << ack_drain_ms_success_
	          << " ack_drain_ms_failure=" << ack_drain_ms_failure_
	          << " ack_timeout_sec=" << ack_timeout_seconds_
	          << " epoll_wait_writable_ms=" << epoll_wait_writable_ms_;
	unacked_byte_cap_ = ComputeUnackedByteCap();
	LOG(INFO) << "Publisher session lease_ns=" << SessionLeaseNs()
	          << " unacked_byte_cap_pre_pool=" << unacked_byte_cap_
	          << " ack_level=" << ack_level_
	          << " memory_emulated_ack2=" << (IsMemoryEmulatedAck2() ? 1 : 0)
	          << " unacked_retention="
	          << (Ack2UsesOwnedRtoCopy() ? "owned_rto_copy" : "pool_pin");

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
		if (IsOrder5SessionMode()) {
			retransmit_thread_ = std::thread([this]() {
					this->RetransmitThread();
					});
		}

		// Wait for acknowledgment thread to initialize (with timeout  EpollAckThread may fail to start)
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

	// Allocate the publish batch pool on this thread before SubscribeToCluster.
	// Historically AddBuffers ran inside the gRPC callback and a multi-tens-of-GB
	// mmap blocked connected_=true past this 60s wait (false "gRPC failing" log).
	{
		size_t qsize = 0;
		{
			absl::MutexLock lock(&mutex_);
			qsize = queueSize_;
		}
		// Memory-emulated ACK2 pool-pin needs pool depth ≈ ACK BDP, not just
		// send-pipeline slots; otherwise credit collapses to ~pipeline size.
		if (IsMemoryEmulatedAck2() && !Ack2UsesOwnedRtoCopy()) {
			qsize = std::max(qsize, ComputeUnackedByteCap());
		}
		if (!pubQue_.AddBuffers(qsize)) {
			LOG(ERROR) << "Publisher::Init() failed to allocate publish queue buffers "
			           << "(qsize_hint=" << qsize << ")";
			return false;
		}
		ApplyUnackedByteCapBounds();
		LOG(INFO) << "Publisher unacked_byte_cap=" << unacked_byte_cap_
		          << " pool_bytes=" << pubQue_.PoolBytes()
		          << " unacked_retention="
		          << (Ack2UsesOwnedRtoCopy() ? "owned_rto_copy" : "pool_pin");
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
		return false;
	}

	// Initialize Corfu sequencer if needed
	if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
		corfu_client_ = std::make_unique<CorfuSequencerClient>(static_cast<uint64_t>(client_id_));
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

	// ORDER=5 per-session FIFO + preferred striping: partial connect leaves dead
	// preferred queues that swallow batch_seq and create permanent head gaps
	// (N=2 pilot: 8/24 threads → fence storm). Refuse to publish in that state.
	if (IsOrder5SessionMode()) {
		const int ready = thread_count_.load(std::memory_order_acquire);
		const int expected = num_threads_.load(std::memory_order_acquire);
		if (ready < expected || expected <= 0) {
			LOG(ERROR) << "Publisher::Init() ORDER=5 requires all publish threads connected; "
			           << "got thread_count_=" << ready << " num_threads_=" << expected
			           << ". Refusing to publish into dead preferred queues.";
			return false;
		}
		{
			absl::MutexLock lock(&mutex_);
			RefreshOrder5PreferredQueuesLocked();
		}
	}

	// [[FIX: B3=0 ACKs]] Wait for all expected broker ACK connections to be established
	// This prevents the race where publishing completes before all ACK connections are up
	if (ack_level_ >= 1) {
		constexpr auto ACK_CONNECTION_TIMEOUT = std::chrono::seconds(30);
		// Small debounce so staged cluster-status updates do not race ACK readiness,
		// without imposing a fixed quarter-second startup penalty on every run.
		constexpr auto ACK_EXPECTED_STABLE_WINDOW = std::chrono::milliseconds(25);
		auto ack_wait_start = std::chrono::steady_clock::now();
		auto last_log_time = ack_wait_start;

		while (true) {
			const int expected = expected_ack_brokers_.load(std::memory_order_acquire);
			const int64_t last_expected_update_ns =
				expected_ack_brokers_last_update_ns_.load(std::memory_order_acquire);
			int connected_count;
			{
				absl::MutexLock lock(&mutex_);
				connected_count = static_cast<int>(brokers_with_ack_connection_.size());
			}

			const bool expected_is_stable =
				last_expected_update_ns > 0 &&
				(SteadyNowNs() - last_expected_update_ns) >=
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						ACK_EXPECTED_STABLE_WINDOW).count();

			if (expected > 0 && connected_count >= expected && expected_is_stable) {
				VLOG(1) << "Publisher::Init() All " << expected << " broker ACK connections established";
				break;
			}

			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - ack_wait_start);

			// Log progress every 2 seconds
			if (now - last_log_time >= std::chrono::seconds(2)) {
				VLOG(1) << "Publisher::Init() Waiting for broker ACK connections: "
				         << connected_count << " / " << expected
				         << " (elapsed: " << elapsed.count() << "s, expected_stable="
				         << (expected_is_stable ? "yes" : "no") << ")";
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

	if (runtime_mode_ == "throughput") {
		StartThroughputTimeseriesIfEnabled();
	}
	return true;
}

void Publisher::StartThroughputTimeseriesIfEnabled() {
	if (real_time_throughput_measure_thread_.joinable()) {
		return;
	}
	const std::string out_file = GetThroughputTimeseriesFilePath();
	if (out_file.empty()) {
		return;
	}

	const int interval_ms = GetThroughputTimeseriesIntervalMs();
	const size_t num_brokers = static_cast<size_t>(
		(expected_num_brokers_ > 0) ? expected_num_brokers_ : NUM_MAX_BROKERS);
	measure_real_time_throughput_ = true;

	real_time_throughput_measure_thread_ = std::thread([this, out_file, interval_ms, num_brokers]() {
		std::ofstream throughput_file(out_file);
		if (!throughput_file.is_open()) {
			LOG(ERROR) << "Failed to open throughput timeseries file: " << out_file;
			return;
		}

		throughput_file << "Timestamp(ms)";
		for (size_t i = 0; i < num_brokers; ++i) {
			throughput_file << ",Broker_" << i << "_sent_GiBps"
			                << ",Broker_" << i << "_ack_GiBps";
		}
		throughput_file << ",Sent_GiBps,Ack_GiBps,Total_GBps"
		                << ",Cum_Sent_Bytes,Cum_Ack_Bytes\n";

		std::vector<size_t> prev_acked_bytes(num_brokers, 0);
		std::vector<size_t> prev_sent_bytes(num_brokers, 0);
		constexpr double kGiBDivisor = 1024.0 * 1024.0 * 1024.0;
		int drain_remaining = -1;
		auto prev_time = std::chrono::steady_clock::now();
		int64_t shared_origin_ms = 0;
		const bool has_shared_origin = GetThroughputTimeseriesOriginMs(shared_origin_ms);

		while (!shutdown_.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
			auto now = std::chrono::steady_clock::now();
			double elapsed_sec = std::chrono::duration<double>(now - prev_time).count();
			if (elapsed_sec <= 0.0) elapsed_sec = interval_ms / 1000.0;
			prev_time = now;

			int64_t timestamp_ms = 0;
			if (has_shared_origin) {
				const int64_t now_epoch_ms = static_cast<int64_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::system_clock::now().time_since_epoch()).count());
				timestamp_ms = now_epoch_ms - shared_origin_ms;
				if (timestamp_ms < 0) timestamp_ms = 0;
			} else {
				timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					now - start_time_).count();
			}
			throughput_file << timestamp_ms;

			size_t total_sent_delta = 0;
			size_t total_ack_delta = 0;
			size_t cum_sent = 0;
			size_t cum_ack = 0;
			for (size_t i = 0; i < num_brokers; ++i) {
				const size_t acked_bytes =
					broker_stats_[i].acked_messages.load(std::memory_order_relaxed) * message_size_;
				const size_t sent_bytes =
					broker_stats_[i].sent_messages.load(std::memory_order_relaxed) * message_size_;
				const size_t sent_delta = sent_bytes - prev_sent_bytes[i];
				const size_t ack_delta = acked_bytes - prev_acked_bytes[i];
				const double sent_gibps = (static_cast<double>(sent_delta) / elapsed_sec) / kGiBDivisor;
				const double ack_gibps = (static_cast<double>(ack_delta) / elapsed_sec) / kGiBDivisor;
				throughput_file << "," << sent_gibps << "," << ack_gibps;
				total_sent_delta += sent_delta;
				total_ack_delta += ack_delta;
				cum_sent += sent_bytes;
				cum_ack += acked_bytes;
				prev_acked_bytes[i] = acked_bytes;
				prev_sent_bytes[i] = sent_bytes;
			}

			const double sent_total =
				(static_cast<double>(total_sent_delta) / elapsed_sec) / kGiBDivisor;
			const double ack_total =
				(static_cast<double>(total_ack_delta) / elapsed_sec) / kGiBDivisor;
			// Total_GBps retained for harness compatibility; prefer Ack_GiBps for ACK runs
			// and Sent_GiBps for publish-path stalls. Name kept for existing analyzers.
			throughput_file << "," << sent_total << "," << ack_total << "," << ack_total
			                << "," << cum_sent << "," << cum_ack << "\n";

			// Keep sampling through Poll()/ACK drain so post-kill recovery is visible.
			if (shutdown_.load(std::memory_order_relaxed)) {
				break;
			}
			if (publish_finished_.load(std::memory_order_relaxed)) {
				if (drain_remaining < 0) {
					drain_remaining = 300;  // ~3s multi-layer drain window
				}
				if (--drain_remaining <= 0) break;
			}
		}

		throughput_file.flush();
		throughput_file.close();
		LOG(INFO) << "Wrote throughput timeseries to " << out_file;
	});
}

void Publisher::WarmupBuffers() {
	// Delegate to the Buffer class which has access to private members
	pubQue_.WarmupBuffers();
}

void Publisher::Publish(char* message, size_t len) {
	// [[FENCE_REOPEN_SPIN_BOUND 2026-07-11]] This wait was unbounded: if the
	// session reopen never completes (e.g. every publish thread died in a
	// multi-client connect storm), the caller spun silently at 100% CPU with
	// no log output — observed for 7 h in run 20260711T065047Z. Bound it and
	// fail loudly instead. Cold path: only entered while a fence is pending.
	if (session_fenced_reopen_pending_.load(std::memory_order_acquire)) {
		const auto reopen_wait_start = std::chrono::steady_clock::now();
		while (session_fenced_reopen_pending_.load(std::memory_order_acquire) &&
		       !shutdown_.load(std::memory_order_relaxed)) {
			Embarcadero::CXL::cpu_pause();
			if (std::chrono::steady_clock::now() - reopen_wait_start >
			    std::chrono::seconds(120)) {
				LOG(ERROR) << "Publish(): session reopen pending for >120s — "
				           << "session is dead; failing the run instead of spinning";
				shutdown_.store(true, std::memory_order_relaxed);
				return;
			}
		}
	}
	constexpr size_t kHeaderSize = sizeof(Embarcadero::MessageHeader);
	// Branchless 64-byte payload alignment for the hot path.
	const size_t padded_total = ((len + 63) & ~static_cast<size_t>(63)) + kHeaderSize;

	// [[PERF]] Per-message order for header only (subscriber ordering). client_order_ updated per batch when sealed.
	size_t my_order = next_publish_order_++;
	// [[TAIL_SEAL_FIX 2026-07-12]] Never silently drop a message on transient buffer
	// backpressure. A dropped message means client_order_ can never reach the Poll
	// target n, so the drain wedges until the 300s timeout and the run fails even
	// though everything sent was acked (observed at N=3 full-stripe and in E4a
	// broker-kill: client_order_ stuck a few hundred short of target). Instead:
	//   - always credit any batch that was sealed+pushed before the failure (Write now
	//     surfaces its count via sealed even on the early-return path), and
	//   - retry THIS message under backpressure until a buffer frees (an ack retires an
	//     unacked batch retires credit -> WaitForUnackedCapacity unblocks; pool
	//     slots recycle on ReleaseBatch after send). Bail on shutdown; fail loudly if
	//     the unacked prefix never retires rather than hanging or dropping.
	static const int kBackpressureTimeoutSec = [] {
		if (const char* e = std::getenv("EMBARCADERO_PUBLISH_BACKPRESSURE_TIMEOUT_SEC")) {
			int v = std::atoi(e);
			if (v > 0) return v;
		}
		return 120;
	}();
	const auto publish_start = std::chrono::steady_clock::now();
	for (;;) {
		size_t sealed = 0;
		const bool ok = pubQue_.Write(my_order, message, len, padded_total, sealed);
		if (sealed > 0) {
			client_order_.fetch_add(sealed, std::memory_order_release);
		}
		if (ok) return;
		if (shutdown_.load(std::memory_order_relaxed)) return;
		if (std::chrono::steady_clock::now() - publish_start >
		    std::chrono::seconds(kBackpressureTimeoutSec)) {
			LOG(ERROR) << "Publish(): buffer backpressure for >" << kBackpressureTimeoutSec
			           << "s (client_order=" << my_order
			           << ") — unacked prefix not retiring; failing run instead of dropping";
			shutdown_.store(true, std::memory_order_relaxed);
			return;
		}
		std::this_thread::yield();
	}
}

bool Publisher::Poll(size_t n, bool include_tail_drain) {
	const bool ack_jitter_trace = (std::getenv("EMBARCADERO_ACK_JITTER_TRACE") != nullptr);
	const auto poll_start_time = std::chrono::steady_clock::now();
	auto queue_drain_done_time = poll_start_time;
	auto publisher_join_done_time = poll_start_time;
	auto ack_wait_start_time = poll_start_time;
	auto ack_wait_done_time = poll_start_time;
	bool ack_wait_measured = false;
	size_t poll_target_acks = 0;
	size_t poll_normalized_received = 0;
	// [[LAST_PERCENT_ACK_FIX]] Seal and return reads before signaling finished.
	// If we set publish_finished_ first, threads that get nullptr from Read() may exit
	// before we've called SealAll(), dropping the last batches.
	WriteFinishedOrPaused();
	// Enter queue-drain mode. Publish threads should keep consuming queued batches
	// until empty, then exit; do not force shutdown here.
	pubQue_.WriteFinished();

	// ACK2: suppress RTO / futile identical-fence storms from publish_finished
	// through ACK wait (covers queue-drain + publisher join). Cleared on all
	// Poll exit paths via this guard.
	struct AckDrainGuard {
		Publisher* self;
		bool armed{false};
		AckDrainGuard(Publisher* p, bool arm) : self(p), armed(arm) {
			if (armed) {
				self->ack_drain_active_.store(true, std::memory_order_release);
			}
		}
		~AckDrainGuard() {
			if (armed) {
				self->ack_drain_active_.store(false, std::memory_order_release);
			}
		}
	} ack_drain_guard{this, ack_level_ >= 2};

	// Signal threads before releasing queue resources [[RELAXED]]
	publish_finished_.store(true, std::memory_order_relaxed);
	consumer_should_exit_.store(true, std::memory_order_relaxed);

	const bool low_payload_poll_mode = (n <= 1000000);
	const auto queue_spin_duration = low_payload_poll_mode
		? std::chrono::microseconds(100)
		: std::chrono::milliseconds(1);
	// [[DRAIN_HANG_FIX 2026-07-11]] Bounded drain wait + periodic re-seal.
	// Observed (e3 nolinger cell, run 20260711T003924Z): client_order_ wedged
	// 26 messages short of target for 38+ min — a partial tail batch missed by
	// the initial WriteFinishedOrPaused() seal pass never seals again, and this
	// loop had no timeout. Re-run the seal pass periodically (idempotent; only
	// newly sealed messages increment client_order_) and fail the Poll after a
	// bounded wait instead of spinning forever.
	const int drain_timeout_sec = [] {
		if (const char* env = std::getenv("EMBARCADERO_QUEUE_DRAIN_TIMEOUT_SEC")) {
			int v = std::atoi(env);
			if (v > 0) return v;
		}
		return 300;
	}();
	const auto queue_wait_start = std::chrono::steady_clock::now();
	auto last_queue_log_time = queue_wait_start;
	auto last_reseal_time = queue_wait_start;
	uint32_t queue_wait_loops = 0;
	while (client_order_.load(std::memory_order_acquire) < n) {
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_reseal_time).count() >= 5) {
			last_reseal_time = now;
			const size_t before_reseal = client_order_.load(std::memory_order_acquire);
			WriteFinishedOrPaused();
			const size_t after_reseal = client_order_.load(std::memory_order_acquire);
			if (after_reseal > before_reseal) {
				LOG(WARNING) << "[Publisher Queue Drain Wait] late re-seal recovered "
				             << (after_reseal - before_reseal)
				             << " message(s) — tail-batch seal race hit; continuing";
			}
		}
		if (std::chrono::duration_cast<std::chrono::seconds>(now - queue_wait_start).count() >= drain_timeout_sec) {
			LOG(ERROR) << "[Publisher Queue Drain Wait] TIMEOUT after " << drain_timeout_sec
			           << "s: client_order=" << client_order_.load(std::memory_order_acquire)
			           << " target=" << n
			           << " — failing Poll instead of hanging (EMBARCADERO_QUEUE_DRAIN_TIMEOUT_SEC to tune)";
			return false;
		}
		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_queue_log_time).count() >= 1) {
			std::string per_broker;
			for (size_t i = 0; i < broker_stats_.size(); i++) {
				if (i) per_broker += " ";
				per_broker += "B" + std::to_string(i)
					+ "(sent=" + std::to_string(broker_stats_[i].sent_messages.load(std::memory_order_relaxed))
					+ ",acked=" + std::to_string(broker_stats_[i].acked_messages.load(std::memory_order_relaxed))
					+ ")";
			}
			LOG(WARNING) << "[Publisher Queue Drain Wait] client_order="
			             << client_order_.load(std::memory_order_relaxed)
			             << " target=" << n
			             << " total_batches_sent=" << total_batches_sent_.load(std::memory_order_relaxed)
			             << " total_messages_sent=" << total_messages_sent_.load(std::memory_order_relaxed)
			             << " total_batches_attempted=" << total_batches_attempted_.load(std::memory_order_relaxed)
			             << " total_batches_failed=" << total_batches_failed_.load(std::memory_order_relaxed)
			             << " elapsed_s="
			             << std::chrono::duration_cast<std::chrono::seconds>(now - queue_wait_start).count()
			             << " [" << per_broker << "]";
			last_queue_log_time = now;
		}
		auto spin_start = std::chrono::steady_clock::now();
		const auto spin_end = spin_start + queue_spin_duration;
		while (std::chrono::steady_clock::now() < spin_end && client_order_.load(std::memory_order_acquire) < n) {
			Embarcadero::CXL::cpu_pause();
		}
		if (client_order_.load(std::memory_order_acquire) < n) {
			if (!low_payload_poll_mode || ((++queue_wait_loops & 0x3F) == 0)) {
				std::this_thread::yield();
			}
		}
	}
	queue_drain_done_time = std::chrono::steady_clock::now();

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
		publisher_join_done_time = std::chrono::steady_clock::now();
		const size_t zero_batch_threads = zero_batch_publish_threads_.load(std::memory_order_relaxed);
		if (zero_batch_threads > 0) {
			LOG(WARNING) << "[Publisher Thread Distribution] " << zero_batch_threads
			             << " publish thread(s) exited without sending a batch. "
			             << "This can occur with skewed queue/thread assignment and is not a failure by itself.";
		}

		// If acknowledgments are enabled, wait for all acks
		if (ack_level_ >= 1) {
		auto wait_start_time = std::chrono::steady_clock::now();
		ack_wait_start_time = wait_start_time;
		ack_wait_done_time = wait_start_time;
		ack_wait_measured = true;
		auto last_log_time = wait_start_time;
		auto last_ack_change_time = wait_start_time;
		auto normalized_acks = [&]() -> size_t {
			// ORDER=5 ACK1/ACK2 are head-owned on broker 0. The broker emits a single
			// per-client frontier for the whole client stream, not one frontier per
			// routed broker. Summing min(sent_i, acked_i) therefore collapses a
			// correct global ACK stream down to one broker's quota.
			if (seq_type_ == heartbeat_system::SequencerType::EMBARCADERO &&
			    order_level_ == Embarcadero::kOrderStrong &&
			    (ack_level_ == 1 || ack_level_ == 2) &&
			    !broker_stats_.empty()) {
				return broker_stats_[0].acked_messages.load(std::memory_order_relaxed);
			}
			size_t total = 0;
			for (size_t i = 0; i < broker_stats_.size(); i++) {
				const size_t sent = broker_stats_[i].sent_messages.load(std::memory_order_relaxed);
				const size_t acked = broker_stats_[i].acked_messages.load(std::memory_order_relaxed);
				total += std::min(sent, acked);
			}
			return total;
		};
		size_t last_ack_val = normalized_acks();
		// Keep low-payload tail latency tight while preserving large-payload behavior.
		const auto ack_spin_duration = low_payload_poll_mode
			? std::chrono::microseconds(100)
			: std::chrono::microseconds(500);
		uint32_t ack_wait_loops = 0;
		
		// Configurable timeout for ACK waits. Runtime policy resolved once in Init().
		int timeout_seconds = ack_timeout_seconds_;
		const auto timeout_duration = std::chrono::seconds(timeout_seconds);
		// [[FIX: ACK Race Condition]] Capture target ONCE - never reload inside loop
		// Reloading allowed concurrent Publish() calls to move the target, causing potential infinite wait
		const size_t target_acks = client_order_.load(std::memory_order_acquire);
		poll_target_acks = target_acks;

		// Need to check for test completion/shutdown condition inside this loop to avoid hanging if things fail
	while (normalized_acks() < target_acks && !shutdown_.load(std::memory_order_relaxed)) {
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - wait_start_time);
			const size_t current_raw_acks = ack_received_.load(std::memory_order_acquire);
			const size_t current_normalized_acks = normalized_acks();

			// Check timeout
			if (timeout_seconds > 0 && elapsed >= timeout_duration) {
				LOG(ERROR) << "[Publisher ACK Timeout]: Waited " << elapsed.count()
					<< " seconds for ACKs, normalized_received=" << current_normalized_acks
					<< " raw_received=" << current_raw_acks
					<< " out of " << target_acks
					<< " (timeout=" << timeout_seconds << "s)";
				LOG(ERROR) << "[Publisher ACK Diagnostics]: ack_level=" << ack_level_
					<< ", last_normalized_ack_received=" << current_normalized_acks
					<< ", last_raw_ack_received=" << current_raw_acks
					<< ", client_order=" << target_acks;
				LOG(ERROR) << "[Publisher Batch Stats]: total_batches_sent=" << total_batches_sent_.load(std::memory_order_relaxed)
					<< " attempted=" << total_batches_attempted_.load(std::memory_order_relaxed)
					<< " failed=" << total_batches_failed_.load(std::memory_order_relaxed)
					<< " (sent_all=" << (total_batches_failed_.load(std::memory_order_relaxed) == 0 ? "yes" : "no") << ")";
				// Per-broker counts to pinpoint which broker(s) are short (sent vs acked)
				std::string per_broker;
				for (size_t i = 0; i < broker_stats_.size(); i++) {
					size_t sent = broker_stats_[i].sent_messages.load(std::memory_order_relaxed);
					size_t acked = broker_stats_[i].acked_messages.load(std::memory_order_relaxed);
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
				if (kill_brokers_) {
					LOG(INFO) << "[Publisher ACK]: Timeout allowed due to killed brokers. Treating as success to gather stats.";
					int drain_ms = ack_drain_ms_failure_;
					if (include_tail_drain && drain_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(drain_ms));
					return true;
				}
				return false;  // Exit early on timeout
			}

			size_t current_acks = current_normalized_acks;
			if (current_acks > last_ack_val) {
				last_ack_val = current_acks;
				last_ack_change_time = now;
			}

			if (kill_brokers_) {
				if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ack_change_time).count() >= 5) { // increased to 5 seconds
					LOG(INFO) << "[Publisher ACK]: No new ACKs for 5s after broker kill. Assuming remaining " << (target_acks - current_acks) << " messages were lost in flight.";
					break;
				}
			}
			if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 1) {
				std::string per_broker;
				for (size_t i = 0; i < broker_stats_.size(); i++) {
					if (i) per_broker += " ";
					per_broker += "B" + std::to_string(i) + "=" + std::to_string(broker_stats_[i].acked_messages.load(std::memory_order_relaxed));
				}
				VLOG(1) << "Waiting for acknowledgments, normalized_received " << current_normalized_acks
					<< " raw_received " << current_raw_acks << " out of " << target_acks
					<< " (elapsed: " << elapsed.count() << "s"
					<< (timeout_seconds > 0 ? ", timeout: " + std::to_string(timeout_seconds) + "s" : "") << ") [" << per_broker << "]";
				last_log_time = now;
			}

			// [[REMOVED: co = client_order_.load()]] - This caused the race condition!
			auto spin_start = std::chrono::steady_clock::now();
			const auto spin_end = spin_start + ack_spin_duration;
			while (std::chrono::steady_clock::now() < spin_end && normalized_acks() < target_acks) {
				Embarcadero::CXL::cpu_pause();
			}
			if (normalized_acks() < target_acks) {
				if (!low_payload_poll_mode || ((++ack_wait_loops & 0x3F) == 0)) {
					std::this_thread::yield();
				}
		}
	}
		// Only treat as success if we actually received ACKs for all messages
		const size_t received = ack_received_.load(std::memory_order_relaxed);
		const size_t normalized_received = normalized_acks();
			poll_normalized_received = normalized_received;
			if (received > target_acks) {
				LOG(WARNING) << "[Publisher ACK Normalize]: raw_received="
				             << received << " target=" << target_acks
				             << " excess=" << (received - target_acks)
				             << " normalized_received=" << normalized_received;
				std::string per_broker;
				for (size_t i = 0; i < broker_stats_.size(); i++) {
					size_t sent = broker_stats_[i].sent_messages.load(std::memory_order_relaxed);
					size_t acked = broker_stats_[i].acked_messages.load(std::memory_order_relaxed);
					if (i) per_broker += " ";
					per_broker += "B" + std::to_string(i) + "=" + std::to_string(acked);
					if (sent != 0 || acked != 0) {
						per_broker += "(sent=" + std::to_string(sent);
						if (acked > sent) per_broker += ",excess=" + std::to_string(acked - sent);
						else if (sent > acked) per_broker += ",short=" + std::to_string(sent - acked);
						per_broker += ")";
					}
				}
				LOG(WARNING) << "[Publisher ACK Per-Broker]: " << per_broker;
			}
			if (normalized_received < target_acks) {
				if (kill_brokers_) {
					LOG(INFO) << "[Publisher ACK]: Allowed shortfall due to killed brokers. normalized_received="
					          << normalized_received << " target=" << target_acks;
					// Clean up resources since test passes
					int drain_ms = ack_drain_ms_failure_;
					if (include_tail_drain && drain_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(drain_ms));
					return true;
				}
			LOG(ERROR) << "[Publisher ACK Failure]: Did not receive ACKs for all messages. normalized_received="
			           << normalized_received << " raw_received=" << received
			           << " target=" << target_acks << " short=" << (target_acks - normalized_received);
			std::string per_broker;
			for (size_t i = 0; i < broker_stats_.size(); i++) {
				size_t sent = broker_stats_[i].sent_messages.load(std::memory_order_relaxed);
				size_t acked = broker_stats_[i].acked_messages.load(std::memory_order_relaxed);
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
			ack_wait_done_time = std::chrono::steady_clock::now();
			LOG(INFO) << "[ACK_VERIFY] normalized_received=" << normalized_received
			          << " raw_received=" << received
			          << " target=" << target_acks << " 100%";
			if (IsOrder5SessionMode() && ack_level_ >= 1) {
				size_t unacked_bytes = 0;
				size_t unacked_batches = 0;
				size_t session_sent_hwm = 0;
				{
					std::lock_guard<std::mutex> lock(unacked_mu_);
					unacked_bytes = unacked_bytes_;
					unacked_batches = unacked_batches_.size();
					session_sent_hwm = session_sent_hwm_;
				}
				LOG(INFO) << "[UNACKED_DRAIN]"
				          << " bytes=" << unacked_bytes
				          << " batches=" << unacked_batches
				          << " session_sent_hwm=" << session_sent_hwm
				          << " ack_base=" << ack_message_base_.load(std::memory_order_acquire);
			}
			if (ack_jitter_trace) {
				const auto ack_wait_ms = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - wait_start_time).count() / 1000.0;
				LOG(INFO) << "[POLL_ACK_TRACE] target=" << target_acks
				          << " normalized_received=" << normalized_received
				          << " raw_received=" << received
				          << " wait_ms=" << ack_wait_ms
				          << " wait_loops=" << ack_wait_loops
				          << " low_payload_mode=" << (low_payload_poll_mode ? 1 : 0);
			}
			// [[ORDER_0_TAIL_ACK]] Drain so EpollAckThread can read in-flight ACKs before we return.
			int drain_ms = ack_drain_ms_success_;
				if (include_tail_drain && drain_ms > 0) {
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
	// NOTE: We do NOT set shutdown_=true or cancel context here
	// This allows the subscriber to continue using the cluster management infrastructure
	// The context will be cleaned up when the Publisher object is destroyed
	LogOrder5RoutingSummary();
	const auto poll_done_time = std::chrono::steady_clock::now();
	LOG(INFO) << "[POLL_BREAKDOWN] target_messages=" << n
	          << " ack_enabled=" << (ack_level_ >= 1 ? 1 : 0)
	          << " queue_drain_ms=" << std::fixed << std::setprecision(3)
	          << DurationMs(poll_start_time, queue_drain_done_time)
	          << " publisher_join_ms=" << DurationMs(queue_drain_done_time, publisher_join_done_time)
	          << " ack_wait_ms=" << (ack_wait_measured ? DurationMs(ack_wait_start_time, ack_wait_done_time) : 0.0)
	          << " post_ack_ms=" << (ack_wait_measured ? DurationMs(ack_wait_done_time, poll_done_time) : 0.0)
	          << " total_poll_ms=" << DurationMs(poll_start_time, poll_done_time)
	          << " target_acks=" << poll_target_acks
	          << " normalized_acks=" << poll_normalized_received;
	return true;
}

bool Publisher::WaitUntilAcked(size_t n) {
	if (ack_level_ < 1) {
		return true;
	}

	const bool low_payload_poll_mode = (n <= 1000000);
	auto wait_start_time = std::chrono::steady_clock::now();
	auto last_log_time = wait_start_time;
	auto last_ack_change_time = wait_start_time;
	auto normalized_acks = [&]() -> size_t {
		size_t total = 0;
		for (size_t i = 0; i < broker_stats_.size(); i++) {
			const size_t sent = broker_stats_[i].sent_messages.load(std::memory_order_relaxed);
			const size_t acked = broker_stats_[i].acked_messages.load(std::memory_order_relaxed);
			total += std::min(sent, acked);
		}
		return total;
	};
	size_t last_ack_val = normalized_acks();
	const auto ack_spin_duration = low_payload_poll_mode
		? std::chrono::microseconds(100)
		: std::chrono::microseconds(500);
	uint32_t ack_wait_loops = 0;
	int timeout_seconds = ack_timeout_seconds_;
	const auto timeout_duration = std::chrono::seconds(timeout_seconds);

	while (normalized_acks() < n && !shutdown_.load(std::memory_order_relaxed)) {
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - wait_start_time);
		const size_t current_raw_acks = ack_received_.load(std::memory_order_acquire);
		const size_t current_normalized_acks = normalized_acks();

		if (timeout_seconds > 0 && elapsed >= timeout_duration) {
			LOG(ERROR) << "[Publisher ACK Timeout]: Waited " << elapsed.count()
			           << " seconds for ACKs, normalized_received=" << current_normalized_acks
			           << " raw_received=" << current_raw_acks
			           << " out of " << n
			           << " (timeout=" << timeout_seconds << "s)";
			LOG(ERROR) << "[Publisher ACK Diagnostics]: ack_level=" << ack_level_
			           << ", last_normalized_ack_received=" << current_normalized_acks
			           << ", last_raw_ack_received=" << current_raw_acks
			           << ", target=" << n;
			return false;
		}

		if (current_normalized_acks > last_ack_val) {
			last_ack_val = current_normalized_acks;
			last_ack_change_time = now;
		}

		if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 1) {
			std::string per_broker;
			for (size_t i = 0; i < broker_stats_.size(); i++) {
				if (i) per_broker += " ";
				per_broker += "B" + std::to_string(i)
					+ "(sent=" + std::to_string(broker_stats_[i].sent_messages.load(std::memory_order_relaxed))
					+ ",acked=" + std::to_string(broker_stats_[i].acked_messages.load(std::memory_order_relaxed))
					+ ")";
			}
			VLOG(1) << "Waiting for acknowledgments, normalized_received "
			        << current_normalized_acks << " raw_received " << current_raw_acks
			        << " out of " << n << " (elapsed: " << elapsed.count()
			        << "s, idle_s="
			        << std::chrono::duration_cast<std::chrono::seconds>(now - last_ack_change_time).count()
			        << ") [" << per_broker << "]";
			last_log_time = now;
		}

		auto spin_start = std::chrono::steady_clock::now();
		const auto spin_end = spin_start + ack_spin_duration;
		while (std::chrono::steady_clock::now() < spin_end && normalized_acks() < n) {
			Embarcadero::CXL::cpu_pause();
		}
		if (normalized_acks() < n) {
			if (!low_payload_poll_mode || ((++ack_wait_loops & 0x3F) == 0)) {
				std::this_thread::yield();
			}
		}
	}

	return normalized_acks() >= n;
}

void Publisher::DEBUG_check_send_finish() {
	WriteFinishedOrPaused();
	pubQue_.ReturnReads();
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
		broker_stats_[i].acked_messages.store(0, std::memory_order_relaxed);
	}

	// Start thread to monitor progress and kill brokers at specified percentage
	kill_brokers_thread_ = std::thread([=, this]() {
		size_t bytes_to_kill_brokers = total_message_size * failure_percentage;

		while (!shutdown_.load(std::memory_order_relaxed) && !publish_finished_.load(std::memory_order_relaxed) && total_sent_bytes_.load(std::memory_order_acquire) < bytes_to_kill_brokers) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Was 10ms, use 1ms for faster detection
		}

		if (!shutdown_.load(std::memory_order_relaxed)) {
			size_t sent_at_trigger = total_sent_bytes_.load(std::memory_order_acquire);
			RecordFailureEvent("Failure threshold reached (sent frontier)");
			LOG(INFO) << "Triggering broker kill at " << sent_at_trigger << " bytes sent";
			RecordFailureEvent("Broker kill requested (gRPC)");
			killbrokers();
			throttle_relaxed_.store(true, std::memory_order_release);
		}
	});

	// Start thread to measure real-time throughput
	real_time_throughput_measure_thread_ = std::thread([=, this]() {
		std::vector<size_t> prev_throughputs(num_brokers, 0);

		// Open file for writing throughput data. Prefer EMBARCADERO_FAILURE_DATA_DIR so run_failures.sh can place output in project data dir.
		const char* failure_dir = std::getenv("EMBARCADERO_FAILURE_DATA_DIR");
		std::string dir;
		if (failure_dir && failure_dir[0]) {
			dir = failure_dir;
		} else {
			const char* home = std::getenv("HOME");
			dir = (home && home[0]) ? home : ".";
			dir += "/Embarcadero/data/failure";
		}
		std::string filename = dir + "/real_time_acked_throughput.csv";
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

		const int kTargetIntervalMs = GetFailureMeasureIntervalMs();
		constexpr double kGBDivisor = 1024.0 * 1024.0 * 1024.0;
		constexpr int kDrainIntervalsAfterFinish = 30;  // 3s of trailing measurements after publish finishes
		const size_t ack_bytes_to_kill_brokers = total_message_size * failure_percentage;
		auto prev_time = std::chrono::steady_clock::now();
		int drain_remaining = -1;  // -1 = not draining yet
		bool ack_frontier_recorded = false;

		while (!shutdown_.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(kTargetIntervalMs));
			auto now = std::chrono::steady_clock::now();
			double elapsed_sec = std::chrono::duration<double>(now - prev_time).count();
			if (elapsed_sec <= 0.0) elapsed_sec = kTargetIntervalMs / 1000.0;
			prev_time = now;

			auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
			throughputFile << timestamp_ms;

			size_t sum = 0;
			for (size_t i = 0; i < num_brokers; i++) {
				size_t bytes = broker_stats_[i].acked_messages.load(std::memory_order_relaxed) * message_size;
				size_t delta_bytes = bytes - prev_throughputs[i];
				double gbps = (static_cast<double>(delta_bytes) / elapsed_sec) / kGBDivisor;
				throughputFile << "," << gbps;
				sum += delta_bytes;
				prev_throughputs[i] = bytes;
			}

			double total_gbps = (static_cast<double>(sum) / elapsed_sec) / kGBDivisor;
			throughputFile << "," << total_gbps << "\n";

			if (!ack_frontier_recorded) {
				const size_t acked_bytes = ack_received_.load(std::memory_order_relaxed) * message_size;
				if (acked_bytes >= ack_bytes_to_kill_brokers) {
					RecordFailureEvent("Failure threshold reached (ACK frontier)");
					ack_frontier_recorded = true;
				}
			}

			if (publish_finished_.load(std::memory_order_relaxed)) {
				if (drain_remaining < 0) drain_remaining = kDrainIntervalsAfterFinish;
				if (--drain_remaining <= 0) break;
			}
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
	auto ack_epoll_start = std::chrono::steady_clock::now();
	int num_events = epoll_wait(epoll_fd, events.data(), max_events, EPOLL_TIMEOUT_MS);
	if (ShouldEnableNetworkPathProfile()) {
		auto& net_profile = GetClientNetworkPathProfile();
		net_profile.ack_epoll_calls.fetch_add(1, std::memory_order_relaxed);
		net_profile.ack_epoll_wait_ns.fetch_add(NsSince(ack_epoll_start), std::memory_order_relaxed);
		if (num_events == 0) {
			net_profile.ack_epoll_timeouts.fetch_add(1, std::memory_order_relaxed);
		}
	}

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

			// [[FIX: TCP_QUICKACK on accepted ACK socket]] Re-arm so broker’s TCP ACKs are sent
			// immediately after each send, not delayed 40ms by delayed-ACK timer.
			{
				int one = 1;
				if (setsockopt(client_sock, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one)) < 0) {
					LOG(WARNING) << "setsockopt(TCP_QUICKACK) on accepted ACK socket failed: " << strerror(errno);
				}
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
				// before we register the fd � socket stuck in WAITING_FOR_ID forever � B0=0 ACKs.
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
							if (IsOrder5SessionMode()) {
								prev_ack_per_sock[client_sock] =
									ack_message_base_.load(std::memory_order_acquire);
							}
							// Continue reading potential ACK data in the same loop iteration
						}
					// If ID still not complete, loop will try recv() again if more data indicated by epoll
				}else if(current_state == ConnState::READING_ACKS){
					// [[CRITICAL_FIX: Buffer partial ACK reads]] - Don't discard bytes when recv returns < sizeof(size_t).
					// Otherwise we can lose ACK data and ack_received_ never reaches client_order_ (test hangs).
					auto& partial = partial_ack_reads[client_sock];
					size_t needed = sizeof(size_t) - partial.second;
					auto ack_recv_start = std::chrono::steady_clock::now();
					ssize_t recv_ret = recv(client_sock,
							reinterpret_cast<char*>(&partial.first) + partial.second,
							needed, 0);
					if (ShouldEnableNetworkPathProfile()) {
						auto& net_profile = GetClientNetworkPathProfile();
						net_profile.ack_recv_calls.fetch_add(1, std::memory_order_relaxed);
						net_profile.ack_recv_syscall_ns.fetch_add(NsSince(ack_recv_start), std::memory_order_relaxed);
					}
					if (recv_ret == 0) { connection_error_or_closed = true; break; }
					if (recv_ret < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK) break; // No more data now
						if (errno == EINTR) continue; // Retry read
						LOG(ERROR) << "AckThread: recv error reading ACK bytes on fd " << client_sock << ": " << strerror(errno);
						connection_error_or_closed = true; break;
					}
					partial.second += static_cast<size_t>(recv_ret);
					if (partial.second != sizeof(size_t)) {
						// EPOLLET requires draining socket data until EAGAIN.
						// If we break here, remaining bytes may never trigger a new edge,
						// stranding an ACK fragment and stalling cumulative progress.
						continue;
					}

					// --- Process Full ACK Value ---
					// Re-arm TCP_QUICKACK once per complete ACK (not per recv fragment).
					// Linux resets to delayed-ACK after each recv(); re-arming keeps the
					// broker's send window open without a syscall on every partial read.
					{
						int one = 1;
						setsockopt(client_sock, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
					}
					size_t acked_msg = partial.first;
					const uint32_t maybe_magic = static_cast<uint32_t>(acked_msg & 0xFFFFFFFFULL);
					const uint32_t control_len = static_cast<uint32_t>((acked_msg >> 32) & 0xFFFFFFFFULL);
					if (maybe_magic == kSessionControlMagic) {
						partial_ack_reads[client_sock] = {0, 0};
						if (control_len == 0 || control_len > kMaxSessionControlPayload) {
							LOG(ERROR) << "AckThread: invalid SessionFenced control length " << control_len;
							connection_error_or_closed = true;
							break;
						}
						std::string payload(control_len, '\0');
						const int old_flags = fcntl(client_sock, F_GETFL, 0);
						if (old_flags >= 0) {
							fcntl(client_sock, F_SETFL, old_flags & ~O_NONBLOCK);
						}
						struct timeval tv;
						tv.tv_sec = 1;
						tv.tv_usec = 0;
						setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
						size_t got = 0;
						while (got < control_len) {
							ssize_t n = recv(client_sock, payload.data() + got, control_len - got, 0);
							if (n <= 0) break;
							got += static_cast<size_t>(n);
						}
						if (old_flags >= 0) {
							fcntl(client_sock, F_SETFL, old_flags);
						}
						if (got != control_len) {
							LOG(ERROR) << "AckThread: short SessionFenced control payload got="
							           << got << " expected=" << control_len;
							connection_error_or_closed = true;
							break;
						}
						embarcadero::session::SessionFenced fenced;
						if (!fenced.ParseFromString(payload)) {
							LOG(ERROR) << "AckThread: failed to parse SessionFenced control payload";
							connection_error_or_closed = true;
							break;
						}
							LOG(WARNING) << "[SESSION_FENCED_OBSERVED]"
							             << " broker_id=" << client_sockets[client_sock]
							             << " committed_batch_seq=" << fenced.committed_batch_seq()
							             << " committed_msg_hwm=" << fenced.committed_msg_hwm()
							             << " control_epoch=" << fenced.control_epoch()
							             << " reason=" << fenced.reason();
							HandleSessionFenced(fenced, client_sockets[client_sock]);
							connection_error_or_closed = true;
							break;
						}
					if (ShouldEnableNetworkPathProfile()) {
						GetClientNetworkPathProfile().ack_values_processed.fetch_add(1, std::memory_order_relaxed);
					}
					partial_ack_reads[client_sock] = {0, 0}; // Reset for next ACK
					int broker_id = client_sockets[client_sock]; // Get broker ID

					// [[CRITICAL FIX: Validate broker_id bounds]] Check for FD reuse corruption
					if (broker_id < 0 || broker_id >= (int)broker_stats_.size()) {
						LOG(ERROR) << "AckThread: Invalid broker_id=" << broker_id << " for fd=" << client_sock
						           << " (FD reuse or corruption). Closing connection.";
						connection_error_or_closed = true; break;
					}

					const size_t session_global_acked =
						IsOrder5SessionMode()
							? SessionGlobalAckFromGeneration(
								ack_message_base_.load(std::memory_order_acquire),
								acked_msg)
							: acked_msg;
					size_t prev_acked = prev_ack_per_sock[client_sock]; // Assumes key exists

					if (session_global_acked >= prev_acked || prev_acked == (size_t)-1) { // Check for valid cumulative value
						// [[CRITICAL_FIX: Handle first ACK correctly to avoid unsigned underflow]]
						// If prev_acked == (size_t)-1, this is the first ACK from this broker
						// Direct subtraction would underflow: acked_msg - (size_t)-1 = huge number
						// We must handle first ACK specially: new_acked_msgs = acked_msg (not acked_msg - (-1))
						size_t new_acked_msgs;
						if (IsOrder5SessionMode()) {
							size_t global_prev = order5_last_ack_hwm_.load(std::memory_order_acquire);
							while (session_global_acked > global_prev &&
							       !order5_last_ack_hwm_.compare_exchange_weak(
								       global_prev, session_global_acked,
								       std::memory_order_acq_rel,
								       std::memory_order_acquire)) {}
							new_acked_msgs = session_global_acked > global_prev ? session_global_acked - global_prev : 0;
						} else if (prev_acked == (size_t)-1) {
							// First ACK from this broker - use value directly (no previous to subtract)
							new_acked_msgs = acked_msg;
						} else {
							// Subsequent ACK - calculate increment from previous
							new_acked_msgs = session_global_acked - prev_acked;
						}
						if (new_acked_msgs > 0) {
#ifdef COLLECT_LATENCY_STATS
								ProcessPublishAckLatency(broker_id, session_global_acked);
#endif
								broker_stats_[broker_id].acked_messages.fetch_add(new_acked_msgs, std::memory_order_relaxed);
								last_ack_progress_ns_.store(SteadyNowNs(), std::memory_order_release);
								prev_ack_per_sock[client_sock] = session_global_acked; // Update last value for this socket
								CompleteUnackedThrough(broker_id, session_global_acked);
								ack_received_.fetch_add(new_acked_msgs, std::memory_order_release);
							} else {
							// Duplicate cumulative value, ignore.
							VLOG(5) << "AckThread: fd=" << client_sock << " (Broker " << broker_id << 
								") Duplicate ACK messages received: " << session_global_acked;
						}
					} else {
						LOG(WARNING) << "AckThread: Received non-monotonic ACK bytes on fd " << client_sock
							<< " (Broker " << broker_id << "). Received: " << session_global_acked << ", Previous: " << prev_acked;
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
	size_t sent_msgs = 0;
	size_t sent_batches = 0;

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

		// Register socket with epoll; EPOLLRDHUP detects peer FIN immediately
		struct epoll_event event;
		event.data.fd = sock.get();
		event.events = EPOLLOUT | EPOLLRDHUP;
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
							MSG_NOSIGNAL);

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
							VLOG(1) << "PublishThread: Handshake sent successfully to broker " << brokerId 
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
			if (!SendSessionOpenOnSocket(sock.get(), efd.get(), brokerId)) {
				LOG(ERROR) << "PublishThread: SessionOpen failed for broker " << brokerId;
				return false;
			}

			return true;
		};

	// Connect to initial broker.
	// [[SESSION_OPEN_RETRY 2026-07-11]] The broker's ReqReceive pool binds one
	// handler thread per publish connection for the connection's lifetime, so
	// under a multi-client connect storm a connection can sit in the accept
	// queue longer than one SessionOpen reply timeout. A one-shot failure
	// here permanently killed the thread — and with it the whole client Init
	// (run 20260711T065047Z, N=3). Retry with backoff; each attempt opens a
	// fresh socket.
	// [[N2_DEAD_QUEUE 2026-07-12]] Stagger first connect so N clients ×
	// THREADS_PER_BROKER do not stampede SessionOpen together. On give-up,
	// MarkQueueInactive so preferred striping cannot seal into a dead queue.
	VLOG(1) << "PublishThread[" << pubQuesIdx << "]: Starting connection to broker " << broker_id;
	if (pubQuesIdx > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(pubQuesIdx) * 40));
	}
	if (ShouldSimulateInitialConnectFail(pubQuesIdx)) {
		LOG(ERROR) << "PublishThread[" << pubQuesIdx << "]: Simulated initial connect failure "
		           << "(EMBARCADERO_SIMULATE_CONNECT_FAIL_MOD)";
		pubQue_.MarkQueueInactive(pubQuesIdx);
		{
			absl::MutexLock l(&mutex_);
			RefreshOrder5PreferredQueuesLocked();
		}
		return;
	}
	{
		const int kInitialConnectAttempts = GetInitialPublishConnectAttempts();
		int connect_attempt = 1;
		while (!connect_to_server(broker_id)) {
			if (connect_attempt >= kInitialConnectAttempts ||
			    shutdown_.load(std::memory_order_relaxed)) {
				LOG(ERROR) << "PublishThread[" << pubQuesIdx << "]: Failed to connect to broker "
				           << broker_id << " after " << connect_attempt << " attempts";
				pubQue_.MarkQueueInactive(pubQuesIdx);
				{
					absl::MutexLock l(&mutex_);
					RefreshOrder5PreferredQueuesLocked();
				}
				return;
			}
			LOG(WARNING) << "PublishThread[" << pubQuesIdx << "]: connect/SessionOpen to broker "
			             << broker_id << " failed (attempt " << connect_attempt << "/"
			             << kInitialConnectAttempts << ") — retrying in 2s";
			connect_attempt++;
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}
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
					zero_batch_publish_threads_.fetch_add(1, std::memory_order_relaxed);
					std::this_thread::yield();
				}
				// Drain remaining batches before exit.
				while ((batch_header = static_cast<Embarcadero::BatchHeader*>(pubQue_.Read(pubQuesIdx))) != nullptr
				       && batch_header->total_size != 0) {
					has_sent_batch = true;
					goto process_batch;
				}
				break;
			} else {
				// [[PERF]] spin 128x before yield when waiting for batch.
				static constexpr int kConsumerSpinCount = 128;
				for (int s = 0; s < kConsumerSpinCount; s++) {
					Embarcadero::CXL::cpu_pause();
				}
				consecutive_empty_reads++;
				// Reduce scheduler churn when producer is far behind: after sustained empties,
				// sleep briefly instead of yielding every poll loop.
				if (consecutive_empty_reads >= 2048) {
					std::this_thread::sleep_for(std::chrono::microseconds(2));
				} else {
					std::this_thread::yield();
				}
				continue;
			}
		}

	process_batch:
			consecutive_empty_reads = 0;
#ifdef COLLECT_LATENCY_STATS
			auto submit_time = std::chrono::steady_clock::now();
			bool has_submit_time = pubQue_.GetBatchSubmitTime(batch_header, &submit_time);
			if (!has_submit_time) {
				// Keep send->ack metric only; submit->ack must use true submit timestamps.
				publish_submit_time_missing_.fetch_add(1, std::memory_order_relaxed);
			}
#endif
		
		if (enable_batch_attempted_for_timeout_log_) {
			total_batches_attempted_.fetch_add(1, std::memory_order_relaxed);
		}

			batch_header->client_id = client_id_;
			batch_header->broker_id = broker_id;
			if (IsOrder5SessionMode()) {
				uint32_t epoch = session_epoch_.load(std::memory_order_acquire);
				if (epoch == 0) {
					epoch = requested_session_epoch_.load(std::memory_order_acquire);
				}
				batch_header->session_epoch = static_cast<uint16_t>(epoch & 0xFFFFU);
				batch_header->session_epoch32 = epoch;
			}

			// Get pointer to message data
			void* msg = reinterpret_cast<uint8_t*>(batch_header) + sizeof(Embarcadero::BatchHeader);
			len = batch_header->total_size;
			const size_t wire_bytes = sizeof(Embarcadero::BatchHeader) + len;
			WaitForUnackedCapacity(wire_bytes);

		// Function to send batch header
		auto send_batch_header = [&]() -> void {
			auto* net_profile = ShouldEnableNetworkPathProfile() ? &GetClientNetworkPathProfile() : nullptr;
			auto header_loop_start = std::chrono::steady_clock::now();
			// Always refresh broker_id from the (potentially updated) local variable.
			// After reconnection to a new broker, broker_id changes; batch header must reflect it.
			batch_header->broker_id = broker_id;

			// Handle sequencer-specific batch header processing
				if (seq_type_ == heartbeat_system::SequencerType::EMBARCADERO &&
				    order_level_ == Embarcadero::kOrderStrong) {
					// ORDER=5 batch_seq is assigned when the producer seals the batch in QueueBuffer.
					// Reassigning it here from parallel send threads makes ordering depend on
					// scheduler/send timing instead of deterministic client submit order.
				} else if (seq_type_ == heartbeat_system::SequencerType::EMBARCADERO &&
				           order_level_ == Embarcadero::kOrderClientBrokerStream) {
					// Weaker ORDER=4 preserves FIFO per (client, broker) stream.
					if (broker_id >= 0 && broker_id < kMaxCorfuBrokers) {
						batch_header->batch_seq =
							order5_batch_seq_per_broker_[broker_id].fetch_add(1, std::memory_order_relaxed);
					}
				} else if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
					// [[CORFU_FIX]] Sequencer expects per-broker batch_seq (0,1,2,...), not global.
					// Use per-broker counter so each broker's batches are sequenced correctly.
					bool got_total_order = false;
					std::string ingress_data_endpoint;
					{
						absl::MutexLock lock(&mutex_);
						auto it = nodes_.find(broker_id);
						if (it != nodes_.end()) ingress_data_endpoint = it->second;
					}
					if (ingress_data_endpoint.empty()) {
						throw std::runtime_error("Corfu token proxy endpoint missing from broker membership");
					}
					auto [ingress_host, ignored_data_port] = ParseAddressPort(ingress_data_endpoint);
					const int proxy_base = [] {
						if (const char* value = std::getenv("EMBARCADERO_CORFU_PROXY_PORT_BASE")) return std::atoi(value);
						return 50100;
					}();
					const std::string proxy_endpoint = ingress_host + ":" +
						std::to_string(proxy_base + broker_id);
					if (broker_id >= 0 && broker_id < kMaxCorfuBrokers) {
						// [[CORFU_ORDER2_FIX]] Serialize sequencer calls per broker (Phase 2C).
						// This ensures in-order delivery to the sequencer, eliminating UNAVAILABLE retries.
						std::lock_guard<std::mutex> lock(corfu_seq_per_broker_lock_[broker_id]);
						batch_header->batch_seq = corfu_batch_seq_per_broker_[broker_id].fetch_add(1, std::memory_order_relaxed);
						got_total_order = corfu_client_->GetTotalOrder(batch_header, proxy_endpoint);
					} else {
						got_total_order = corfu_client_->GetTotalOrder(batch_header, proxy_endpoint);
					}
					if (!got_total_order) {
						throw std::runtime_error("corfu sequencer GetTotalOrder failed");
					}

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

			// ORDER=5 EMBARCADERO now uses per-broker batch_seq to match broker-local stream sequencing.

			// Send batch header with retry logic
			size_t total_sent = 0;
			const size_t header_size = sizeof(Embarcadero::BatchHeader) + len;
			size_t consecutive_timeouts = 0;
			const size_t max_consecutive_timeouts = 500; // 500ms fallback (TCP_USER_TIMEOUT handles fast path)

			while (total_sent < header_size) {
				auto send_start = std::chrono::steady_clock::now();
				int send_flags = MSG_NOSIGNAL;
				if (ShouldEnablePayloadMsgMore()) {
					send_flags |= MSG_MORE;
				}
				bytesSent = send(sock.get(), 
						reinterpret_cast<uint8_t*>(batch_header) + total_sent, 
						header_size - total_sent, 
						send_flags);
				if (net_profile) {
					net_profile->header_send_syscall_ns.fetch_add(NsSince(send_start), std::memory_order_relaxed);
				}

				if (bytesSent < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
						if (net_profile) {
							net_profile->header_eagain_events.fetch_add(1, std::memory_order_relaxed);
						}
						// Wait for socket to become writable so broker can recv() and drain kernel buffer.
						// [[ROOT_CAUSE_FIX]] 0ms caused busy-loop; brokers blocked in recv(), ACK stall.
						// Use 1ms so we yield and broker gets CPU; epoll returns when EPOLLOUT (writable).
						static constexpr int EPOLL_WAIT_WRITABLE_MS = 1;
						struct epoll_event events[64];
						auto wait_start = std::chrono::steady_clock::now();
						int n = epoll_wait(efd.get(), events, 64, EPOLL_WAIT_WRITABLE_MS);
						if (net_profile) {
							net_profile->header_wait_calls.fetch_add(1, std::memory_order_relaxed);
							net_profile->header_wait_ns.fetch_add(NsSince(wait_start), std::memory_order_relaxed);
						}

						if (n == -1) {
							if (errno == EINTR) continue;
							LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
							throw std::runtime_error("epoll_wait failed");
						} else if (n == 0) {
							if (net_profile) {
								net_profile->header_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
							}
							consecutive_timeouts++;
							size_t effective_hdr_timeout = throttle_relaxed_.load(std::memory_order_relaxed) ? 50 : max_consecutive_timeouts;
							if (consecutive_timeouts > effective_hdr_timeout) {
								LOG(ERROR) << "PublishThread: Header send timed out. Assuming broker is dead.";
								throw std::runtime_error("send timeout");
							}
						} else {
							consecutive_timeouts = 0;
						}
					} else {
						// Fatal error
						LOG(ERROR) << "Failed to send batch header: " << strerror(errno);
						throw std::runtime_error("send failed");
					}
				} else {
					if (net_profile) {
						net_profile->header_send_calls.fetch_add(1, std::memory_order_relaxed);
						net_profile->header_send_bytes.fetch_add(static_cast<uint64_t>(bytesSent), std::memory_order_relaxed);
					}
					
					total_sent += bytesSent;
					broker_stats_[broker_id].sent_bytes.fetch_add(bytesSent, std::memory_order_relaxed);
					total_sent_bytes_.fetch_add(bytesSent, std::memory_order_relaxed);

					consecutive_timeouts = 0;
					if (throttle_relaxed_.load(std::memory_order_relaxed)) {
						char probe;
						ssize_t r = recv(sock.get(), &probe, 1, MSG_PEEK | MSG_DONTWAIT);
						if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
							throw std::runtime_error("broker dead (FIN/RST detected)");
						}
					}
				}
			}
			if (net_profile) {
				net_profile->header_loop_ns.fetch_add(NsSince(header_loop_start), std::memory_order_relaxed);
			}
		};

		// Try to send batch header, handle failures
		try {
			send_batch_header();
			if (sent_batches % 100 == 0 || sent_batches == 0) {
				VLOG(2) << "PublishThread[" << pubQuesIdx << "]: Sent batch header for batch " 
				        << sent_batches << " to broker " << broker_id;
			}
		} catch (const std::exception& e) {
			total_batches_failed_.fetch_add(1, std::memory_order_relaxed);
			LOG(ERROR) << "Exception sending batch header: " << e.what();
			std::string fail_msg = "Header Send Fail Broker " + std::to_string(broker_id) + " (" + e.what() + ")";
			RecordFailureEvent(fail_msg); // Record event

			// DYNAMIC MASK UPDATE: stop upstream from feeding this queue
			pubQue_.MarkQueueInactive(pubQuesIdx);

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

					const std::vector<int> survivors = brokers_;
					new_broker_id = RendezvousBroker(static_cast<uint32_t>(client_id_),
					                                batch_header->batch_seq,
					                                survivors,
					                                broker_id);
					if (new_broker_id < 0) {
						new_broker_id = brokers_[(pubQuesIdx % num_threads_per_broker_) % brokers_.size()];
					}
				}

			// CORFU: GetTotalOrder pre-assigns log_idx and broker_batch_seq for the
			// original broker. Re-routing after that would write data to the wrong
			// broker-local log and can strand the original per-broker sequence.
			if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
				pubQue_.ReleaseBatch(batch_header);
				LOG(ERROR) << "CORFU: broker " << broker_id << " failed after GetTotalOrder was issued; "
				           << "cannot reroute safely. Aborting thread.";
				RecordFailureEvent("CORFU Header Send Fail No Reroute Broker " + std::to_string(broker_id));
				return;
			}

			// Connect to new broker
			if (!connect_to_server(new_broker_id)) {
				pubQue_.ReleaseBatch(batch_header);
				RecordFailureEvent("Reconnect Fail Broker " + std::to_string(new_broker_id));
				LOG(ERROR) << "Failed to connect to replacement broker " << new_broker_id;
				// Already MarkQueueInactive above; refresh preferred so routing
				// diagnostics drop this dead queue (ORDER=5 striping).
				{
					absl::MutexLock l(&mutex_);
					RefreshOrder5PreferredQueuesLocked();
				}
				return;
			}

			std::string reconn_msg = "Reconnect Success Broker " + std::to_string(new_broker_id) + " (from " + std::to_string(broker_id) + ")";
			RecordFailureEvent(reconn_msg);

			{
				absl::MutexLock lock(&mutex_);
				ReassignQueueBrokerLocked(static_cast<size_t>(pubQuesIdx), broker_id, new_broker_id);
			}
			broker_id = new_broker_id;
			pubQue_.MarkQueueActive(pubQuesIdx);
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
		}

		// Mark that we've sent at least one batch
		has_sent_batch = true;
		total_batches_sent_.fetch_add(1, std::memory_order_relaxed);
		total_messages_sent_.fetch_add(batch_header->num_msg, std::memory_order_relaxed);
		// Track wall-clock time of last batch send completion (atomic max across threads).
		// Used by GetLastSendWallNs() for accurate bandwidth measurement excluding Poll() overhead.
		{
			const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			int64_t prev = last_send_wall_ns_.load(std::memory_order_relaxed);
			while (now_ns > prev &&
			       !last_send_wall_ns_.compare_exchange_weak(prev, now_ns,
			           std::memory_order_relaxed, std::memory_order_relaxed)) {}
		}
			size_t prev_sent = broker_stats_[broker_id].sent_messages.fetch_add(
				batch_header->num_msg, std::memory_order_relaxed);
			size_t end_count = prev_sent + batch_header->num_msg;
				const bool unacked_owns_batch = RecordUnackedBatch(
					*batch_header, batch_header,
					sizeof(Embarcadero::BatchHeader) + batch_header->total_size,
					broker_id, end_count);
	#ifdef COLLECT_LATENCY_STATS
			RecordPublishSend(broker_id, end_count, submit_time, has_submit_time);
#else
		(void)end_count;
#endif
		sent_batches += 1;
		sent_msgs += batch_header->num_msg;

			// Owned RTO copy → release hugepage slot now. Pool-pin retention
			// (ACK1 / memory ACK2) holds the slot until CompleteUnackedThrough.
			if (!unacked_owns_batch) {
				pubQue_.ReleaseBatch(batch_header);
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

		VLOG(1) << "SubscribeToCluster: Creating gRPC reader for cluster status subscription...";
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
			VLOG(1) << "SubscribeToCluster: Received cluster status update with "
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
					if (expected_num_brokers_ > 0 && broker_id >= expected_num_brokers_) {
						VLOG(1) << "SubscribeToCluster: ignoring broker " << broker_id
						        << " beyond expected NUM_BROKERS=" << expected_num_brokers_;
						continue;
					}
					if (bi.accepts_publishes()) {
						nodes_[broker_id] = bi.network_mgr_addr();
						brokers_.emplace_back(broker_id);
						VLOG(1) << "SubscribeToCluster: Added broker " << broker_id
						         << " (accepts_publishes=true)";
					} else {
						VLOG(1) << "SubscribeToCluster: Skipping broker " << broker_id
						         << " (accepts_publishes=false)";
					}
				}
				std::sort(brokers_.begin(), brokers_.end());
				RefreshOrder5PreferredQueuesLocked();

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
					if (expected_num_brokers_ > 0 && broker_id >= expected_num_brokers_) {
						VLOG(1) << "SubscribeToCluster: ignoring legacy broker " << broker_id
						        << " beyond expected NUM_BROKERS=" << expected_num_brokers_;
						continue;
					}
					nodes_[broker_id] = addr;
					brokers_.emplace_back(broker_id);
				}

				// Sort brokers for deterministic round-robin assignment
				std::sort(brokers_.begin(), brokers_.end());
				RefreshOrder5PreferredQueuesLocked();
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
					bool all_connected = true;
					for (int broker_id : brokers_needing_threads) {
						bool connect_broker = true;
						{
							absl::MutexLock lock(&mutex_);
							connect_broker = ShouldConnectPublishThreadsToBrokerLocked(broker_id);
							if (!connect_broker) {
								// Mark decided without creating threads so connected_
								// can reach full cluster size under HOME_BROKERS.
								// Do not bump expected_ack_brokers_ — no ACK path here.
								brokers_with_threads_.insert(broker_id);
								VLOG(1) << "SubscribeToCluster: skipping PublishThreads for "
								        << "non-home broker " << broker_id
								        << " (ORDER5_HOME_BROKERS=" << order5_home_brokers_ << ")";
							}
						}
						if (!connect_broker) {
							continue;
						}
						VLOG(1) << "SubscribeToCluster: Adding publisher threads for broker " << broker_id;
						if (!AddPublisherThreads(num_threads_per_broker_, broker_id, qsize)) {
							LOG(ERROR) << "Failed to add publisher threads for broker " << broker_id;
							all_connected = false;
							break;
						}
						// Track that this broker now has threads
						{
							absl::MutexLock lock(&mutex_);
							brokers_with_threads_.insert(broker_id);
							// ACK expected = brokers with real PublishThreads (queue map),
							// not non-home sentinels in brokers_with_threads_.
							expected_ack_brokers_.store(
								static_cast<int>(broker_queue_indices_.size()),
								std::memory_order_release);
							expected_ack_brokers_last_update_ns_.store(
								SteadyNowNs(),
								std::memory_order_release);
						}
					}

					// Signal that we're connected (only on first successful connection)
					if (all_connected && !connected_.load(std::memory_order_acquire)) {
						int expected = expected_num_brokers_;
						if (static_cast<int>(brokers_with_threads_.size()) >= expected) {
							connected_.store(true, std::memory_order_release);
							VLOG(1) << "SubscribeToCluster: Connection established successfully. connected_=true ("
							         << brokers_with_threads_.size() << "/" << expected << " brokers)";
						} else {
							VLOG(1) << "SubscribeToCluster: " << brokers_with_threads_.size()
							         << "/" << expected << " brokers ready, waiting for full cluster...";
						}
					}
				} else if (!connected_.load(std::memory_order_acquire) && brokers_.empty() && !use_broker_info) {
					// Legacy-only fallback for old brokers that do not publish broker_info.
					LOG(WARNING) << "SubscribeToCluster: No broker_info available; using legacy fallback broker 0";
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
				// [[Issue 4]] Read failed  break inner loop, Finish(), then outer loop re-establishes reader
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
				break;  // Exit inner loop � Finish() � outer loop creates new reader
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
	std::vector<size_t> created_queue_indices;
	created_queue_indices.reserve(num_threads);
	try {
		for (size_t i = 0; i < num_threads; i++) {
			int thread_idx = num_threads_.fetch_add(1);
			threads_.emplace_back(&Publisher::PublishThread, this, broker_id, thread_idx);
			created_queue_indices.push_back(static_cast<size_t>(thread_idx));
			created++;
		}
		// So producer round-robins only over queues that have consumers (no ghost queues).
		pubQue_.SetActiveQueues(static_cast<size_t>(num_threads_.load(std::memory_order_relaxed)));
		{
			absl::MutexLock lock(&mutex_);
			auto& queues = broker_queue_indices_[broker_id];
			queues.insert(queues.end(), created_queue_indices.begin(), created_queue_indices.end());
			RefreshOrder5PreferredQueuesLocked();
		}
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
