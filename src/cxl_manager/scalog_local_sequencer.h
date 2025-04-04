#ifndef SCALOG_LOCAL_SEQUENCER_H
#define SCALOG_LOCAL_SEQUENCER_H

#include "cxl_manager.h"
#include "common/config.h"
#include <scalog_sequencer.grpc.pb.h>

namespace Scalog {

class CXLManager;
using Embarcadero::TInode;
using Embarcadero::MessageHeader;

class ScalogLocalSequencer {
	public:
		ScalogLocalSequencer(Embarcadero::CXLManager* cxl_manager, int broker_id, void* cxl_addr);

		/// Sends a register request to the global sequencer
		void Register();

		/// Send a local cut to the global seq after every interval
		void SendLocalCut(std::string topic_str);

		/// Sends a request to global sequencer to terminate itself
		void TerminateGlobalSequencer();

		/// Receives the global cut from the global sequencer
		void ReceiveGlobalCut(std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>>& stream, std::string topic_str);

		/// Receives the global cut from the head node
		/// This function is called in the callback of the send local cut grpc call
		void ScalogSequencer(const char* topic, absl::btree_map<int, int> &global_cut);

	private:
		Embarcadero::CXLManager* cxl_manager_;
		int broker_id_;
		int replica_id_ = 0;
		void* cxl_addr_;
		std::unique_ptr<ScalogSequencer::Stub> stub_;

		/// Time between each local cut
		std::chrono::microseconds local_cut_interval_ = std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL);

		/// Map of broker_id to local cut
		absl::btree_map<int, int> global_cut_;

		/// Local epoch
		int local_epoch_ = 0;

		// Global seq ip
		std::string scalog_global_sequencer_ip_ = "128.110.219.89";

		/// Flag to indicate if we should stop reading from the stream
		bool stop_reading_from_stream_ = false;

		/// Lock for streams
		absl::Mutex stream_mu_;
};

} // End of namespace Scalog
#endif