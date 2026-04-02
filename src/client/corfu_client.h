#include "corfu_sequencer.grpc.pb.h"
#include "common/config.h"
#include "../common/performance_utils.h"

#include <grpcpp/grpcpp.h>
#include <glog/logging.h>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using corfusequencer::CorfuSequencer;
using corfusequencer::TotalOrderRequest;
using corfusequencer::TotalOrderResponse;

class CorfuSequencerClient {
	public:
		CorfuSequencerClient(const std::string& server_address, uint64_t client_id) 
			: stub_(CorfuSequencer::NewStub(
					grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials()))),
			client_id_(client_id){}

		// Get total order for a batch of messages
		bool GetTotalOrder(Embarcadero::BatchHeader *batch_header){
			TotalOrderRequest request;
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
				TotalOrderResponse response;
				ClientContext context;
				// Set a reasonable timeout for the RPC
				context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

				Status status = stub_->GetTotalOrder(&context, request, &response);

				if (status.ok()) {
					batch_header->total_order = response.total_order();
					batch_header->log_idx = response.log_idx();
					batch_header->batch_seq = response.broker_batch_seq();
					return true;
				}

				if (status.error_code() == grpc::StatusCode::UNAVAILABLE) {
					unavailable_retry_count++;
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

				// [[PHASE_8]] Retry on other transient errors (e.g. DEADLINE_EXCEEDED)
				if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
				    status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED) {
					if (++transient_retry_count < 3) {
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
		std::unique_ptr<CorfuSequencer::Stub> stub_;
		const uint64_t client_id_;
};
