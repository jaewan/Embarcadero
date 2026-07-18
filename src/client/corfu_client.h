#include "corfu_token_proxy.grpc.pb.h"
#include "common/config.h"
#include "../common/performance_utils.h"

#include <grpcpp/grpcpp.h>
#include <glog/logging.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using corfutokenproxy::CorfuTokenProxy;
using corfutokenproxy::TokenRequest;
using corfutokenproxy::TokenResponse;

class CorfuSequencerClient {
	public:
		explicit CorfuSequencerClient(uint64_t client_id) : client_id_(client_id) {}

		// Structured retry counters for the [CORFU_TOKEN_PHASE] evaluation line
		// (invariant C10). Retries are idempotent end-to-end: the ingress proxy
		// dedupes by (client_id, broker_id, batchseq) and single-flights the
		// upstream hop, so a retry can only observe the original grant or a
		// terminal rejection — never allocate a second token range (C4).
		uint64_t UnavailableRetries() const {
			return unavailable_retries_.load(std::memory_order_relaxed);
		}
		uint64_t TransientRetries() const {
			return transient_retries_.load(std::memory_order_relaxed);
		}

		// Terminal cancellation used by the publisher's fail-closed gate.  It
		// cancels the active RPC and prevents a retry from registering after an
		// abort raced with the previous attempt's completion.
		void CancelActiveRequests() {
			std::lock_guard<std::mutex> lock(active_contexts_mu_);
			cancelled_.store(true, std::memory_order_release);
			for (ClientContext* context : active_contexts_) context->TryCancel();
		}

		// Get total order for a batch of messages
		bool GetTotalOrder(Embarcadero::BatchHeader *batch_header,
				const std::string& proxy_endpoint){
			// The response rewrites batch_seq to the broker sequence.  Preserve the
			// original client sequence before that mutation for Corfu ValueId.
			batch_header->original_client_batch_seq = batch_header->batch_seq;
			TokenRequest request;
			request.set_client_id(client_id_);
			request.set_batchseq(batch_header->batch_seq);
			request.set_num_msg(batch_header->num_msg);
			request.set_total_size(batch_header->total_size);
			request.set_broker_id(batch_header->broker_id);

			int transient_retry_count = 0;
			int unavailable_retry_count = 0;
			const auto start = std::chrono::steady_clock::now();
			constexpr auto kUnavailableRetryBudget = std::chrono::seconds(15);
			constexpr int kUnavailableLogInterval = 1000;
			while (true) {
				TokenResponse response;
				ClientContext context;
				// Set a reasonable timeout for the RPC
				context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

				// Reuse a persistent channel/stub per proxy endpoint. Creating a
				// channel on every token (~1 ms) was the dominant Corfu N=1 tax
				// (batch_bytes / mean_token_latency ≈ measured GB/s).
				CorfuTokenProxy::Stub* stub = StubFor(proxy_endpoint);
				if (!RegisterActiveContext(&context)) {
					LOG(ERROR) << "GetTotalOrder cancelled by terminal publisher abort";
					return false;
				}
				Status status = stub->GetTotalOrder(&context, request, &response);
				UnregisterActiveContext(&context);

				if (status.ok()) {
					batch_header->total_order = response.total_order();
					batch_header->log_idx = response.log_idx();
					batch_header->batch_seq = response.broker_batch_seq();
					return true;
				}

				if (status.error_code() == grpc::StatusCode::UNAVAILABLE) {
					// Drop the cached stub so the next attempt reconnects.
					InvalidateStub(proxy_endpoint);
					unavailable_retry_count++;
					unavailable_retries_.fetch_add(1, std::memory_order_relaxed);
					if (unavailable_retry_count == 1) {
						LOG(WARNING) << "GetTotalOrder UNAVAILABLE on first attempt"
						             << " client_id=" << client_id_
						             << " broker_id=" << batch_header->broker_id
						             << " request_batch_seq=" << request.batchseq()
						             << " msg_count=" << batch_header->num_msg
						             << " total_size=" << batch_header->total_size
						             << " error=\"" << status.error_message() << "\"";
					}
					if ((unavailable_retry_count % kUnavailableLogInterval) == 0) {
						const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now() - start).count();
						LOG(WARNING) << "GetTotalOrder still retrying after UNAVAILABLE"
						             << " client_id=" << client_id_
						             << " broker_id=" << batch_header->broker_id
						             << " request_batch_seq=" << request.batchseq()
						             << " retries=" << unavailable_retry_count
						             << " waited_ms=" << waited_ms
						             << " msg_count=" << batch_header->num_msg
						             << " total_size=" << batch_header->total_size
						             << " error=\"" << status.error_message() << "\"";
					}
					if (std::chrono::steady_clock::now() - start >= kUnavailableRetryBudget) {
						LOG(ERROR) << "GetTotalOrder giving up after repeated UNAVAILABLE"
						           << " client_id=" << client_id_
						           << " broker_id=" << batch_header->broker_id
						           << " request_batch_seq=" << request.batchseq()
						           << " retries=" << unavailable_retry_count
						           << " waited_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
						                   std::chrono::steady_clock::now() - start).count()
						           << " error=\"" << status.error_message() << "\"";
						return false;
					}
					std::this_thread::yield();
					continue;
				}

				// [[PHASE_8]] Retry on other transient errors (e.g. DEADLINE_EXCEEDED).
				// Safe under ambiguous completion: the retry carries the SAME
				// (client_id, broker_id, batchseq), so the ingress proxy either
				// returns the cached/in-flight original grant or fails terminally
				// ("already processed" after cache eviction) — it cannot allocate a
				// second range for this batch (C4/C8).
				if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
				    status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED) {
					if (++transient_retry_count < 3) {
						transient_retries_.fetch_add(1, std::memory_order_relaxed);
						LOG(WARNING) << "GetTotalOrder transient error: " << status.error_message() << ". Retrying...";
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
						continue;
					}
				}

				LOG(ERROR) << "GetTotalOrder failed: " << status.error_message()
				           << " (code=" << status.error_code() << ")"
				           << " client_id=" << client_id_
				           << " broker_id=" << batch_header->broker_id
				           << " request_batch_seq=" << request.batchseq();
				return false;
			}
		}

	private:
		bool RegisterActiveContext(ClientContext* context) {
			std::lock_guard<std::mutex> lock(active_contexts_mu_);
			if (cancelled_.load(std::memory_order_acquire)) return false;
			active_contexts_.insert(context);
			return true;
		}

		void UnregisterActiveContext(ClientContext* context) {
			std::lock_guard<std::mutex> lock(active_contexts_mu_);
			active_contexts_.erase(context);
		}

		CorfuTokenProxy::Stub* StubFor(const std::string& proxy_endpoint) {
			std::lock_guard<std::mutex> lock(stubs_mu_);
			auto it = stubs_.find(proxy_endpoint);
			if (it != stubs_.end()) {
				return it->second.get();
			}
			auto channel = grpc::CreateChannel(proxy_endpoint, grpc::InsecureChannelCredentials());
			auto stub = CorfuTokenProxy::NewStub(channel);
			CorfuTokenProxy::Stub* raw = stub.get();
			stubs_.emplace(proxy_endpoint, std::move(stub));
			return raw;
		}

		void InvalidateStub(const std::string& proxy_endpoint) {
			std::lock_guard<std::mutex> lock(stubs_mu_);
			stubs_.erase(proxy_endpoint);
		}

		const uint64_t client_id_;
		std::mutex stubs_mu_;
		std::unordered_map<std::string, std::unique_ptr<CorfuTokenProxy::Stub>> stubs_;
		std::mutex active_contexts_mu_;
		std::unordered_set<ClientContext*> active_contexts_;
		std::atomic<bool> cancelled_{false};
		std::atomic<uint64_t> unavailable_retries_{0};
		std::atomic<uint64_t> transient_retries_{0};
};
