#pragma once

// LazyLogMailboxSequencer — the CXL-transport port of the LazyLog coordinator (binding-round)
// hop.
//
// This is a PURE TRANSPORT SWAP (docs/baselines/porting_rule.md): it holds a borrowed pointer to
// ONE LazyLogBindingCore (the shared per-broker binding-delta ordering state, also used by the
// gRPC baseline) and a MailboxSegment, and runs a single polling thread that drains
// LazyLogLocalProgressMsg records from every up(b) ring, calls LazyLogBindingCore::
// AddLocalProgress (the SAME accumulation code the gRPC baseline uses), and — once every broker
// has reported (the SAME readiness gate the gRPC SendGlobalBinding uses) — computes the
// GlobalBinding via the core and delivers it to every down(b) with BroadcastDown. The ordering
// logic is not duplicated or altered here; only the wire changes (gRPC bidi stream -> CXL mailbox
// up-rings + BroadcastDown).
//
// SPSC contract: the sequencer thread is the SINGLE READER of every up(b) and the SINGLE WRITER
// of every down(b). Each up(b) must have exactly one writer thread (the broker's local sequencer
// / the benchmark driver), enforced by topology; the DEBUG concurrent-writer detector in
// MailboxRing LOG(FATAL)s otherwise.
//
// Wedged-broker safety: GlobalBinding delivery uses MailboxSegment::BroadcastDown, which makes
// ONE bounded, decoupled pass and returns per-ring BroadcastStatus. A broker that never drains
// its down(b) ring (WEDGED) does not block delivery to the others and does not hang Stop()/
// Join() — the sequencer logs the wedged ring and moves on (the binding is re-broadcast on the
// next ready epoch, since binding deltas are cumulative/idempotent: the wedged broker gets the
// aggregate of the rounds it missed once it drains again).

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "cxl_manager/lazylog_binding_core.h"
#include "cxl_transport/cxl_mailbox.h"

namespace Embarcadero {
namespace cxl_manager {

class LazyLogMailboxSequencer {
 public:
	// Borrowed (non-owning) core and segment. K = max LocalProgress records drained per up(b)
	// ring per poll iteration before moving to the next ring (fairness bound across brokers).
	LazyLogMailboxSequencer(LazyLogBindingCore* core,
			cxl_transport::MailboxSegment* segment, uint32_t K = 64);
	~LazyLogMailboxSequencer();

	LazyLogMailboxSequencer(const LazyLogMailboxSequencer&) = delete;
	LazyLogMailboxSequencer& operator=(const LazyLogMailboxSequencer&) = delete;

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
	// Drain up to K_ LazyLogLocalProgressMsg records from up(broker_id) into the core. Returns the
	// number of records processed this call.
	uint32_t DrainBrokerProgress(uint32_t broker_id);

	// If every broker has reported AND is aligned on one epoch (core readiness + epoch barrier),
	// compute the per-broker binding delta, encode it tagged with that aligned DATA epoch, and
	// BroadcastDown to every down(b). Advances the broadcast counter on a successful compute.
	// Wedged rings are logged, not retried in-place. Returns true iff a binding was broadcast.
	bool MaybeEmitGlobalBinding();

	LazyLogBindingCore* core_;                  // borrowed: shared ordering state
	cxl_transport::MailboxSegment* segment_;    // borrowed: mailbox rings
	uint32_t K_;

	// Count of global-binding broadcasts emitted (for logging only). The broadcast WIRE tag is
	// the aligned DATA epoch from the core's epoch barrier, not this counter. Never resets.
	int64_t current_binding_round_ = 0;

	std::atomic<bool> stop_flag_{false};
	std::unique_ptr<std::thread> poll_thread_;
};

}  // namespace cxl_manager
}  // namespace Embarcadero
