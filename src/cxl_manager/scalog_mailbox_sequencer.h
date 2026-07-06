#pragma once

// ScalogMailboxSequencer — the CXL-transport port of the Scalog global-sequencer hop.
//
// This is a PURE TRANSPORT SWAP (docs/baselines/porting_rule.md): it holds a borrowed
// pointer to ONE ScalogGlobalOrderingCore (the shared min-across-replicas-per-shard ordering
// state, also used by the gRPC baseline) and a MailboxSegment, and runs a single polling
// thread that drains ScalogLocalCutMsg records from every up(b) ring, calls
// ScalogGlobalOrderingCore::AddLocalCut (the SAME accumulation code the gRPC baseline uses),
// and — once the whole replica set has reported (the SAME readiness gate the gRPC
// SendGlobalCut uses) — computes the GlobalCut via the core and delivers it to every down(b)
// with BroadcastDown. The ordering logic is not duplicated or altered here; only the wire
// changes (gRPC bidi stream -> CXL mailbox up-rings + BroadcastDown).
//
// SPSC contract: the sequencer thread is the SINGLE READER of every up(b) and the SINGLE
// WRITER of every down(b). Each up(b) must have exactly one writer thread (the broker's
// local sequencer / the benchmark driver), enforced by topology; the DEBUG concurrent-writer
// detector in MailboxRing LOG(FATAL)s otherwise.
//
// Wedged-broker safety: GlobalCut delivery uses MailboxSegment::BroadcastDown, which makes
// ONE bounded, decoupled pass and returns per-ring BroadcastStatus. A broker that never
// drains its down(b) ring (WEDGED) does not block delivery to the others and does not hang
// Stop()/Join() — the sequencer logs the wedged ring and moves on (the cut is re-broadcast
// on the next ready epoch, since cuts are cumulative/idempotent).

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "cxl_manager/scalog_global_ordering_core.h"
#include "cxl_transport/cxl_mailbox.h"

namespace Embarcadero {
namespace cxl_manager {

class ScalogMailboxSequencer {
 public:
	// Borrowed (non-owning) core and segment. K = max LocalCut records drained per up(b)
	// ring per poll iteration before moving to the next ring (fairness bound across brokers).
	ScalogMailboxSequencer(ScalogGlobalOrderingCore* core,
			cxl_transport::MailboxSegment* segment, uint32_t K = 64);
	~ScalogMailboxSequencer();

	ScalogMailboxSequencer(const ScalogMailboxSequencer&) = delete;
	ScalogMailboxSequencer& operator=(const ScalogMailboxSequencer&) = delete;

	// The polling loop. May be called directly on a caller-owned thread, or via StartThread().
	// Returns after Stop() is set AND a full pass over all up(b) rings drains zero records.
	void Run();

	// Start Run() on an internal thread (non-blocking).
	void StartThread();

	// Signal Run() to exit after draining the current backlog (non-blocking).
	void Stop();

	// Join the internal thread started by StartThread() (blocking, no-op otherwise).
	void Join();

 private:
	// Drain up to K_ ScalogLocalCutMsg records from up(broker_id) into the core. Returns the
	// number of records processed this call.
	uint32_t DrainBrokerCuts(uint32_t broker_id);

	// If the whole replica set has reported AND is aligned on one epoch (core readiness +
	// epoch barrier), compute the per-broker MIN-across-replicas global cut, encode it tagged
	// with that aligned DATA epoch, and BroadcastDown to every down(b). Advances the broadcast
	// counter on a successful compute. Wedged rings are logged, not retried in-place. Returns
	// true iff a cut was broadcast.
	bool MaybeEmitGlobalCut();

	ScalogGlobalOrderingCore* core_;            // borrowed: shared ordering state
	cxl_transport::MailboxSegment* segment_;    // borrowed: mailbox rings
	uint32_t K_;

	// Count of global-cut broadcasts emitted (for logging only). The broadcast WIRE tag is the
	// aligned DATA epoch from the core's epoch barrier, not this counter. Never resets.
	int64_t current_epoch_ = 0;

	std::atomic<bool> stop_flag_{false};
	std::unique_ptr<std::thread> poll_thread_;
};

}  // namespace cxl_manager
}  // namespace Embarcadero
