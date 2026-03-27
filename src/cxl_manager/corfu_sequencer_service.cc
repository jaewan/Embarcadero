#include "cxl_manager/corfu_sequencer_service.h"

#include "../common/performance_utils.h"

CorfuSequencerImpl::CorfuSequencerImpl() {
	for (auto& v : idx_per_broker_) {
		v.store(0, std::memory_order_relaxed);
	}
	for (auto& v : batch_seq_per_broker_) {
		v.store(0, std::memory_order_relaxed);
	}
}

grpc::Status CorfuSequencerImpl::GetTotalOrder(grpc::ServerContext* context,
		const corfusequencer::TotalOrderRequest* request,
		corfusequencer::TotalOrderResponse* response) {
	(void)context;
	const uint64_t client_id = request->client_id();
	const uint64_t batch_seq = request->batchseq();
	const uint64_t num_msg = request->num_msg();
	const uint64_t total_size = request->total_size();
	const int broker_id = static_cast<int>(request->broker_id());

	if (broker_id < 0 || broker_id >= kMaxBrokers) {
		return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "broker_id out of range");
	}
	if (num_msg == 0 || total_size == 0) {
		return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid num_msg or total_size");
	}

	// Serialize token issue per client_id so global total_order respects a single FIFO for
	// this client across all brokers (matches Appendix: sequencer observes client order).
	// Lock order: client_order mutex, then per-(client,broker) stream mutex — always this order.
	std::mutex* client_fifo_mu = GetOrCreateClientOrderMutex(client_id);
	std::unique_lock<std::mutex> client_fifo_lock(*client_fifo_mu);

	const uint64_t client_broker_key =
			(static_cast<uint64_t>(client_id) << 32) | static_cast<uint64_t>(broker_id);

	ClientBrokerState* state = GetOrCreateState(client_broker_key);

	std::unique_lock<std::mutex> broker_stream_lock(state->mu);

	if (batch_seq < state->expected_batch_seq) {
		return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Batch sequence already processed");
	}

	if (batch_seq != state->expected_batch_seq) {
		return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Out of order batch sequence, please retry");
	}

	uint64_t start_order = next_order_.fetch_add(num_msg, std::memory_order_relaxed);
	uint64_t log_offset = idx_per_broker_[broker_id].fetch_add(total_size, std::memory_order_relaxed);
	uint64_t bbseq = batch_seq_per_broker_[broker_id].fetch_add(1, std::memory_order_relaxed);

	state->expected_batch_seq++;

	response->set_total_order(start_order);
	response->set_log_idx(log_offset);
	response->set_broker_batch_seq(bbseq);

	Embarcadero::CXL::store_fence();

	return grpc::Status::OK;
}

CorfuSequencerImpl::ClientBrokerState* CorfuSequencerImpl::GetOrCreateState(uint64_t key) {
	{
		std::shared_lock<std::shared_mutex> rlock(client_map_mu_);
		auto it = client_state_.find(key);
		if (it != client_state_.end()) {
			return it->second.get();
		}
	}
	{
		std::unique_lock<std::shared_mutex> wlock(client_map_mu_);
		auto [it, inserted] = client_state_.emplace(key, nullptr);
		if (inserted) {
			it->second = std::make_unique<ClientBrokerState>();
		}
		return it->second.get();
	}
}

std::mutex* CorfuSequencerImpl::GetOrCreateClientOrderMutex(uint64_t client_id) {
	{
		std::shared_lock<std::shared_mutex> rlock(client_order_map_mu_);
		auto it = client_order_mutexes_.find(client_id);
		if (it != client_order_mutexes_.end()) {
			return it->second.get();
		}
	}
	{
		std::unique_lock<std::shared_mutex> wlock(client_order_map_mu_);
		auto [it, inserted] = client_order_mutexes_.emplace(client_id, nullptr);
		if (inserted) {
			it->second = std::make_unique<std::mutex>();
		}
		return it->second.get();
	}
}
