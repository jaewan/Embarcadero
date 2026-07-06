// Scalog CXL mailbox port — correctness + performance harness.
//
// Topology (docs/baselines/porting_rule.md): one co-located ScalogGlobalOrderingCore + one
// ScalogMailboxSequencer poll thread; one driver thread per broker (the broker-resident local
// sequencer role), each the SINGLE WRITER of its up(b) ring, plus one receiver thread per
// broker, each the SINGLE READER of its down(b) ring. Anonymous MAP_SHARED segment (no
// /dev/shm, so no testbed flock — in-proc across threads).
//
// This reuses the EXACT ordering code the gRPC baseline uses (ScalogGlobalOrderingCore's
// min-across-replicas-per-shard decision + readiness gate, via the poll thread), so a pass
// here is evidence the transport swap preserves the protocol.
//
// The drivers advance in per-epoch lockstep (a barrier): every driver posts epoch e for all
// its replicas, then all drivers wait until the sequencer has broadcast a cut tagged >= e
// before posting epoch e+1. This makes the sequencer's ready-snapshot at broadcast time
// correspond to a well-defined epoch, so the harness can INDEPENDENTLY recompute the expected
// MIN-across-replicas per broker for that epoch and compare it against the broadcast cut. It
// mirrors Scalog's per-epoch report -> aggregate -> broadcast cadence.
//
// Correctness:
//   (a) broadcast GlobalCut[b][epoch] == element-wise MIN across b's replicas of the
//       cumulative local cut for that epoch (recomputed independently);
//   (b) per-broker global cut values monotonically non-decreasing across epochs;
//   (c) every broker receives every epoch's cut (broadcast fidelity).
// Plus a WEDGED-BROKER regression: one broker never drains its down ring; the others still
// receive all cuts and Stop()/Join() returns (no hang).
// Performance: cuts/sec and ordered-messages/sec.
// Prints per-check "ok" lines and a final ALL CHECKS PASSED/FAILED; returns nonzero on fail.

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "cxl_manager/scalog_global_ordering_core.h"
#include "cxl_manager/scalog_mailbox_messages.h"
#include "cxl_manager/scalog_mailbox_sequencer.h"
#include "cxl_transport/cxl_mailbox.h"
#include "common/performance_utils.h"

namespace {

using Embarcadero::cxl_manager::kAbsentCut;
using Embarcadero::cxl_manager::ScalogGlobalCutMsg;
using Embarcadero::cxl_manager::ScalogGlobalOrderingCore;
using Embarcadero::cxl_manager::ScalogLocalCutMsg;
using Embarcadero::cxl_manager::ScalogMailboxSequencer;
using Embarcadero::cxl_transport::MailboxParams;
using Embarcadero::cxl_transport::MailboxSegment;

using Clock = std::chrono::high_resolution_clock;

// Benchmark shape.
constexpr uint32_t kNumBrokers = 4;
constexpr uint32_t kNumReplicas = 2;        // replicas per broker (replication factor)
constexpr uint32_t kEpochs = 2000;
constexpr uint32_t kRecordSize = 512;       // >= sizeof(ScalogGlobalCutMsg) == 272
constexpr uint32_t kRingCapacity = 1024;    // power of two

// Deterministic per-(broker,replica,epoch) cumulative local cut. Strictly increasing in
// epoch (so the MIN across replicas is strictly increasing -> monotonicity is testable), and
// differs across replicas (so the MIN is a non-trivial choice, not all-equal). Replica 0 is
// the slowest (smallest offset) for every broker, so the expected global cut == replica 0's
// value; we keep the formula general and recompute the MIN explicitly in the harness anyway.
int64_t LocalCutValue(uint32_t broker_id, uint32_t replica_id, uint32_t epoch) {
	// base grows with epoch; replicas add a positive, replica-dependent lead so replica 0 is
	// the minimum. Broker offset keeps per-broker series distinct.
	return static_cast<int64_t>(epoch + 1) * 1000
			+ static_cast<int64_t>(replica_id) * 100
			+ static_cast<int64_t>(broker_id);
}

// Expected global cut for a broker at an epoch = MIN across its replicas (independent recompute).
int64_t ExpectedGlobalCut(uint32_t broker_id, uint32_t epoch) {
	int64_t m = std::numeric_limits<int64_t>::max();
	for (uint32_t r = 0; r < kNumReplicas; ++r) {
		m = std::min(m, LocalCutValue(broker_id, r, epoch));
	}
	return m;
}

// Shared cross-thread epoch barrier. The sequencer broadcasts a cut tagged with a
// monotonically increasing epoch; receivers publish the highest tag seen. Drivers wait for
// broadcast_epoch >= their current epoch before advancing, so the sequencer's ready snapshot
// corresponds to a well-defined epoch.
struct BenchState {
	std::atomic<int64_t> max_broadcast_epoch{-1};
	std::atomic<bool> abort{false};
};

// Per-broker receiver context. Written by exactly one receiver thread; read by main after join.
struct ReceiverContext {
	uint32_t broker_id = 0;
	// global_cuts[epoch] = the cut value this broker received for that epoch tag (-1 if never).
	std::vector<int64_t> global_cuts;
	uint32_t epochs_received = 0;
	bool ok = false;
};

// Driver: single writer of up(broker_id). Posts epoch e for all replicas, then waits for the
// sequencer to broadcast a cut tagged >= e before posting e+1 (per-epoch lockstep).
void RunBrokerDriver(uint32_t broker_id, MailboxSegment* segment, BenchState* state) {
	Embarcadero::cxl_transport::MailboxRing& up = segment->up(broker_id);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

	for (uint32_t epoch = 0; epoch < kEpochs; ++epoch) {
		for (uint32_t replica = 0; replica < kNumReplicas; ++replica) {
			ScalogLocalCutMsg msg{};
			msg.local_cut = LocalCutValue(broker_id, replica, epoch);
			msg.epoch = epoch;
			msg.topic_id = 0;
			msg.broker_id = static_cast<int32_t>(broker_id);
			msg.replica_id = static_cast<int32_t>(replica);
			msg.pad = 0;
			while (!up.TryProduce(&msg, sizeof(msg))) {
				if (state->abort.load()) return;
				Embarcadero::CXL::cpu_pause();
			}
		}
		// Lockstep: wait until the sequencer has broadcast a cut tagged >= this epoch. The
		// broadcast epoch tag increments once per ready snapshot; when it reaches e, the
		// snapshot that produced it included this epoch's cuts from every broker.
		while (state->max_broadcast_epoch.load() < static_cast<int64_t>(epoch)) {
			if (state->abort.load()) return;
			if (std::chrono::steady_clock::now() > deadline) {
				std::fprintf(stderr, "FAIL: driver %u timed out at epoch %u\n", broker_id, epoch);
				state->abort.store(true);
				return;
			}
			Embarcadero::CXL::cpu_pause();
		}
	}
}

// Receiver: single reader of down(broker_id). Records the cut value per broadcast epoch tag
// and publishes the max tag seen so drivers can advance.
void RunBrokerReceiver(uint32_t broker_id, MailboxSegment* segment, BenchState* state,
		ReceiverContext* ctx) {
	Embarcadero::cxl_transport::MailboxRing& down = segment->down(broker_id);
	ctx->broker_id = broker_id;
	ctx->global_cuts.assign(kEpochs, kAbsentCut);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

	while (ctx->epochs_received < kEpochs) {
		ScalogGlobalCutMsg msg{};
		uint32_t len = 0;
		if (down.TryConsume(&msg, sizeof(msg), &len)) {
			if (len != sizeof(ScalogGlobalCutMsg)) {
				std::fprintf(stderr, "FAIL: receiver %u malformed GlobalCut len=%u\n",
						broker_id, len);
				state->abort.store(true);
				return;
			}
			if (msg.epoch >= 0 && msg.epoch < static_cast<int64_t>(kEpochs)) {
				if (ctx->global_cuts[msg.epoch] == kAbsentCut) ++ctx->epochs_received;
				ctx->global_cuts[msg.epoch] = msg.cut[broker_id];
			}
			// Publish the highest epoch tag seen (monotonic max) so drivers can advance.
			int64_t prev = state->max_broadcast_epoch.load();
			while (msg.epoch > prev &&
					!state->max_broadcast_epoch.compare_exchange_weak(prev, msg.epoch)) {
			}
		} else {
			if (state->abort.load()) return;
			if (std::chrono::steady_clock::now() > deadline) {
				std::fprintf(stderr, "FAIL: receiver %u timed out (received=%u/%u)\n",
						broker_id, ctx->epochs_received, kEpochs);
				state->abort.store(true);
				return;
			}
			Embarcadero::CXL::cpu_pause();
		}
	}
	ctx->ok = true;
}

bool ValidateAll(const std::vector<ReceiverContext>& ctxs) {
	bool all_ok = true;

	// Receiver completion (broadcast fidelity precondition: every receiver finished).
	for (const auto& ctx : ctxs) {
		if (!ctx.ok) {
			std::fprintf(stderr, "FAIL: receiver-complete (broker %u): did not finish (%u/%u)\n",
					ctx.broker_id, ctx.epochs_received, kEpochs);
			all_ok = false;
		} else {
			std::fprintf(stderr, "check: receiver-complete (broker %u): ok\n", ctx.broker_id);
		}
	}

	// Check (a): broadcast cut == independently recomputed MIN across replicas per epoch.
	for (const auto& ctx : ctxs) {
		bool ok = true;
		for (uint32_t e = 0; e < kEpochs; ++e) {
			const int64_t expected = ExpectedGlobalCut(ctx.broker_id, e);
			if (ctx.global_cuts[e] != expected) {
				std::fprintf(stderr,
						"FAIL: min-across-replicas (broker %u epoch %u): got %lld expected %lld\n",
						ctx.broker_id, e, (long long)ctx.global_cuts[e], (long long)expected);
				ok = false;
				break;
			}
		}
		if (ok) std::fprintf(stderr, "check: min-across-replicas (broker %u): ok\n", ctx.broker_id);
		all_ok &= ok;
	}

	// Check (b): monotonic non-decreasing per broker across epochs.
	for (const auto& ctx : ctxs) {
		bool ok = true;
		for (uint32_t e = 1; e < kEpochs; ++e) {
			if (ctx.global_cuts[e] < ctx.global_cuts[e - 1]) {
				std::fprintf(stderr,
						"FAIL: monotonic (broker %u epoch %u): %lld < %lld\n",
						ctx.broker_id, e, (long long)ctx.global_cuts[e],
						(long long)ctx.global_cuts[e - 1]);
				ok = false;
				break;
			}
		}
		if (ok) std::fprintf(stderr, "check: monotonic (broker %u): ok\n", ctx.broker_id);
		all_ok &= ok;
	}

	// Check (c): broadcast fidelity — every broker received every epoch's cut.
	for (const auto& ctx : ctxs) {
		uint32_t received = 0;
		for (uint32_t e = 0; e < kEpochs; ++e) {
			if (ctx.global_cuts[e] != kAbsentCut) ++received;
		}
		if (received != kEpochs) {
			std::fprintf(stderr, "FAIL: broadcast-fidelity (broker %u): %u/%u\n",
					ctx.broker_id, received, kEpochs);
			all_ok = false;
		} else {
			std::fprintf(stderr, "check: broadcast-fidelity (broker %u): ok\n", ctx.broker_id);
		}
	}

	return all_ok;
}

void ReportPerf(double elapsed_sec) {
	if (elapsed_sec <= 0.0) {
		std::fprintf(stderr, "perf: elapsed too small to measure\n");
		return;
	}
	// One cut per broker per epoch is broadcast; each carries kNumReplicas ordered inputs.
	const uint64_t total_cuts = static_cast<uint64_t>(kEpochs);        // broadcasts (per-epoch)
	const uint64_t ordered_msgs =
			static_cast<uint64_t>(kEpochs) * kNumBrokers * kNumReplicas;  // local cuts aggregated
	std::fprintf(stderr,
			"perf: epochs=%u brokers=%u replicas=%u elapsed=%.3f ms | %.1f cuts/s | %.1f ordered-msg/s\n",
			kEpochs, kNumBrokers, kNumReplicas, elapsed_sec * 1e3,
			(double)total_cuts / elapsed_sec, (double)ordered_msgs / elapsed_sec);
}

// Regression: one broker never drains its down ring (WEDGED). The sequencer must still serve
// the OTHER (healthy) broker to completion and Stop()/Join() must return (no hang). With a
// blocking broadcast this would hang in Join() and be killed by the ctest timeout.
bool TestWedgedBrokerDoesNotBlock() {
	MailboxParams p;
	p.num_brokers = 2;
	p.record_size = kRecordSize;
	p.up_capacity = 128;
	p.down_capacity = 4;  // small so the wedged broker's down ring fills quickly
	const size_t bytes = MailboxSegment::BytesNeeded(p);
	void* region = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(region != MAP_FAILED) << "mmap failed";
	auto seg = MailboxSegment::CreateInPlace(region, bytes, p);

	// RF=1 for this regression (one replica per broker) so a cut is ready as soon as both
	// brokers post their single local cut.
	auto core = std::make_unique<ScalogGlobalOrderingCore>(/*num_replicas_per_broker=*/1);
	ScalogMailboxSequencer sequencer(core.get(), seg.get(), /*K=*/8);
	sequencer.StartThread();

	// The sequencer emits one broadcast per poll pass that drains fresh data AND finds the
	// replica set ready, NOT one per posted cut, so we assert a LIVENESS target (the healthy
	// broker keeps receiving cuts while broker 0 is wedged) rather than an exact 1:1 count.
	// Both producers post continuously so there is ample fresh data to trigger >= this many
	// ready broadcasts; the point is that delivery to broker 1 never stalls behind broker 0.
	constexpr uint32_t kLivenessTarget = 20;

	// Broker 0: WEDGED — posts local cuts continuously but never reads down(0), so down(0)
	// fills and stays full. It must not stall delivery to broker 1.
	std::atomic<bool> stop_producers{false};
	std::thread wedged([&] {
		auto& up = seg->up(0);
		int64_t e = 0;
		while (!stop_producers.load()) {
			ScalogLocalCutMsg m{};
			m.local_cut = (e + 1) * 10;
			m.epoch = e; m.topic_id = 0; m.broker_id = 0; m.replica_id = 0; m.pad = 0;
			if (up.TryProduce(&m, sizeof(m))) ++e;
			else Embarcadero::CXL::cpu_pause();
		}
		// deliberately never consume down(0)
	});

	// Broker 1: HEALTHY — posts local cuts continuously and drains its down ring. Must reach
	// the liveness target and let Stop()/Join() return.
	uint32_t recv1 = 0;
	bool b1_ok = false;
	std::thread healthy([&] {
		auto& up = seg->up(1);
		auto& down = seg->down(1);
		std::thread producer([&] {
			int64_t e = 0;
			while (!stop_producers.load()) {
				ScalogLocalCutMsg m{};
				m.local_cut = (e + 1) * 10;
				m.epoch = e; m.topic_id = 0; m.broker_id = 1; m.replica_id = 0; m.pad = 0;
				if (up.TryProduce(&m, sizeof(m))) ++e;
				else Embarcadero::CXL::cpu_pause();
			}
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (recv1 < kLivenessTarget) {
			ScalogGlobalCutMsg g{}; uint32_t gl = 0;
			if (down.TryConsume(&g, sizeof(g), &gl)) {
				++recv1;
			} else {
				if (std::chrono::steady_clock::now() > deadline) break;  // healthy broker starved
				Embarcadero::CXL::cpu_pause();
			}
		}
		b1_ok = (recv1 >= kLivenessTarget);
		stop_producers.store(true);  // release both producers
		producer.join();
	});

	// Do NOT stop the producers here: both producer loops must keep posting local cuts until
	// the healthy receiver has reached its liveness target. The healthy thread sets
	// stop_producers (line above, after recv1 >= kLivenessTarget) which releases BOTH the
	// wedged producer and the healthy producer; join then unblocks. Setting it here on the
	// main thread would stop both loops before any cut is posted, so the core would never see
	// both brokers report, no GlobalCut would be broadcast, and the healthy receiver would
	// drain 0/kLivenessTarget.
	healthy.join();  // completes once recv1 >= kLivenessTarget, then sets stop_producers
	wedged.join();   // released by the healthy thread's stop_producers.store(true)
	sequencer.Stop();
	sequencer.Join();  // MUST return; a blocking-broadcast sequencer would hang here.

	const bool ok = b1_ok && recv1 >= kLivenessTarget;
	if (ok) {
		std::fprintf(stderr, "check: wedged-broker-does-not-block: ok "
				"(healthy broker got %u/%u cuts; Stop/Join returned)\n", recv1, kLivenessTarget);
	} else {
		std::fprintf(stderr, "FAIL: wedged-broker-does-not-block "
				"(healthy_complete=%d received=%u/%u)\n", (int)b1_ok, recv1, kLivenessTarget);
	}
	munmap(region, bytes);
	return ok;
}

}  // namespace

int main(int argc, char** argv) {
	google::InitGoogleLogging(argv[0]);
	(void)argc;

	// Liveness/fidelity regression first: a wedged broker must not block the others or hang Stop().
	const bool wedged_ok = TestWedgedBrokerDoesNotBlock();

	MailboxParams params;
	params.num_brokers = kNumBrokers;
	params.record_size = kRecordSize;
	params.up_capacity = kRingCapacity;
	params.down_capacity = kRingCapacity;

	const size_t bytes = MailboxSegment::BytesNeeded(params);
	// Anonymous MAP_SHARED: no /dev/shm, so no testbed flock (in-proc across threads).
	void* region = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(region != MAP_FAILED) << "mmap failed";

	std::unique_ptr<MailboxSegment> segment =
			MailboxSegment::CreateInPlace(region, bytes, params);
	CHECK(segment) << "CreateInPlace failed";

	// ONE shared ordering state machine + ONE poll thread (the co-located global sequencer).
	auto core = std::make_unique<ScalogGlobalOrderingCore>(/*num_replicas_per_broker=*/kNumReplicas);
	// Register every broker with the cluster replication factor (broker 0 sets it), mirroring
	// the gRPC RegisterBroker step. The mailbox path readiness gate uses num_brokers().
	for (uint32_t b = 0; b < kNumBrokers; ++b) {
		core->RegisterBroker(static_cast<int>(b), static_cast<int>(kNumReplicas));
	}
	ScalogMailboxSequencer sequencer(core.get(), segment.get(), /*K=*/64);
	sequencer.StartThread();

	BenchState state;
	std::vector<ReceiverContext> ctxs(kNumBrokers);
	std::vector<std::thread> drivers, receivers;
	drivers.reserve(kNumBrokers);
	receivers.reserve(kNumBrokers);

	const auto t0 = Clock::now();
	for (uint32_t b = 0; b < kNumBrokers; ++b) {
		receivers.emplace_back([b, &segment, &state, &ctxs] {
			RunBrokerReceiver(b, segment.get(), &state, &ctxs[b]);
		});
	}
	for (uint32_t b = 0; b < kNumBrokers; ++b) {
		drivers.emplace_back([b, &segment, &state] {
			RunBrokerDriver(b, segment.get(), &state);
		});
	}
	for (auto& t : drivers) t.join();
	for (auto& t : receivers) t.join();
	const auto t1 = Clock::now();

	// All producers/consumers done; stop and drain the sequencer.
	sequencer.Stop();
	sequencer.Join();

	const double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();

	const bool bench_ok = !state.abort.load() && ValidateAll(ctxs);
	ReportPerf(elapsed_sec);

	const bool all_ok = bench_ok && wedged_ok;
	std::fprintf(stderr, "%s\n", all_ok ? "ALL CHECKS PASSED" : "ALL CHECKS FAILED");

	munmap(region, bytes);
	return all_ok ? 0 : 1;
}
