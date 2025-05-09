#include "corfu_sequencer.grpc.pb.h"
#include "common/config.h"

#include <grpcpp/grpcpp.h>
#include <glog/logging.h>
#include <vector>
#include <memory>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using corfusequencer::CorfuSequencer;
using corfusequencer::TotalOrderRequest;
using corfusequencer::TotalOrderResponse;

/*
	// Create client
	std::string server_address = "localhost:" + std::to_string(CORFU_SEQ_PORT);
	CorfuSequencerClient client(server_address);

Corfu in a nutshell
Corfu is a scalable, fault-tolerant, and consistent data store based on the concept of a linearizable log. It uses a chain replication approach where writes are sequenced and then broadcasted to all replicas. Here's a simplified explanation of the publish sequence with multiple brokers:

1. Client Request:

The client sends a write request to one of the brokers along with a unique client ID and a sequence number.
The client sequence number ensures that requests from the same client are applied in order.
2. Broker Assignment:

Brokers are organized into a consistent hashing ring. Each broker is responsible for a specific range of keys.
The broker that receives the client request determines the key associated with the request and forwards it to the "leader" broker responsible for that key range.
3. Leader Sequencing:

The leader broker receives the write request and assigns it a globally unique sequence number using a sequencer.
The original Corfu paper utilizes a separate Paxos group for sequencing, while vCorfu optimizes this using a dedicated sequencer role within the key's replica group.
This sequence number determines the order in which the write will be applied across all replicas.
4. Log Replication:

The leader appends the write request (with its assigned sequence number) to its local log.
The leader propagates the sequenced write to other brokers responsible for that key range (followers) using a reliable broadcast protocol (e.g., chain replication).
5. Follower Application:

Followers receive the sequenced write from the leader and append it to their local logs in the same order determined by the sequence number.
Once a write is appended to the log, it is considered durable and can be read by clients.
6. Client Confirmation:

Once the write is successfully replicated to a quorum of followers, the leader sends a confirmation to the client.
The client can be sure that the write is durable and will be applied in the sequence determined by the sequencer.
7. Read Operations:

Read requests are handled by the broker responsible for the key.
The broker reads the latest state of the data from its local log, ensuring that all preceding writes (based on sequence number) have been applied.
Benefits of this approach:

Total Order: The sequencer ensures that all writes are applied in the same order across all replicas, guaranteeing strong consistency.
Scalability: Distributing data across multiple brokers using consistent hashing allows Corfu to scale horizontally.
Fault Tolerance: Replication and the use of a quorum for write confirmation ensures data durability and availability even if some brokers fail.
	*/

class CorfuSequencerClient {
	public:
		CorfuSequencerClient(const std::string& server_address) 
			: stub_(CorfuSequencer::NewStub(
						grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials()))),
			client_id_(GenerateClientId()){}

		// Get total order for a batch of messages
		bool GetTotalOrder(Embarcadero::BatchHeader *batch_header){
		//, size_t batch_seq, size_t num_msg, size_t total_size, int broker_id, std::vector<uint64_t>& total_orders) {
			TotalOrderRequest request;
			request.set_client_id(client_id_);
			request.set_batchseq(batch_header->batch_seq);
			request.set_num_msg(batch_header->num_msg);
			request.set_total_size(batch_header->total_size);
			request.set_broker_id(batch_header->broker_id);

			TotalOrderResponse response;
			ClientContext context;

			Status status = stub_->GetTotalOrder(&context, request, &response);

			if (!status.ok()) {
				LOG(ERROR) << "GetTotalOrder failed: " << status.error_message();
				return false;
			}

			batch_header->total_order = response.total_order();
			batch_header->log_idx = response.log_idx();
			batch_header->batch_seq = response.broker_batch_seq();

			return true;
		}

	private:
		static size_t GenerateClientId() {
			// Simple implementation - you might want to make this more sophisticated
			static std::atomic<size_t> next_id(0);
			return next_id++;
		}

		std::unique_ptr<CorfuSequencer::Stub> stub_;
		const size_t client_id_;
};
