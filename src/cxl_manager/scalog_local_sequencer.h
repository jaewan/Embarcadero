#ifndef SCALOG_LOCAL_SEQUENCER_H
#define SCALOG_LOCAL_SEQUENCER_H

#include "common/config.h"
#include "cxl_datastructure.h"
#include <scalog_sequencer.grpc.pb.h>
#include <atomic>

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
		void SendLocalCut(std::string topic_str, std::atomic<bool>& stop_thread);

		/// Sends a request to global sequencer to terminate itself
		void TerminateGlobalSequencer();

		/// Receives the global cut from the global sequencer
		void ReceiveGlobalCut(std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>>& stream, std::string topic_str);

		/// Receives the global cut from the head node
		/// This function is called in the callback of the send local cut grpc call
		void ScalogSequencer(const char* topic, absl::btree_map<int, int> &global_cut);

	private:
		TInode* tinode_;
		int broker_id_;
		int replica_id_ = 0;
		void* cxl_addr_;
		BatchHeader* batch_header_;
		std::unique_ptr<ScalogSequencer::Stub> stub_;

		/// Map of broker_id to local cut
		absl::btree_map<int, int> global_cut_;

		/// Local epoch
		int local_epoch_ = 0;

		// Global seq ip
		std::string scalog_global_sequencer_ip_ = SCLAOG_SEQUENCER_IP;

		/// Flag to indicate if we should stop reading from the stream
		bool stop_reading_from_stream_ = false;

		/// Lock for streams
		absl::Mutex stream_mu_;
};

} // End of namespace Scalog
#endif
