
#include "common/env_flags.h"
#include "common/stage_trace.h"
#include <array>
#include <iomanip>
#include "publisher.h"
#include "publisher_internal.h"

using namespace Embarcadero::client::detail;
#include "publisher_profile.h"
#include "latency_stats.h"
#include "common/config.h"
#include "common/order_level.h"
#include "common/scoped_fd.h"
#include "common/fault_injection.h"
#include "network_manager/protocol.h"
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
#include <condition_variable>
#include <netdb.h>
#include <numeric>
#include <set>
#include <stdexcept>



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


int RuntimeHeaderSendTimeoutMs() {
	if (const char* env = std::getenv("EMBARCADERO_HEADER_SEND_TIMEOUT_MS")) {
		char* end = nullptr;
		const long parsed = std::strtol(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0 && parsed <= 600000) {
			return static_cast<int>(parsed);
		}
		LOG(WARNING) << "Ignoring invalid EMBARCADERO_HEADER_SEND_TIMEOUT_MS='"
		             << env << "'";
	}
	const auto& runtime = Embarcadero::GetConfig().config().client.runtime;
	const std::string mode = Embarcadero::GetConfig().getRuntimeMode();
	if (mode == "failure") return runtime.header_send_timeout_ms_failure.get();
	if (mode == "latency") return runtime.header_send_timeout_ms_latency.get();
	return runtime.header_send_timeout_ms_throughput.get();
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

std::vector<int> GetOrder5BrokerAllowlist() {
	const char* env_name = "EMBARCADERO_PUBLISH_BROKER_ALLOWLIST";
	const char* env = std::getenv(env_name);
	if (!env || env[0] == '\0') {
		env_name = "EMBARCADERO_ORDER5_BROKER_ALLOWLIST";
		env = std::getenv(env_name);
	}
	if (!env || env[0] == '\0') return {};

	std::vector<int> brokers;
	std::stringstream input(env);
	std::string item;
	while (std::getline(input, item, ',')) {
		if (item.empty()) {
			throw std::invalid_argument(
			    std::string("empty broker in ") + env_name + "='" +
			    env + "'");
		}
		char* end = nullptr;
		long parsed = std::strtol(item.c_str(), &end, 10);
		if (end == item.c_str() || *end != '\0' || parsed < 0 ||
		    parsed >= NUM_MAX_BROKERS) {
			throw std::invalid_argument(
			    std::string("invalid broker '") + item +
			    "' in " + env_name + "='" + env + "'");
		}
		if (std::find(brokers.begin(), brokers.end(), static_cast<int>(parsed)) !=
		    brokers.end()) {
			throw std::invalid_argument(
			    std::string("duplicate broker ") + std::to_string(parsed) +
			    " in " + env_name + "='" + env + "'");
		}
		brokers.push_back(static_cast<int>(parsed));
	}
	return brokers;
}

uint64_t GetOrder5GapBatchSeq() {
	const char* env = std::getenv("EMBARCADERO_ORDER5_GAP_BATCH_SEQ");
	if (!env || env[0] == '\0') return UINT64_MAX;
	char* end = nullptr;
	unsigned long long parsed = std::strtoull(env, &end, 10);
	if (end == env || *end != '\0') {
		throw std::invalid_argument(
		    std::string("invalid EMBARCADERO_ORDER5_GAP_BATCH_SEQ='") + env + "'");
	}
	return static_cast<uint64_t>(parsed);
}

int GetOrder5GapDelayMs() {
	const char* env = std::getenv("EMBARCADERO_ORDER5_GAP_DELAY_MS");
	if (!env || env[0] == '\0') return 0;
	char* end = nullptr;
	long parsed = std::strtol(env, &end, 10);
	if (end == env || *end != '\0' || parsed < 0 || parsed > 60000) {
		throw std::invalid_argument(
		    std::string("invalid EMBARCADERO_ORDER5_GAP_DELAY_MS='") + env + "'");
	}
	return static_cast<int>(parsed);
}

// 0 = one-shot gap; N>0 = re-inject the gap every N batches (repeated skew samples).
uint64_t GetOrder5GapPeriodBatches() {
	const char* env = std::getenv("EMBARCADERO_ORDER5_GAP_PERIOD_BATCHES");
	if (!env || env[0] == '\0') return 0;
	char* end = nullptr;
	unsigned long long parsed = std::strtoull(env, &end, 10);
	if (end == env || *end != '\0') {
		throw std::invalid_argument(
		    std::string("invalid EMBARCADERO_ORDER5_GAP_PERIOD_BATCHES='") + env + "'");
	}
	return static_cast<uint64_t>(parsed);
}

// Multi-client SessionOpen storms can exceed the old 1s reply window (N=2×24
// threads). Override with EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC.


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
	order5_broker_allowlist_(GetOrder5BrokerAllowlist()),
	order5_gap_batch_seq_(GetOrder5GapBatchSeq()),
	order5_gap_delay_ms_(GetOrder5GapDelayMs()),
	order5_gap_period_batches_(GetOrder5GapPeriodBatches()),
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
	if (!order5_broker_allowlist_.empty()) {
		std::vector<int> allowed;
		allowed.reserve(order5_broker_allowlist_.size());
		for (int broker_id : order5_broker_allowlist_) {
			if (std::find(brokers_.begin(), brokers_.end(), broker_id) != brokers_.end()) {
				allowed.push_back(broker_id);
			}
		}
		return allowed;
	}
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
	// A general publish allowlist implements sticky placement for any sequencer.
	// ORDER=5's home-broker policy remains the default when no explicit list is
	// present.
	if (!order5_broker_allowlist_.empty()) {
		return std::find(order5_broker_allowlist_.begin(),
		                 order5_broker_allowlist_.end(), broker_id) !=
		       order5_broker_allowlist_.end();
	}
	if (!IsOrder5SessionMode() ||
	    order5_home_brokers_ == 0) {
		return true;
	}
	const std::vector<int> homes = Order5HomeBrokerIdsLocked();
	return std::find(homes.begin(), homes.end(), broker_id) != homes.end();
}

void Publisher::RefreshOrder5PreferredQueuesLocked() {
	if (seq_type_ != heartbeat_system::SequencerType::EMBARCADERO ||
	    order_level_ != Embarcadero::kOrderStrong ||
	    (order5_home_brokers_ == 0 && order5_broker_allowlist_.empty()) ||
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
	if (!IsOrder5SessionMode() && order5_broker_allowlist_.empty()) {
		return;
	}
	std::ostringstream oss;
	oss << (IsOrder5SessionMode() ? "[ORDER5_ROUTING]" : "[PUBLISH_ROUTING]")
	    << " client_id=" << client_id_
	    << " home_brokers=" << order5_home_brokers_
	    << " retransmit_attempts="
	    << retransmit_attempts_.load(std::memory_order_relaxed)
	    << " session_fenced_observed="
	    << session_fenced_observed_.load(std::memory_order_relaxed)
	    << " session_rto_min_ms=" << RuntimeSessionRtoFloorMs();
	if (!order5_broker_allowlist_.empty()) {
		oss << " allowlist=";
		for (size_t i = 0; i < order5_broker_allowlist_.size(); ++i) {
			if (i != 0) oss << ",";
			oss << order5_broker_allowlist_[i];
		}
	}
	for (size_t broker_id = 0; broker_id < broker_stats_.size(); ++broker_id) {
		const size_t sent = broker_stats_[broker_id].sent_messages.load(std::memory_order_relaxed);
		if (sent == 0) continue;
		oss << " broker" << broker_id << "_msgs=" << sent;
	}
	LOG(INFO) << oss.str();
}


// [[CORFU_GATE]] Ordered token-request gate (C2/C5/C6/C7). See
// docs/contracts/CORFU_INVARIANT_LEDGER.md. The turn is granted strictly in
// seal-ticket order; failure or shutdown poisons the gate so no successor can
// acquire a token past a failed predecessor.
bool Publisher::CorfuAcquireTicketTurn(size_t ticket) {
	const auto result = corfu_gate_.Acquire(ticket, shutdown_);
	corfu_ticket_wait_ns_.fetch_add(result.wait_ns, std::memory_order_relaxed);
	if (!result.acquired) {
		corfu_gate_denied_.fetch_add(1, std::memory_order_relaxed);
		LOG(ERROR) << "Corfu token turn denied: ticket=" << ticket
		           << " next=" << result.next_ticket
		           << " reason=\"" << result.abort_reason << "\"";
	}
	return result.acquired;
}

bool Publisher::CorfuCompleteTicket(size_t ticket) {
	if (corfu_gate_.Complete(ticket, shutdown_)) return true;
	const auto snapshot = corfu_gate_.GetSnapshot();
	LOG(ERROR) << "CorfuCompleteTicket failed closed: ticket=" << ticket
	           << " next=" << snapshot.next_ticket
	           << " aborted=" << (snapshot.aborted ? 1 : 0)
	           << " reason=\"" << snapshot.abort_reason << "\"";
	return false;
}

void Publisher::CorfuAbortGate(const char* reason) {
	// Cancel an in-flight RPC before poisoning the gate. Register/cancel is
	// terminal and synchronized inside the client, so a retry cannot slip in
	// after this cancellation point.
	if (corfu_client_) corfu_client_->CancelActiveRequests();
	if (corfu_gate_.Abort(reason)) {
		const auto snapshot = corfu_gate_.GetSnapshot();
		LOG(ERROR) << "[CORFU_GATE_ABORT] reason=\"" << reason
		           << "\" next_ticket=" << snapshot.next_ticket;
	}
}

void Publisher::LogCorfuTokenPhase() {
	if (seq_type_ != heartbeat_system::SequencerType::CORFU) return;
	const uint64_t token_requests = corfu_token_requests_.load(std::memory_order_relaxed);
	const uint64_t token_grants = corfu_token_grants_.load(std::memory_order_relaxed);
	const uint64_t token_latency_ns = corfu_token_latency_ns_.load(std::memory_order_relaxed);
	const auto gate = corfu_gate_.GetSnapshot();
	LOG(INFO) << "[CORFU_TOKEN_PHASE] requests=" << token_requests
	          << " grants=" << token_grants
	          << " failures=" << corfu_token_failures_.load(std::memory_order_relaxed)
	          << " gate_aborts=" << gate.abort_transitions
	          << " gate_denied=" << corfu_gate_denied_.load(std::memory_order_relaxed)
	          << " payload_sends=" << corfu_payload_sends_.load(std::memory_order_relaxed)
	          << " payload_before_grant="
	          << corfu_payload_before_grant_.load(std::memory_order_relaxed)
	          << " payload_admission_denied="
	          << corfu_payload_admission_denied_.load(std::memory_order_relaxed)
	          << " mean_token_latency_us="
	          << (token_requests == 0 ? 0 : token_latency_ns / token_requests / 1000)
	          << " ticket_wait_ms_sum="
	          << corfu_ticket_wait_ns_.load(std::memory_order_relaxed) / 1000000
	          << " unavailable_retries="
	          << (corfu_client_ ? corfu_client_->UnavailableRetries() : 0)
	          << " transient_retries="
	          << (corfu_client_ ? corfu_client_->TransientRetries() : 0)
	          << " token_delay_us=" << corfu_token_delay_us_
	          << " aborted=" << (gate.aborted ? 1 : 0)
	          << " abort_reason=\"" << gate.abort_reason << "\"";
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
    {
        std::lock_guard<std::mutex> lock(session_fence_handle_mu_);
        publisher_workers_stop_.store(true, std::memory_order_release);
    }
    NotifyPublisherWork();
	consumer_should_exit_.store(true, std::memory_order_relaxed);
	unacked_cv_.notify_all();
	// [[CORFU_GATE]] Wake ordered-token-gate waiters so the joins below cannot
	// block on a wedged ticket (C6/C7). No-op after a normal Poll (threads
	// already joined; gate idle).
	if (seq_type_ == heartbeat_system::SequencerType::CORFU &&
	    !threads_joined_.load(std::memory_order_acquire)) {
		CorfuAbortGate("publisher destructor shutdown");
	}
	// Cancel current gRPC SubscribeToCluster call so cluster_probe_thread_ can exit (reader->Read() unblocks).
	if (grpc::ClientContext* ctx = subscribe_context_.exchange(nullptr)) {
		ctx->TryCancel();
	}

    std::vector<std::thread> publishers;
    {
        std::lock_guard<std::mutex> owner_lock(publisher_threads_mutex_);
        threads_joined_.store(true, std::memory_order_release);
        publishers.swap(threads_);
    }
	// Join outside the owner lock; cluster discovery can no longer add workers.
	for (auto& t : publishers) {
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
	CloseAllRetransmitChannels();

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


bool Publisher::HasSupportedOrder5AckRouting(int ack_level) const {
	if (!IsOrder5SessionMode() || ack_level < 1) return true;
	if (order5_broker_allowlist_.empty()) return order5_home_brokers_ == 0;
	return std::find(order5_broker_allowlist_.begin(),
	                 order5_broker_allowlist_.end(), 0) != order5_broker_allowlist_.end();
}

bool Publisher::Init(int ack_level) {
#ifdef EMBARCADERO_CLIENT_NO_BASELINES
    if (seq_type_ != heartbeat_system::SequencerType::EMBARCADERO) {
        LOG(ERROR) << "Baseline support was disabled when this client was built.";
        return false;
    }
#endif

	ack_level_ = ack_level;
	// ORDER5 progress and fence notifications are authoritative only on the
	// head. ACK connections currently follow publish connections; a follower-
	// only allowlist would admit data with no way to observe its completion.
	// Reject before Init resolves runtime config, allocates the batch pool, or
	// starts owned workers/RPCs. The constructor's lazy gRPC channel exists already.
	if (!HasSupportedOrder5AckRouting(ack_level)) {
		LOG(ERROR) << "ORDER5 acknowledged publishing requires default all-broker routing "
		              "or an explicit publish allowlist containing broker 0: head-only "
		              "ACK/fence connectivity is not independent of publish routing. "
		              "Follower-only allowlists and implicit home-broker routing are unsupported.";
		return false;
	}

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
	if (order5_gap_delay_ms_ > 0) {
		LOG(WARNING) << "[ORDER5_GAP_CONFIG]"
		             << " batch_seq=" << order5_gap_batch_seq_
		             << " delay_ms=" << order5_gap_delay_ms_
		             << " period_batches=" << order5_gap_period_batches_;
	}

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
				// No broker may be allowed to publish to an ACK endpoint that never
				// became a listener.  Continuing here makes a connection race look
				// like partial broker failure and can admit an unacknowledgeable batch.
				return false;
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
		// [[CORFU_TOKEN_DELAY]] Token-stage sensitivity knob (C9). Parsed once
		// before publish threads start; recorded in [CORFU_TOKEN_PHASE] (C10).
		if (const char* delay_env = std::getenv("EMBARCADERO_CORFU_TOKEN_DELAY_US")) {
			char* end = nullptr;
			const long long v = std::strtoll(delay_env, &end, 10);
			if (end == delay_env || *end != '\0' || v < 0 || v > 10'000'000) {
				LOG(ERROR) << "Publisher::Init() invalid EMBARCADERO_CORFU_TOKEN_DELAY_US=\""
				           << delay_env << "\" (must be an integer in [0, 10000000]); failing closed";
				return false;
			}
			corfu_token_delay_us_ = static_cast<uint64_t>(v);
			if (corfu_token_delay_us_ > 0) {
				LOG(INFO) << "[CORFU_TOKEN_DELAY] injected post-grant token-stage delay: "
				          << corfu_token_delay_us_ << " us";
			}
		}
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

	// ORDER=5 preferred striping and explicit sticky placement both require all
	// selected publish paths to connect. A partial connection would silently
	// change the requested placement policy.
	if (IsOrder5SessionMode() || !order5_broker_allowlist_.empty()) {
		const int ready = thread_count_.load(std::memory_order_acquire);
		const int expected = num_threads_.load(std::memory_order_acquire);
		if (ready < expected || expected <= 0) {
			LOG(ERROR) << "Publisher::Init() requested routing requires all selected publish threads connected; "
			           << "got thread_count_=" << ready << " num_threads_=" << expected
			           << ". Refusing to run with partial placement.";
			return false;
		}
		{
			absl::MutexLock lock(&mutex_);
			if (!order5_broker_allowlist_.empty()) {
				for (int broker_id : order5_broker_allowlist_) {
					if (broker_id >= expected_num_brokers_ ||
					    std::find(brokers_.begin(), brokers_.end(), broker_id) ==
					        brokers_.end()) {
						LOG(ERROR) << "Publisher::Init() publish allowlist broker "
						           << broker_id << " is not in the connected "
						           << expected_num_brokers_ << "-broker cluster";
						return false;
					}
				}
			}
			if (IsOrder5SessionMode()) RefreshOrder5PreferredQueuesLocked();
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
				// Token/payload correctness is not enough if an expected ACK path is
				// absent: continuing here admitted work that can never satisfy ACK2
				// (and previously left RF3 smokes hung).  Fail before any publisher
				// thread can enqueue a payload; the orchestrator may retry a clean
				// startup, but it must not turn partial connectivity into a result.
				return false;
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
					drain_remaining = 300;  // ~30s at 100ms interval
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
    const bool defer_publisher_join = IsOrder5SessionMode() && ack_level_ >= 1;
    double publisher_join_ms = 0;
    auto join_publishers = [&]() -> bool {
        const auto begin = std::chrono::steady_clock::now();
	// CRITICAL FIX: Use atomic flag to prevent double-join race conditions
        std::vector<std::thread> publishers;
        {
            std::lock_guard<std::mutex> lock(publisher_threads_mutex_);
            if (threads_joined_.exchange(true)) return true;
            publishers.swap(threads_);
        }
        {
			// Bound PublishThread join: previously hung forever in WaitForUnackedCapacity
			// and never reached ACK timeout. Fail closed so harness can retry.
			const int join_timeout_sec = [] {
				if (const char* env = std::getenv("EMBARCADERO_PUBLISH_JOIN_TIMEOUT_SEC")) {
					const int v = std::atoi(env);
					if (v > 0) return v;
				}
				return 120;
			}();
			// Join watchdog must wake promptly when PublishThreads finish.
			// A previous 100ms sleep_for loop added ~100ms of artificial Poll()
			// wall time on every healthy ORDER=5 ACK=1 run (seen as
			// publisher_join_ms≈100 with ack_wait_ms=0), which cut short-run
			// Bandwidth by ~11% on 4 GiB cells while Send-done matched ORDER=0.
			std::atomic<bool> join_watchdog_stop{false};
			std::atomic<bool> join_timed_out{false};
			std::mutex join_watchdog_mu;
			std::condition_variable join_watchdog_cv;
			std::thread join_watchdog;
			// [[CORFU_GATE]] CORFU joins are watched too: a thread stuck in a slow
			// send loop must not hang Poll forever; forcing shutdown_ makes gate
			// waiters abort fail-closed (C6/C7).
			if ((IsOrder5SessionMode() && ack_level_ >= 1) ||
			    seq_type_ == heartbeat_system::SequencerType::CORFU) {
				join_watchdog = std::thread([this, join_timeout_sec, &join_watchdog_stop,
				                            &join_timed_out, &join_watchdog_mu,
				                            &join_watchdog_cv]() {
					const auto deadline =
						std::chrono::steady_clock::now() + std::chrono::seconds(join_timeout_sec);
					std::unique_lock<std::mutex> lock(join_watchdog_mu);
					while (!join_watchdog_stop.load(std::memory_order_relaxed)) {
						if (std::chrono::steady_clock::now() >= deadline) {
							join_timed_out.store(true, std::memory_order_release);
							LOG(ERROR) << "[PUBLISH_JOIN_TIMEOUT] after " << join_timeout_sec
							           << "s — forcing shutdown so PublishThreads exit WaitForUnackedCapacity "
							              "(EMBARCADERO_PUBLISH_JOIN_TIMEOUT_SEC to tune)";
							shutdown_.store(true, std::memory_order_relaxed);
							if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
								CorfuAbortGate("publisher join watchdog timeout");
							}
							unacked_cv_.notify_all();
							break;
						}
						join_watchdog_cv.wait_until(lock, deadline, [&join_watchdog_stop]() {
							return join_watchdog_stop.load(std::memory_order_relaxed);
						});
					}
				});
			}
			for (size_t i = 0; i < publishers.size(); ++i) {
			if (publishers[i].joinable()) {
				try {
				// Joining publisher thread
				publishers[i].join();
				// Successfully joined publisher thread
				} catch (const std::exception& e) {
					LOG(ERROR) << "Exception joining publisher thread " << i << ": " << e.what();
				}
			}
			// Publisher thread not joinable (already joined or detached)
		}
			{
				std::lock_guard<std::mutex> lock(join_watchdog_mu);
				join_watchdog_stop.store(true, std::memory_order_relaxed);
			}
			join_watchdog_cv.notify_all();
			unacked_cv_.notify_all();
			if (join_watchdog.joinable()) {
				join_watchdog.join();
			}
			if (join_timed_out.load(std::memory_order_acquire)) {

				LOG(ERROR) << "[PUBLISH_JOIN_TIMEOUT] Poll failing closed after PublishThread join timeout";
				LogCorfuTokenPhase();
				return false;
			}
			// All publisher threads completed transmission
		}


        publisher_join_done_time = std::chrono::steady_clock::now();
        publisher_join_ms += DurationMs(begin, publisher_join_done_time);
        return true;
    };
    bool workers_finalized = false;
    auto finish_publishers = [&](bool success) -> bool {
        if (workers_finalized) return true;
        workers_finalized = true;
        if (!success) shutdown_.store(true, std::memory_order_release);
        {
            // Exclude late suffix enqueue, but never hold this gate during join:
            // reconnecting workers can themselves observe a SessionFenced reply.
            std::lock_guard<std::mutex> lock(session_fence_handle_mu_);
            publisher_workers_stop_.store(true, std::memory_order_release);
        }
        NotifyPublisherWork();
        unacked_cv_.notify_all();
        return join_publishers();
    };
    struct WorkerCleanup {
        std::function<void()> cleanup;
        ~WorkerCleanup() { cleanup(); }
    } worker_cleanup{[&] { if (!workers_finalized) (void)finish_publishers(false); }};
	// [[LAST_PERCENT_ACK_FIX]] Seal and return reads before signaling finished.
	// If we set publish_finished_ first, threads that get nullptr from Read() may exit
	// before we've called SealAll(), dropping the last batches.
	WriteFinishedOrPaused();
    // Producer input is complete. Session senders remain available for retained
    // suffix recovery until the ACK wait finishes; other modes drain and exit.
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
			LogCorfuTokenPhase();
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

    if (!defer_publisher_join && !join_publishers()) return false;
		// [[CORFU_GATE]] Fail fast (C5): a poisoned gate means at least one batch
		// was lost and successors were stopped. Do not wait out the ACK timeout;
		// the run is terminally failed and its counters mark it INVALID.
		if (seq_type_ == heartbeat_system::SequencerType::CORFU &&
		    corfu_gate_.IsAborted()) {
			LogCorfuTokenPhase();
			LOG(ERROR) << "[CORFU_ABORT] Poll failing closed: ordered token gate aborted";
			return false;
		}
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
			// ORDER=5 ACK1/ACK2 are a single per-client frontier. EpollAckThread
			// already folds every socket's deltas into order5_last_ack_hwm_ /
			// ack_received_, while broker_stats_[i] only records which socket
			// delivered the delta. After fence/reconnect the head ACK fd often
			// remaps, so broker_stats_[0] can stall (e.g. B0=27, B1=118k) while
			// the global ledger is correct — wait on that ledger, not B0 alone.
			if (IsOrder5SessionMode() && (ack_level_ == 1 || ack_level_ == 2)) {
				return order5_last_ack_hwm_.load(std::memory_order_acquire);
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
		uint64_t ack_event_waits = 0;

		
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
				// [[FENCE_POLL_EXIT]] After SESSION_FENCED is observed the committed
				// prefix is final.  Survivor brokers may still trickle ACKs for
				// already-ordered held batches, resetting last_ack_change_time and
				// extending the wait indefinitely.  Once fenced, cap the wait to 5s
				// from the fence observation regardless of trickle ACKs.
				if (session_fenced_observed_.load(std::memory_order_acquire) > 0) {
					const int64_t fence_ns = session_fence_observed_ns_.load(std::memory_order_acquire);
					if (fence_ns > 0) {
						const int64_t elapsed_since_fence_ms =
							(SteadyNowNs() - fence_ns) / 1000000LL;
						if (elapsed_since_fence_ms >= 5000) {
							LOG(INFO) << "[Publisher ACK]: SESSION_FENCED observed "
							          << elapsed_since_fence_ms << "ms ago; exiting Poll "
							          << "(committed=" << current_acks
							          << " target=" << target_acks << ")";
							break;
						}
					}
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

            // Spin only at the start of this public wait, then park between
            // progress events. The 1ms fallback also observes terminal states
            // that originate outside the ACK thread without adding a tail tax
            // to ordinary ACKs, which notify this event directly.
            if (std::chrono::steady_clock::now() - wait_start_time < ack_spin_duration) {
                Embarcadero::CXL::cpu_pause();
            } else {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
                if (timeout_seconds > 0) deadline = std::min(deadline, wait_start_time + timeout_duration);
                ++ack_event_waits;
                ack_progress_event_.WaitUntil(deadline, [&] {
                    return normalized_acks() >= target_acks || shutdown_.load(std::memory_order_acquire);
                });
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
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
            if (!Embarcadero::fault::Pause("poll.after_ack_snapshot",
                    {static_cast<uint64_t>(client_id_), session_epoch_.load(), normalized_received,
                     received, target_acks}, &shutdown_)) return false;
#endif
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
				          << " event_waits=" << ack_event_waits
				          << " low_payload_mode=" << (low_payload_poll_mode ? 1 : 0);
			}
			// [[ORDER_0_TAIL_ACK]] Drain so EpollAckThread can read in-flight ACKs before we return.
			int drain_ms = ack_drain_ms_success_;
				if (include_tail_drain && drain_ms > 0) {
					std::this_thread::sleep_for(std::chrono::milliseconds(drain_ms));
				}
	}

    if (!finish_publishers(true)) return false;

	// IMPROVED: Graceful disconnect - keep gRPC context alive for subscriber
	// Only set publish_finished flag, don't shutdown entire system
	// The gRPC context remains active to support subscriber cluster management
	// Publisher data connections are closed by joined threads
#ifdef COLLECT_LATENCY_STATS
	WritePublishLatencyResults();
#endif
	// NOTE: We do NOT set shutdown_=true or cancel context here
	// This allows the subscriber to continue using the cluster management infrastructure
	// The context will be cleaned up when the Publisher object is destroyed
	LogOrder5RoutingSummary();
	LogCorfuTokenPhase();
	const auto poll_done_time = std::chrono::steady_clock::now();
	LOG(INFO) << "[POLL_BREAKDOWN] target_messages=" << n
	          << " ack_enabled=" << (ack_level_ >= 1 ? 1 : 0)
	          << " queue_drain_ms=" << std::fixed << std::setprecision(3)
	          << DurationMs(poll_start_time, queue_drain_done_time)
	          << " publisher_join_ms=" << publisher_join_ms
	          << " ack_wait_ms=" << (ack_wait_measured ? DurationMs(ack_wait_start_time, ack_wait_done_time) : 0.0)
	          << " post_ack_ms=" << (ack_wait_measured ? std::max(0.0,
                  DurationMs(ack_wait_done_time, poll_done_time) -
                  (defer_publisher_join ? publisher_join_ms : 0.0)) : 0.0)
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
		if (IsOrder5SessionMode() && (ack_level_ == 1 || ack_level_ == 2)) {
			return order5_last_ack_hwm_.load(std::memory_order_acquire);
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
	const auto ack_spin_duration = low_payload_poll_mode
		? std::chrono::microseconds(100)
		: std::chrono::microseconds(500);

	int timeout_seconds = ack_timeout_seconds_;
	const auto timeout_duration = std::chrono::seconds(timeout_seconds);

	while (normalized_acks() < n && !shutdown_.load(std::memory_order_relaxed)) {
		// [[CORFU_GATE]] A poisoned gate is terminal: the missing batch's ACK can
		// never arrive. Fail fast instead of waiting out the timeout.
		if (seq_type_ == heartbeat_system::SequencerType::CORFU &&
		    corfu_gate_.IsAborted()) {
			LOG(ERROR) << "[CORFU_ABORT] WaitUntilAcked failing closed: ordered token gate aborted";
			return false;
		}
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

        if (std::chrono::steady_clock::now() - wait_start_time < ack_spin_duration) {
            Embarcadero::CXL::cpu_pause();
        } else {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
            if (timeout_seconds > 0) deadline = std::min(deadline, wait_start_time + timeout_duration);
            ack_progress_event_.WaitUntil(deadline, [&] {
                return normalized_acks() >= n || shutdown_.load(std::memory_order_acquire) ||
                    (seq_type_ == heartbeat_system::SequencerType::CORFU && corfu_gate_.IsAborted());
            });
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
		// Wall-clock kill: if EMBARCADERO_FAILURE_AFTER_MS > 0, sleep for that many
		// milliseconds after publish starts, then kill. Gives reproducible kill timing
		// independent of throughput rate. Falls back to bytes-threshold when unset.
		int after_ms = 0;
		if (const char* env = std::getenv("EMBARCADERO_FAILURE_AFTER_MS")) {
			try { after_ms = std::stoi(env); } catch (...) { after_ms = 0; }
		}

		if (after_ms > 0) {
			LOG(INFO) << "Failure kill armed: wall-clock " << after_ms
			          << " ms after publish start";
			for (int elapsed = 0;
			     elapsed < after_ms &&
			     !shutdown_.load(std::memory_order_relaxed) &&
			     !publish_finished_.load(std::memory_order_relaxed);
			     ++elapsed) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		} else {
			size_t bytes_to_kill_brokers = total_message_size * failure_percentage;
			while (!shutdown_.load(std::memory_order_relaxed) &&
			       !publish_finished_.load(std::memory_order_relaxed) &&
			       total_sent_bytes_.load(std::memory_order_acquire) < bytes_to_kill_brokers) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		if (!shutdown_.load(std::memory_order_relaxed)) {
			const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start_time_).count();
			const size_t sent_at_trigger = total_sent_bytes_.load(std::memory_order_acquire);
			RecordFailureEvent(after_ms > 0
				? "Failure threshold reached (wall-clock)"
				: "Failure threshold reached (sent frontier)");
			LOG(INFO) << "Triggering broker kill at t=" << elapsed_ms
			          << "ms sent=" << sent_at_trigger << " bytes";
			RecordFailureEvent("Broker kill requested (gRPC)");
			killbrokers();
			throttle_relaxed_.store(true, std::memory_order_release);
		}
	});

	// Start thread to measure real-time throughput
	real_time_throughput_measure_thread_ = std::thread([=, this]() {
		std::vector<size_t> prev_throughputs(num_brokers, 0);
		std::vector<size_t> prev_acked_bytes_fail(num_brokers, 0);  // per-broker acked baseline for Total_GBps

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
			// For ORDER=5, per-broker ACK accounting is collapsed onto broker_stats_[0]
			// by HandleSessionFenced. Use sent_messages for Broker_N_GBps display so
			// load distribution is visible; keep Total_GBps ACK-based so the stall is shown.
			const bool use_sent_for_per_broker = IsOrder5SessionMode();
			for (size_t i = 0; i < num_brokers; i++) {
				// Per-server column.
				size_t bytes = (use_sent_for_per_broker
					? broker_stats_[i].sent_messages.load(std::memory_order_relaxed)
					: broker_stats_[i].acked_messages.load(std::memory_order_relaxed)) * message_size;
				size_t delta_bytes = bytes - prev_throughputs[i];
				double gbps = (static_cast<double>(delta_bytes) / elapsed_sec) / kGBDivisor;
				throughputFile << "," << gbps;
				prev_throughputs[i] = bytes;
				// Accumulate ACK delta for Total_GBps.
				const size_t acked =
					broker_stats_[i].acked_messages.load(std::memory_order_relaxed) * message_size;
				if (acked > prev_acked_bytes_fail[i]) sum += acked - prev_acked_bytes_fail[i];
				prev_acked_bytes_fail[i] = acked;
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


void Publisher::NotifyPublisherWork() {
    ack_progress_event_.Notify();
    {
        std::lock_guard<std::mutex> lock(publisher_work_mutex_);
        ++publisher_work_generation_;
    }
    publisher_work_cv_.notify_all();
}

Embarcadero::BatchHeader* Publisher::ReadPublishBatch(int queue_index) {
    auto read = [&] { return static_cast<Embarcadero::BatchHeader*>(pubQue_.Read(queue_index)); };
    auto* batch = read();
    while (!batch && publish_finished_.load(std::memory_order_acquire) &&
           IsOrder5SessionMode() && ack_level_ >= 1 &&
           !publisher_workers_stop_.load(std::memory_order_acquire) &&
           !shutdown_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(publisher_work_mutex_);
        const auto generation = publisher_work_generation_;
        // Recheck under the notification mutex: enqueue-before-wait cannot be lost.
        batch = read();
        if (batch) break;
        publisher_work_cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return publisher_work_generation_ != generation ||
                publisher_workers_stop_.load(std::memory_order_acquire) ||
                shutdown_.load(std::memory_order_acquire);
        });
        lock.unlock();
        batch = read();
    }
    return batch;
}

void Publisher::PublishThread(int broker_id, int pubQuesIdx) {
	ScopedFd sock, efd;  // [[Phase 2.2]] RAII: closed when thread returns or on reassignment
	size_t sent_msgs = 0;
	size_t sent_batches = 0;

	// [[CORFU_GATE]] Any abnormal exit of a CORFU publish thread strands the
	// seal tickets still queued for it; successors would wait on the gate for a
	// turn that can never come (C6). Poison the gate on every exit path except
	// the normal drained-queue fall-through (disarmed below). Idempotent with
	// the explicit aborts in the catch handler.
	struct CorfuThreadExitGuard {
		Publisher* publisher;
		bool clean = false;
		~CorfuThreadExitGuard() {
			if (publisher != nullptr && !clean) {
				publisher->CorfuAbortGate("corfu publish thread exited before its queue drained");
			}
		}
	} corfu_exit_guard{
		seq_type_ == heartbeat_system::SequencerType::CORFU ? this : nullptr};

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
				if (shutdown_.load(std::memory_order_relaxed) ||
                    publisher_workers_stop_.load(std::memory_order_acquire)) {
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
	// [[EPOCH_CHANGE_RECONNECT]] Shadow of session_epoch_ seen by this thread.
	// Initialised to 0 so the first batch read always syncs to the current epoch
	// without triggering a spurious reconnect.
	uint32_t thread_local_epoch = 0;

	// Main publishing loop. [[CRITICAL: DRAIN_BEFORE_EXIT]] Do NOT break at loop top on consumer_should_exit_.
	// Doing so would exit without draining the queue, leaving batches unsent and causing ACK timeout (~0.03% shortfall).
	// Only exit when we get nullptr from Read() AND consumer_should_exit_ is set, after draining any remaining batches.
	while (true) {
		size_t len;
		int bytesSent = 0;

		// Read a batch from the queue (QueueBuffer)
		Embarcadero::BatchHeader* batch_header =
			ReadPublishBatch(pubQuesIdx);

        // ReadPublishBatch keeps session recovery consumers available after
        // producer completion; nullptr then means final stop or a legacy drain.
		if (batch_header == nullptr || batch_header->total_size == 0) {
            if (shutdown_.load(std::memory_order_acquire) ||
                publisher_workers_stop_.load(std::memory_order_acquire)) break;
            // Poll can mark producer completion after ReadPublishBatch returned
            // null. The exit decision itself must use final session completion.
            if (IsOrder5SessionMode() && ack_level_ >= 1 &&
                !publisher_workers_stop_.load(std::memory_order_acquire) &&
                !shutdown_.load(std::memory_order_acquire)) continue;
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

			// [[EPOCH_CHANGE_RECONNECT 2026-07-17]] After a session fence,
			// HandleSessionFenced advances session_epoch_ and resumes the queue.
			// Each PublishThread keeps its existing socket open (connected with the
			// old epoch). The broker validates every batch header against the epoch
			// from the SessionOpen handshake and rejects mismatches, triggering
			// another fence cycle indefinitely. Fix: detect epoch change at batch
			// dispatch time and proactively reconnect so the new SessionOpen carries
			// the new epoch before we attempt to send any batch.
			if (IsOrder5SessionMode()) {
				const uint32_t cur_epoch = session_epoch_.load(std::memory_order_acquire);
				if (cur_epoch != thread_local_epoch) {
					if (thread_local_epoch != 0) {
						// Epoch changed under us — reconnect on the same broker (or best
						// survivor) so the handshake uses the new epoch.
						LOG(WARNING) << "PublishThread[" << pubQuesIdx << "]: epoch changed "
						             << thread_local_epoch << " → " << cur_epoch
						             << " on broker=" << broker_id << "; reconnecting";
						// Try current broker first, then iterate survivors on failure.
						if (!connect_to_server(static_cast<size_t>(broker_id))) {
							std::vector<int> all_brokers;
							{ absl::MutexLock l(&mutex_); all_brokers = brokers_; }
							bool reconnected = false;
							for (int alt : all_brokers) {
								if (alt == broker_id) continue;
								if (connect_to_server(static_cast<size_t>(alt))) {
									broker_id = alt;
									{
										absl::MutexLock l(&mutex_);
										ReassignQueueBrokerLocked(static_cast<size_t>(pubQuesIdx),
										                          broker_id, alt);
									}
									reconnected = true;
									break;
								}
							}
							if (!reconnected) {
								LOG(ERROR) << "PublishThread[" << pubQuesIdx
								           << "]: no survivor accepted epoch=" << cur_epoch
								           << "; thread exiting";
								pubQue_.ReleaseBatch(batch_header);
								return;
							}
						}
					}
					thread_local_epoch = cur_epoch;
				}
			}

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
			if (shutdown_.load(std::memory_order_relaxed)) {
				// Join-timeout / fail-closed path: drop this batch and exit so Poll can finish.
				pubQue_.ReleaseBatch(batch_header);
				break;
			}
			bool should_inject_gap = false;
			if (IsOrder5SessionMode() && order5_gap_delay_ms_ > 0) {
				if (order5_gap_period_batches_ > 0) {
					should_inject_gap =
					    batch_header->batch_seq >= order5_gap_batch_seq_ &&
					    ((batch_header->batch_seq - order5_gap_batch_seq_) %
					     order5_gap_period_batches_) == 0;
				} else {
					should_inject_gap =
					    batch_header->batch_seq == order5_gap_batch_seq_ &&
					    !order5_gap_injected_.exchange(true, std::memory_order_acq_rel);
				}
			}
			if (should_inject_gap) {
				const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				    std::chrono::system_clock::now().time_since_epoch()).count();
				long long origin_ms = 0;
				if (const char* origin =
				        std::getenv("EMBARCADERO_THROUGHPUT_TIMESERIES_ORIGIN_MS")) {
					char* end = nullptr;
					origin_ms = std::strtoll(origin, &end, 10);
					if (end == origin || *end != '\0') origin_ms = 0;
				}
				LOG(WARNING) << "[ORDER5_GAP_INJECT] phase=start batch_seq="
				             << batch_header->batch_seq
				             << " delay_ms=" << order5_gap_delay_ms_
				             << " wall_ms=" << wall_ms
				             << " event_rel_ms="
				             << (origin_ms > 0 ? wall_ms - origin_ms : -1);
				std::this_thread::sleep_for(
				    std::chrono::milliseconds(order5_gap_delay_ms_));
				const auto end_wall_ms =
				    std::chrono::duration_cast<std::chrono::milliseconds>(
				        std::chrono::system_clock::now().time_since_epoch())
				        .count();
				LOG(WARNING) << "[ORDER5_GAP_INJECT] phase=end batch_seq="
				             << batch_header->batch_seq
				             << " wall_ms=" << end_wall_ms;
			}

		// Function to send batch header
		auto send_batch_header = [&]() -> void {
			bool corfu_token_granted = false;
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
					// [[CORFU_FIFO_FIX]] batch_seq still holds the seal-time global
					// sequence here (queue_buffer.cc Seal) — the client's submission
					// order across ALL brokers. Corfu's per-client FIFO contract comes
					// from token acquisition order, so hold this batch's token request
					// until every earlier-sealed batch has completed its grant. Without
					// this gate the per-broker threads race their RPCs and the sequencer
					// (whose expected_batch_seq is per (client,broker) stream) grants
					// total_order in arrival order, permuting cross-broker submission
					// order.
					// [[CORFU_GATE]] Fail-closed ordered gate (C2/C5/C6/C7): a denied
					// turn (predecessor failure or shutdown) aborts this thread before
					// any token request or payload byte.
					const size_t corfu_token_ticket = batch_header->batch_seq;
					if (!CorfuAcquireTicketTurn(corfu_token_ticket)) {
						throw std::runtime_error(
							"corfu ordered token gate aborted before grant (shutdown or predecessor failure)");
					}
					if (broker_id < 0 || broker_id >= kMaxCorfuBrokers) {
						// Fail closed: an unsequenced (client,broker) stream would desync
						// the sequencer's expected_batch_seq for every later batch (C3).
						CorfuAbortGate("corfu broker_id outside per-broker sequencing range");
						throw std::runtime_error("corfu broker_id out of per-broker sequencing range");
					}
					{
						// [[CORFU_ORDER2_FIX]] Per-broker lock keeps (client,broker)
						// stream requests in-order at the sequencer. The global gate
						// already serializes token requests; this stays as a cheap,
						// uncontended second fence for the per-broker counter + RPC pair.
						std::lock_guard<std::mutex> lock(corfu_seq_per_broker_lock_[broker_id]);
						batch_header->batch_seq = corfu_batch_seq_per_broker_[broker_id].fetch_add(1, std::memory_order_relaxed);
						const auto token_start = std::chrono::steady_clock::now();
						corfu_token_requests_.fetch_add(1, std::memory_order_relaxed);
						got_total_order = corfu_client_->GetTotalOrder(batch_header, proxy_endpoint);
						if (got_total_order && corfu_token_delay_us_ > 0) {
							// [[CORFU_TOKEN_DELAY]] Token-stage sensitivity (C9): add
							// post-grant critical-path time while the ordered turn is held.
							// This models stage cost; it is not a network fault injector.
							std::this_thread::sleep_for(
								std::chrono::microseconds(corfu_token_delay_us_));
						}
						corfu_token_latency_ns_.fetch_add(
							std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now() - token_start).count(),
							std::memory_order_relaxed);
					}
					if (!got_total_order) {
						corfu_token_failures_.fetch_add(1, std::memory_order_relaxed);
						// Fail closed (C5): successors must not acquire tokens past a
						// failed predecessor. The gate stays poisoned; every waiter
						// aborts without sending payload.
						CorfuAbortGate("corfu token acquisition failed");
						throw std::runtime_error("corfu sequencer GetTotalOrder failed");
					}
					// The coordinator issued this token even if a concurrent terminal
					// abort prevents its payload from being admitted. Such a range is
					// deliberately burned, never reused or retried under a new identity.
					corfu_token_grants_.fetch_add(1, std::memory_order_relaxed);
					// Pass the turn only after a successful grant (C2): token
					// acquisition order across brokers equals seal order. Completion
					// also linearizes against abort/shutdown; a late grant is burned.
					if (!CorfuCompleteTicket(corfu_token_ticket)) {
						throw std::runtime_error(
							"corfu token grant completed after terminal gate abort");
					}
					corfu_token_granted = true;

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
			if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
				if (!corfu_token_granted) {
					corfu_payload_before_grant_.fetch_add(1, std::memory_order_relaxed);
					throw std::runtime_error("Corfu payload attempted without token grant");
				}
				if (!corfu_gate_.AdmitPayload(shutdown_)) {
					corfu_payload_admission_denied_.fetch_add(1, std::memory_order_relaxed);
					throw std::runtime_error("Corfu payload admission denied after terminal gate abort");
				}
				corfu_payload_sends_.fetch_add(1, std::memory_order_relaxed);
			}
			size_t total_sent = 0;
			const size_t header_size = sizeof(Embarcadero::BatchHeader) + len;
			const auto header_timeout =
				std::chrono::milliseconds(RuntimeHeaderSendTimeoutMs());
			auto header_last_progress = std::chrono::steady_clock::now();

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
						} else {
							if (net_profile) {
								if (n == 0) {
									net_profile->header_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
								}
							}
							// EPOLLOUT is level-triggered and may be reported even when the
							// following send still makes no progress. Enforce the wall-clock
							// deadline after every EAGAIN wait, not only epoll timeouts.
							const auto effective_timeout =
								throttle_relaxed_.load(std::memory_order_relaxed)
									? std::chrono::milliseconds(50)
									: header_timeout;
							if (HeaderSendNoProgressExpired(
							        std::chrono::steady_clock::now(),
							        header_last_progress,
							        effective_timeout)) {
								LOG(ERROR) << "PublishThread: Header send timed out after "
								           << effective_timeout.count()
								           << " ms. Assuming broker is dead.";
								throw std::runtime_error("send timeout");
							}
						}
					} else {
						// Fatal error
						LOG(ERROR) << "Failed to send batch header: " << strerror(errno);
						throw std::runtime_error("send failed");
					}
				} else {
					header_last_progress = std::chrono::steady_clock::now();
					if (net_profile) {
						net_profile->header_send_calls.fetch_add(1, std::memory_order_relaxed);
						net_profile->header_send_bytes.fetch_add(static_cast<uint64_t>(bytesSent), std::memory_order_relaxed);
					}
					
					total_sent += bytesSent;
					broker_stats_[broker_id].sent_bytes.fetch_add(bytesSent, std::memory_order_relaxed);
					total_sent_bytes_.fetch_add(bytesSent, std::memory_order_relaxed);

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

			// [[CORFU_GATE]] CORFU fails closed on ANY publish-thread failure
			// (gate abort, token failure, header/payload send failure):
			// - GetTotalOrder pre-assigns log_idx/broker_batch_seq for the original
			//   broker, so rerouting would write the wrong broker-local log (C8);
			// - publishing successors past a lost predecessor silently breaks the
			//   client's FIFO as observed by consumers (C5).
			// Poison the gate so every other publish thread aborts before its next
			// token, then exit. Membership is left untouched for the subscriber.
			if (seq_type_ == heartbeat_system::SequencerType::CORFU) {
				CorfuAbortGate("corfu publish thread failure");
				pubQue_.ReleaseBatch(batch_header);
				LOG(ERROR) << "CORFU: publish thread for broker " << broker_id
				           << " aborting after failure; failing closed (no reroute).";
				RecordFailureEvent("CORFU Fail-Closed Abort Broker " + std::to_string(broker_id));
				return;
			}

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

			// (CORFU never reaches here: it fails closed at the top of this catch.)

			// Connect to new broker — try all survivors before giving up.
			// [[RECONNECT_RETRY_ALL_SURVIVORS]] If the first replacement broker also
			// rejects SessionOpen, the thread exits permanently, leaving permanent
			// gaps in the hold buffer that can never drain. Retry the full survivor set.
			{
				bool connected = connect_to_server(new_broker_id);
				if (!connected) {
					RecordFailureEvent("Reconnect Fail Broker " + std::to_string(new_broker_id));
					LOG(ERROR) << "Failed to connect to replacement broker " << new_broker_id
					           << " — trying remaining survivors";
					std::vector<int> all_brokers;
					{ absl::MutexLock l(&mutex_); all_brokers = brokers_; }
					for (int fallback : all_brokers) {
						if (fallback == new_broker_id) continue;
						LOG(WARNING) << "PublishThread[" << pubQuesIdx << "]: trying fallback broker "
						             << fallback << " for queue";
						if (connect_to_server(fallback)) {
							new_broker_id = fallback;
							connected = true;
							break;
						}
						RecordFailureEvent("Reconnect Fail Broker " + std::to_string(fallback));
					}
				}
				if (!connected) {
					pubQue_.ReleaseBatch(batch_header);
					LOG(ERROR) << "All survivor brokers unreachable — thread exiting";
					{
						absl::MutexLock l(&mutex_);
						RefreshOrder5PreferredQueuesLocked();
					}
					return;
				}
			}

			std::string reconn_msg = "Reconnect Success Broker " + std::to_string(new_broker_id) + " (from " + std::to_string(broker_id) + ")";
			RecordFailureEvent(reconn_msg);

			{
				absl::MutexLock lock(&mutex_);
				ReassignQueueBrokerLocked(static_cast<size_t>(pubQuesIdx), broker_id, new_broker_id);
			}
			broker_id = new_broker_id;
			pubQue_.MarkQueueActive(pubQuesIdx);
			// [[RECONNECT_EPOCH_RESTAMP 2026-07-17]] After reconnect the broker uses
			// the epoch from the SessionOpen handshake (connection_session_epoch) to
			// validate every arriving batch header.  If the session was fenced and
			// reopened (new epoch) while this batch was in-flight, session_epoch_ has
			// already been updated by HandleSessionFenced, but batch_header->session_epoch
			// was stamped at the top of this loop iteration with the old value.
			// Re-reading session_epoch_ here closes that window: the batch we are about
			// to retransmit gets the epoch that matches the active connection.
			if (IsOrder5SessionMode()) {
				uint32_t cur_epoch = session_epoch_.load(std::memory_order_acquire);
				if (cur_epoch == 0) {
					cur_epoch = requested_session_epoch_.load(std::memory_order_acquire);
				}
				batch_header->session_epoch   = static_cast<uint16_t>(cur_epoch & 0xFFFFU);
				batch_header->session_epoch32 = cur_epoch;
			}
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

	// [[CORFU_GATE]] Normal exit: the queue is drained (or shutdown was
	// requested, in which case waiters abort via the gate's shutdown check).
	corfu_exit_guard.clean = true;

    // ScopedFd owns the ingress/epoll descriptors until this worker returns.
    VLOG(1) << "PublishThread[" << pubQuesIdx << "]: Exiting main loop; closing socket "
            << sock.get() << " publish_finished=" << publish_finished_.load()
            << ", shutdown=" << shutdown_.load();
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
    std::unique_lock<std::mutex> owner_lock(publisher_threads_mutex_);
    if (threads_joined_.load(std::memory_order_acquire) ||
        publisher_workers_stop_.load(std::memory_order_acquire) ||
        shutdown_.load(std::memory_order_acquire)) return false;
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
            try {
                threads_.emplace_back(&Publisher::PublishThread, this, broker_id, thread_idx);
            } catch (...) {
                num_threads_.fetch_sub(1);
                throw;
            }
            ++created;
			created_queue_indices.push_back(static_cast<size_t>(thread_idx));
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
        // Partial construction is terminal. Wake started workers before joining
        // them; they must not wait for future producer work during rollback.
        shutdown_.store(true, std::memory_order_release);
        pubQue_.WriteFinished();
        NotifyPublisherWork();
        unacked_cv_.notify_all();
        // Freeze ownership, then release the lifecycle lock before any join.
        threads_joined_.store(true, std::memory_order_release);
        std::vector<std::thread> failed_workers;
        failed_workers.swap(threads_);
        num_threads_.fetch_sub(static_cast<int>(created));
        owner_lock.unlock();
        for (auto& worker : failed_workers) {
            if (worker.joinable()) worker.join();
        }
		return false;
	}
	return true;
}
