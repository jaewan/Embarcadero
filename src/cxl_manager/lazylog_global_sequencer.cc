#include "lazylog_global_sequencer.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <glog/logging.h>

namespace {
size_t ResolveExpectedBrokers() {
  const char* brokers_env = std::getenv("EMBARCADERO_NUM_BROKERS");
  if (brokers_env && std::strlen(brokers_env) > 0) {
    try {
      const int parsed = std::stoi(brokers_env);
      if (parsed > 0) {
        return static_cast<size_t>(parsed);
      }
    } catch (const std::exception& e) {
      LOG(WARNING) << "Invalid EMBARCADERO_NUM_BROKERS='" << brokers_env
                   << "', falling back to " << NUM_MAX_BROKERS_CONFIG;
    }
  }
  return static_cast<size_t>(NUM_MAX_BROKERS_CONFIG);
}

// No artificial per-tick cap. The original value of 4096 could throttle throughput
// at lower binding intervals. Scalog's global cut has no per-tick cap either.
// Bind everything available each tick for a fair comparison.
constexpr int64_t kMaxBindingsPerBrokerPerTick = std::numeric_limits<int64_t>::max();

std::string ResolveLazyLogSequencerIp() {
  const char* sequencer_ip_env = std::getenv("EMBARCADERO_LAZYLOG_SEQ_IP");
  if (sequencer_ip_env && std::strlen(sequencer_ip_env) > 0) {
    return std::string(sequencer_ip_env);
  }

  const char* configured_ip = LAZYLOG_SEQUENCER_IP;
  if (configured_ip && std::strlen(configured_ip) > 0) {
    return std::string(configured_ip);
  }

  LOG(WARNING) << "LazyLog sequencer IP resolved empty; falling back to 127.0.0.1";
  return "127.0.0.1";
}
}  // namespace

LazyLogGlobalSequencer::LazyLogGlobalSequencer(const std::string& sequencer_address)
    : expected_brokers_(ResolveExpectedBrokers()) {
  grpc::ServerBuilder builder;
  int selected_port = 0;
  builder.AddListeningPort(sequencer_address, grpc::InsecureServerCredentials(), &selected_port);
  builder.RegisterService(this);
  server_ = builder.BuildAndStart();
  if (!server_) {
    LOG(ERROR) << "LazyLog global sequencer failed to bind " << sequencer_address;
    return;
  }
  LOG(INFO) << "LazyLog global sequencer listening on " << sequencer_address
            << " selected_port=" << selected_port
            << " expected_brokers=" << expected_brokers_;
}

void LazyLogGlobalSequencer::Run() {
  if (!server_) {
    LOG(ERROR) << "LazyLog global sequencer failed to start";
    return;
  }
  server_->Wait();
}

grpc::Status LazyLogGlobalSequencer::HandleRegisterBroker(
    grpc::ServerContext* /*context*/,
    const lazylogsequencer::RegisterBrokerRequest* request,
    lazylogsequencer::RegisterBrokerResponse* /*response*/) {
  const int broker_id = static_cast<int>(request->broker_id());
  if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS_CONFIG) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "broker_id out of range");
  }
  absl::WriterMutexLock lock(&registered_brokers_mu_);
  registered_brokers_.insert(broker_id);
  LOG(INFO) << "LazyLog sequencer registered broker=" << broker_id
            << " total_registered=" << registered_brokers_.size()
            << "/" << expected_brokers_;
  if (registered_brokers_.size() >= expected_brokers_ && !binding_thread_.joinable()) {
    binding_thread_ = std::thread(&LazyLogGlobalSequencer::SendGlobalBinding, this);
  }
  return grpc::Status::OK;
}

grpc::Status LazyLogGlobalSequencer::HandleTerminateGlobalSequencer(
    grpc::ServerContext* /*context*/,
    const lazylogsequencer::TerminateGlobalSequencerRequest* /*request*/,
    lazylogsequencer::TerminateGlobalSequencerResponse* /*response*/) {
  LOG(INFO) << "LazyLog global sequencer terminating";
  stop_reading_streams_.store(true, std::memory_order_release);
  shutdown_requested_.store(true, std::memory_order_release);
  if (binding_thread_.joinable()) {
    binding_thread_.join();
  }
  std::thread([this]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    server_->Shutdown();
  }).detach();
  return grpc::Status::OK;
}

grpc::Status LazyLogGlobalSequencer::HandleSendLocalProgress(
    grpc::ServerContext* /*context*/,
    grpc::ServerReaderWriter<lazylogsequencer::GlobalBinding, lazylogsequencer::LocalProgress>* stream) {
  {
    absl::MutexLock lock(&stream_mu_);
    local_streams_.push_back(stream);
  }
  ReceiveLocalProgress(stream);
  {
    // Remove closed stream to avoid writing into stale/dangling pointers.
    absl::MutexLock lock(&stream_mu_);
    local_streams_.erase(
        std::remove(local_streams_.begin(), local_streams_.end(), stream),
        local_streams_.end());
  }
  return grpc::Status::OK;
}

void LazyLogGlobalSequencer::ReceiveLocalProgress(
    grpc::ServerReaderWriter<lazylogsequencer::GlobalBinding, lazylogsequencer::LocalProgress>* stream) {
  while (!stop_reading_streams_.load(std::memory_order_acquire)) {
    lazylogsequencer::LocalProgress progress;
    if (!stream->Read(&progress)) {
      break;
    }
    const int broker_id = static_cast<int>(progress.broker_id());
    if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS_CONFIG) {
      LOG(WARNING) << "Ignoring local progress with invalid broker_id=" << broker_id;
      continue;
    }

    // Guard: this sequencer instance does not multiplex topics. Log a warning
    // if a second topic appears so the operator notices the misconfiguration.
    const std::string& topic = progress.topic();
    if (!topic.empty()) {
      static std::string first_topic;
      static std::once_flag once;
      std::call_once(once, [&]() { first_topic = topic; });
      if (topic != first_topic) {
        LOG_EVERY_N(WARNING, 10000) << "LazyLog global sequencer received progress for topic '"
            << topic << "' but is already tracking topic '" << first_topic
            << "'. Multi-topic multiplexing is not supported; results may be incorrect.";
      }
    }

    const int64_t current = progress.local_progress();
    absl::WriterMutexLock lock(&progress_mu_);
    const int64_t previous = last_progress_[broker_id];
    if (current < previous) {
      continue;
    }
    last_progress_[broker_id] = current;
    reported_brokers_.insert(broker_id);
  }
}

void LazyLogGlobalSequencer::SendGlobalBinding() {
  while (!shutdown_requested_.load(std::memory_order_acquire)) {
    lazylogsequencer::GlobalBinding binding;
    absl::btree_set<int> registered_snapshot;
    {
      absl::ReaderMutexLock lock(&registered_brokers_mu_);
      registered_snapshot = registered_brokers_;
    }
    bool ready_to_bind = false;
    {
      absl::WriterMutexLock lock(&progress_mu_);
      // LazyLog binding starts only after all brokers have registered and reported progress at least once.
      if (registered_snapshot.size() >= expected_brokers_ &&
          reported_brokers_.size() >= expected_brokers_) {
        ready_to_bind = true;
        for (int broker_id : registered_snapshot) {
          const int64_t reported = last_progress_[broker_id];
          const int64_t already_bound = bound_progress_[broker_id];
          if (reported <= already_bound) {
            continue;
          }

          const int64_t available = reported - already_bound;
          const int64_t bind_now = std::min<int64_t>(available, kMaxBindingsPerBrokerPerTick);
          if (bind_now <= 0) {
            continue;
          }
          binding.mutable_global_binding()->insert({broker_id, bind_now});
          bound_progress_[broker_id] = already_bound + bind_now;
        }
      }
    }

    if (ready_to_bind && !binding.global_binding().empty()) {
      absl::MutexLock lock(&stream_mu_);
      for (auto it = local_streams_.begin(); it != local_streams_.end(); ) {
        if (*it && (*it)->Write(binding)) {
          ++it;
        } else {
          it = local_streams_.erase(it);
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(LAZYLOG_SEQ_LOCAL_CUT_INTERVAL));
  }
}

int main(int argc, char* argv[]) {
  google::InitGoogleLogging(argv[0]);
  const char* sequencer_port_env = std::getenv("EMBARCADERO_LAZYLOG_SEQ_PORT");
  const std::string sequencer_ip = ResolveLazyLogSequencerIp();
  int sequencer_port = LAZYLOG_SEQ_PORT;
  if (sequencer_port_env && std::strlen(sequencer_port_env) > 0) {
    try {
      sequencer_port = std::stoi(sequencer_port_env);
    } catch (const std::exception& e) {
      LOG(WARNING) << "Invalid EMBARCADERO_LAZYLOG_SEQ_PORT='" << sequencer_port_env
                   << "', falling back to " << LAZYLOG_SEQ_PORT;
    }
  }
  std::string sequencer_addr = sequencer_ip + ":" + std::to_string(sequencer_port);
  LazyLogGlobalSequencer sequencer(sequencer_addr);
  sequencer.Run();
  return 0;
}
