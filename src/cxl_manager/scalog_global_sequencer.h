#include <grpcpp/grpcpp.h>
#include <condition_variable>
#include <glog/logging.h>
#include "absl/container/flat_hash_map.h"
#include "absl/container/btree_set.h"
#include "absl/container/btree_map.h"
#include <scalog_sequencer.grpc.pb.h>
#include "common/config.h"
#include <thread>

class ScalogGlobalSequencer : public ScalogSequencer::Service {
    public:
        ScalogGlobalSequencer(std::string scalog_seq_address);

        void SendGlobalCut();

        /// Receives a local cut from a local sequencer
            /// @param request Request containing the local cut and the epoch
            /// @param response Empty for now
        grpc::Status HandleSendLocalCut(grpc::ServerContext* context, grpc::ServerReaderWriter<GlobalCut, LocalCut>* stream);

        /// Receives a register request from a local sequencer
            /// @param request Request containing the broker id
            /// @param response Empty for now
        grpc::Status HandleRegisterBroker(grpc::ServerContext* context, const RegisterBrokerRequest* request, RegisterBrokerResponse* response);

        /// Receives a terminate request from a local sequencer
            /// @param request Empty for now
            /// @param response Empty for now
        grpc::Status HandleTerminateGlobalSequencer(grpc::ServerContext* context, const TerminateGlobalSequencerRequest* request, TerminateGlobalSequencerResponse* response);

        /// Keep track of the global cut and if all the local cuts have been received
		void ReceiveLocalCut(grpc::ServerReaderWriter<GlobalCut, LocalCut>* stream);
    private:
		/// The head node keeps track of the global epoch and increments it whenever we complete a round of local cuts
		int global_epoch_;

        std::unique_ptr<grpc::Server> scalog_server_;

        /// Used in ReceiveLocalCut() so we receive local cuts one at a time
		std::mutex mutex_;

		/// Used in ReceiveLocalCut() to wait for all local cuts to be received
		std::condition_variable cv_;
		std::condition_variable reset_cv_;

		/// Map of broker_id to local cut
		absl::Mutex global_cut_mu_;
		absl::btree_map<int, int> global_cut_ ABSL_GUARDED_BY(global_cut_mu_);

		/// Used to keep track of # messages of each epoch so we can calculate the global cut
		/// Map of broker_id to logical offset
		absl::btree_map<int, int> logical_offsets_ ABSL_GUARDED_BY(global_cut_mu_);

        /// Lock needed to read and write to registered_brokers_
        absl::Mutex registered_brokers_mu_;

        /// Used to keep track of all registered brokers
        /// Each element is a broker_id
        absl::btree_set<int> registered_brokers_;

        /// Flag to indicate shutdown request
        std::atomic<bool> shutdown_requested_{false};

        /// Flag to indicate if we should stop reading from the stream
        std::atomic<bool> stop_reading_from_stream_{false};

        /// Time between sending global cut
		std::chrono::microseconds global_cut_interval_ = std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL);

        /// Stream to send global cut to all local sequencers
        std::vector<std::shared_ptr<grpc::ServerReaderWriter<GlobalCut, LocalCut>>> local_sequencers_;

        /// Mutex to protect local_sequencers_
        absl::Mutex stream_mu_;
};