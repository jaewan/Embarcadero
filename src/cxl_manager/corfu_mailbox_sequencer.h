#pragma once

// CorfuMailboxSequencer — the CXL-transport port of the Corfu global sequencer hop.
//
// This is a PURE TRANSPORT SWAP (docs/baselines/porting_rule.md): it holds a borrowed
// pointer to ONE CorfuSequencerImpl (the shared ordering state machine) and a MailboxSegment,
// and runs a single polling thread that drains CorfuTokenRequest records from every up(b)
// ring, calls CorfuSequencerImpl::AssignToken (the SAME ordering code the gRPC baseline uses),
// and posts a CorfuTokenGrant on the matching down(b) ring. The ordering logic is not
// duplicated or altered here — only the wire changes (gRPC -> CXL mailbox).
//
// SPSC contract: the sequencer thread is the SINGLE READER of every up(b) and the SINGLE
// WRITER of every down(b). Each up(b) must have exactly one writer thread (the ingress
// broker / the benchmark driver), enforced by topology; the DEBUG concurrent-writer detector
// in MailboxRing LOG(FATAL)s otherwise.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "cxl_manager/corfu_sequencer_service.h"
#include "cxl_transport/cxl_mailbox.h"

namespace Embarcadero {
namespace cxl_manager {

class CorfuMailboxSequencer {
 public:
	// Borrowed (non-owning) impl and segment. K = max requests drained per up(b) ring per
	// poll iteration before moving to the next ring (fairness bound across brokers).
	CorfuMailboxSequencer(CorfuSequencerImpl* impl,
			cxl_transport::MailboxSegment* segment, uint32_t K = 64);
	~CorfuMailboxSequencer();

	CorfuMailboxSequencer(const CorfuMailboxSequencer&) = delete;
	CorfuMailboxSequencer& operator=(const CorfuMailboxSequencer&) = delete;

	// The polling loop. May be called directly on a caller-owned thread, or via StartThread().
	// Returns after Stop() is set AND a full pass over all up(b) rings drains zero requests.
	void Run();

	// Start Run() on an internal thread (non-blocking).
	void StartThread();

	// Signal Run() to exit after draining the current backlog (non-blocking).
	void Stop();

	// Join the internal thread started by StartThread() (blocking, no-op otherwise).
	void Join();

 private:
	// Drain up to K_ requests from up(broker_id): AssignToken each and Produce the grant on
	// down(broker_id). Returns the number of requests processed this call.
	uint32_t DrainBrokerRequests(uint32_t broker_id);

	static uint32_t StatusToGrantValue(CorfuSequencerImpl::TokenStatus status);

	CorfuSequencerImpl* impl_;                  // borrowed: shared ordering state
	cxl_transport::MailboxSegment* segment_;    // borrowed: mailbox rings
	uint32_t K_;
	std::atomic<bool> stop_flag_{false};
	std::unique_ptr<std::thread> poll_thread_;
};

}  // namespace cxl_manager
}  // namespace Embarcadero
