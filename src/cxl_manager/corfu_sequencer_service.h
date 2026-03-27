#pragma once

#include "corfu_sequencer.grpc.pb.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <grpcpp/grpcpp.h>

#include "absl/container/flat_hash_map.h"

// Corfu global sequencer gRPC service. Token assignment uses one mutex per client_id so
// total_order is a valid shuffle of per-client submission order even when a client fans
// out to multiple brokers (multiple concurrent GetTotalOrder RPCs).
class CorfuSequencerImpl final : public corfusequencer::CorfuSequencer::Service {
 public:
	CorfuSequencerImpl();

	grpc::Status GetTotalOrder(grpc::ServerContext* context,
			const corfusequencer::TotalOrderRequest* request,
			corfusequencer::TotalOrderResponse* response) override;

 private:
	// Must match NUM_MAX_BROKERS / broker_id range in the rest of the stack.
	static constexpr int kMaxBrokers = 32;

	struct alignas(64) ClientBrokerState {
		std::mutex mu;
		uint64_t expected_batch_seq{0};
	};

	std::atomic<uint64_t> next_order_{0};
	std::array<std::atomic<uint64_t>, kMaxBrokers> idx_per_broker_{};
	std::array<std::atomic<uint64_t>, kMaxBrokers> batch_seq_per_broker_{};

	std::shared_mutex client_map_mu_;
	absl::flat_hash_map<uint64_t, std::unique_ptr<ClientBrokerState>> client_state_;

	std::shared_mutex client_order_map_mu_;
	absl::flat_hash_map<uint64_t, std::unique_ptr<std::mutex>> client_order_mutexes_;

	ClientBrokerState* GetOrCreateState(uint64_t key);
	std::mutex* GetOrCreateClientOrderMutex(uint64_t client_id);
};
