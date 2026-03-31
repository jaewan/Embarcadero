#ifndef SCALOG_LOCAL_SEQUENCER_H
#define SCALOG_LOCAL_SEQUENCER_H

#include "common/config.h"
#include "cxl_datastructure.h"
#include <scalog_sequencer.grpc.pb.h>
#include <atomic>
#include <cstdint>

namespace Embarcadero{
	class CXLManager;
}

namespace Scalog {

using Embarcadero::TInode;
using Embarcadero::MessageHeader;
using Embarcadero::BatchHeader;

class ScalogLocalSequencer {
	public:
		ScalogLocalSequencer(TInode* tinode, int broker_id, 
				void* cxl_addr, std::string topic_str, BatchHeader *batch_header);

		/// Sends a register request to the global sequencer
		void Register(int replication_factor);

		/// Send a local cut to the global seq after every interval
		void SendLocalCut(std::string topic_str, volatile bool& stop_thread);

		/// Sends a request to global sequencer to terminate itself
		void TerminateGlobalSequencer();

		/// Receives the global cut from the global sequencer
		void ReceiveGlobalCut(std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>>& stream, std::string topic_str);

		/// Receives the global cut from the head node.
		/// The input map is a per-broker delta (not cumulative).
		void ScalogSequencer(const char* topic, absl::btree_map<int, int64_t> &global_cut_delta);

	private:
		TInode* tinode_;
		int broker_id_;
		int replica_id_ = 0;
		void* cxl_addr_;
		BatchHeader* batch_header_;
		std::unique_ptr<ScalogSequencer::Stub> stub_;
		size_t seq_ = 0;
		MessageHeader* msg_to_order_ = nullptr;
		size_t batch_header_idx_ = 0;

		/// Last cumulative global cut received from global sequencer (broker_id -> cumulative count).
		absl::btree_map<int, int64_t> global_cut_;

		/// Last cumulative global cut already applied by this local sequencer.
		/// Used to convert cumulative wire format into exactly-once per-broker deltas.
		absl::btree_map<int, int64_t> last_applied_global_cut_;

		/// Local epoch
		int local_epoch_ = 0;

		// Global seq ip
		std::string scalog_global_sequencer_ip_ = SCALOG_SEQUENCER_IP;

		/// Flag to indicate if we should stop reading from the stream
		std::atomic<bool> stop_reading_from_stream_{false};
};

} // End of namespace Scalog
#endif
