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

		/// Called when first starting the scalog local sequencer. It manages
		/// the timing between each local cut
		void LocalSequencer(std::string topic_str);

		/// Sends a local cut to the global sequencer
		void SendLocalCut(int local_cut, const char* topic);

		/// Sends a register request to the global sequencer
		void Register();

		/// Sends a request to global sequencer to terminate itself
		void TerminateGlobalSequencer();

		/// Receives the global cut from the head node
		/// This function is called in the callback of the send local cut grpc call
		void ScalogSequencer(const char* topic, absl::flat_hash_map<int, absl::btree_map<int, int>> &global_cut);

		void SetEpochToOrder(int epoch){
			epoch_to_order_ = epoch;
		}

	private:
		Embarcadero::CXLManager* cxl_manager_;
		int broker_id_;
		void* cxl_addr_;
		std::unique_ptr<ScalogSequencer::Stub> stub_;

		/// Time between each local cut
		std::chrono::microseconds local_cut_interval_ = std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL);

		/// The key is the current epoch and it contains another map of broker_id to local cut
		absl::flat_hash_map<int, absl::btree_map<int, int>> global_cut_;

		/// Local epoch
		int local_epoch_ = 0;

		/// Epoch to order
		int epoch_to_order_ = 0;

		// Global seq ip
		std::string scalog_global_sequencer_ip_ = "128.110.219.89";
};

} // End of namespace Scalog
#endif