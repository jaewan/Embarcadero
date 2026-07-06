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

	// Transport-independent result of the token-assignment core. GetTotalOrder maps this
	// onto grpc::Status; the CXL mailbox port (CorfuMailboxSequencer) maps it onto the
	// CorfuTokenGrant.status field. Same values, same ordering logic, only the wire differs.
	enum class TokenStatus { OK, INVALID_ARGUMENT, ALREADY_PROCESSED, OUT_OF_ORDER };

	// Transport-independent token-assignment core. This is steps (1)-(5) of the Corfu
	// sequencer protocol (validate -> per-client FIFO lock -> per-(client,broker) state
	// lock -> expected_batch_seq gate -> monotonic counter fetch_adds + store_fence),
	// extracted verbatim from GetTotalOrder so the gRPC baseline and the CXL mailbox port
	// use LITERALLY the same ordering code (strongest fairness). Takes raw integers and
	// returns a TokenStatus enum — NO proto/gRPC types.
	//
	// out_total_order / out_log_idx / out_broker_batch_seq are written ONLY when the
	// returned status is TokenStatus::OK; on any error status they are left untouched and
	// the caller MUST NOT read them.
	TokenStatus AssignToken(uint64_t client_id, uint64_t batch_seq, uint64_t num_msg,
			uint64_t total_size, int broker_id, uint64_t* out_total_order,
			uint64_t* out_log_idx, uint64_t* out_broker_batch_seq);

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
