#ifndef LAZYLOG_GLOBAL_SEQUENCER_H
#define LAZYLOG_GLOBAL_SEQUENCER_H

#include <condition_variable>
#include <thread>
#include <vector>
#include <grpcpp/grpcpp.h>
#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/synchronization/mutex.h"
#include <lazylog_sequencer.grpc.pb.h>
#include "common/config.h"

class LazyLogGlobalSequencer : public lazylogsequencer::LazyLogSequencer::Service {
 public:
  explicit LazyLogGlobalSequencer(const std::string& sequencer_address);
  void Run();

  grpc::Status HandleSendLocalProgress(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<lazylogsequencer::GlobalBinding, lazylogsequencer::LocalProgress>* stream) override;

  grpc::Status HandleRegisterBroker(
      grpc::ServerContext* context,
      const lazylogsequencer::RegisterBrokerRequest* request,
      lazylogsequencer::RegisterBrokerResponse* response) override;

  grpc::Status HandleTerminateGlobalSequencer(
      grpc::ServerContext* context,
      const lazylogsequencer::TerminateGlobalSequencerRequest* request,
      lazylogsequencer::TerminateGlobalSequencerResponse* response) override;

 private:
  void ReceiveLocalProgress(
      grpc::ServerReaderWriter<lazylogsequencer::GlobalBinding, lazylogsequencer::LocalProgress>* stream);
  void SendGlobalBinding();

  std::unique_ptr<grpc::Server> server_;
  std::thread binding_thread_;
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> stop_reading_streams_{false};
  size_t expected_brokers_;

  absl::Mutex registered_brokers_mu_;
  absl::btree_set<int> registered_brokers_ ABSL_GUARDED_BY(registered_brokers_mu_);

  absl::Mutex stream_mu_;
  std::vector<grpc::ServerReaderWriter<lazylogsequencer::GlobalBinding, lazylogsequencer::LocalProgress>*> local_streams_
      ABSL_GUARDED_BY(stream_mu_);

  absl::Mutex progress_mu_;
  absl::btree_map<int, int64_t> last_progress_ ABSL_GUARDED_BY(progress_mu_);
  absl::btree_map<int, int64_t> pending_binding_ ABSL_GUARDED_BY(progress_mu_);
};

#endif
