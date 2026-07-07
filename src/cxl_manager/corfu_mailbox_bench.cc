// E10 benchmark + FIFO correctness harness for the Corfu CXL mailbox port.
//
// Topology (docs/baselines/porting_rule.md): one co-located CorfuSequencerImpl + one
// CorfuMailboxSequencer poll thread; one driver thread per broker (the ingress-broker role),
// each the SINGLE WRITER of its up(b) ring and SINGLE READER of its down(b) ring. Each driver
// posts N CorfuTokenRequest records in ascending per-(client,broker) batch_seq order and reads
// the matching CorfuTokenGrant records back.
//
// This reuses the EXACT ordering code the TCP baseline uses (CorfuSequencerImpl::AssignToken
// via the poll thread), so a pass here is evidence the transport swap preserves the protocol.
//
// Correctness: (a) per-client total_order strictly increasing in submission order (per-client
// FIFO), (b) per-broker log_idx monotonically increasing by total_size, (c) per-broker
// broker_batch_seq contiguous 0,1,2,..., (d) every grant OK under correct in-order load.
// Performance: tokens/sec and messages/sec over the active loop. Prints per-check "ok" lines
// and a final ALL ... PASSED/FAILED summary; returns nonzero on any failure.

#include <sys/mman.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "cxl_manager/corfu_mailbox_messages.h"
#include "cxl_manager/corfu_mailbox_sequencer.h"
#include "cxl_manager/corfu_sequencer_service.h"
#include "cxl_transport/cxl_mailbox.h"
#include "common/performance_utils.h"

namespace {

using Embarcadero::cxl_manager::CorfuMailboxSequencer;
using Embarcadero::cxl_manager::CorfuTokenGrant;
using Embarcadero::cxl_manager::CorfuTokenRequest;
using Embarcadero::cxl_transport::MailboxParams;
using Embarcadero::cxl_transport::MailboxSegment;

using Clock = std::chrono::high_resolution_clock;

// [[CALIBRATION]] Env override for benchmark-shape knobs (positive integers only). The smoke
// defaults keep CTest fast; calibration runs (docs/baselines/calibration_plan.md) scale the
// request count to a seconds-long plateau and sweep the batch size without a recompile.
uint64_t EnvShapeU64(const char* name, uint64_t def) {
	const char* e = std::getenv(name);
	if (!e || !*e) return def;
	char* end = nullptr;
	unsigned long long v = std::strtoull(e, &end, 10);
	return (end != e && *end == '\0' && v > 0) ? static_cast<uint64_t>(v) : def;
}

// Benchmark shape.
constexpr uint32_t kNumBrokers = 4;
constexpr uint32_t kNumClients = 8;
constexpr uint32_t kRecordSize = 128;      // >= sizeof(CorfuTokenGrant) == 48
constexpr uint32_t kRingCapacity = 1024;   // power of two
const uint32_t kRequestsPerBroker = static_cast<uint32_t>(
		EnvShapeU64("CORFU_BENCH_REQUESTS_PER_BROKER", 4096));
const uint64_t kMessagesPerRequest =
		EnvShapeU64("CORFU_BENCH_MSGS_PER_REQ", 4);   // batch size (num_msg per token request)
constexpr uint64_t kBytesPerRequest = 512;    // total_size per request

// One request submitted and (later) its grant, kept in submission order per driver.
struct RequestObs {
	CorfuTokenRequest req;
	Clock::time_point post_ts;
};
struct GrantObs {
	CorfuTokenGrant grant;
	Clock::time_point recv_ts;
};

// Per-broker driver context. Each field is written by exactly one thread (the driver), so no
// locking is needed: the driver owns its vectors, the main thread reads them only after join.
struct DriverContext {
	uint32_t broker_id = 0;
	std::vector<RequestObs> requests;   // in submission order
	std::vector<GrantObs> grants;       // in receive order (== submission order per SPSC ring)
	bool ok = false;
};

// Driver: single writer of up(broker_id), single reader of down(broker_id).
void RunBrokerDriver(uint32_t broker_id, MailboxSegment* segment, DriverContext* ctx) {
	Embarcadero::cxl_transport::MailboxRing& up = segment->up(broker_id);
	Embarcadero::cxl_transport::MailboxRing& down = segment->down(broker_id);

	// Broker b owns clients {b, b+kNumBrokers, ...} so each (client,broker) stream has a
	// single writer and batch_seq is generated in ascending order.
	std::vector<uint64_t> my_clients;
	for (uint64_t c = broker_id; c < kNumClients; c += kNumBrokers) my_clients.push_back(c);
	if (my_clients.empty()) my_clients.push_back(broker_id);  // safety

	std::map<uint64_t, uint64_t> next_batch_seq;  // client_id -> next batch_seq
	for (uint64_t c : my_clients) next_batch_seq[c] = 0;

	ctx->requests.reserve(kRequestsPerBroker);
	ctx->grants.reserve(kRequestsPerBroker);

	uint32_t posted = 0;
	uint32_t received = 0;
	uint32_t rr = 0;  // round-robin index into my_clients

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

	while (received < kRequestsPerBroker) {
		// Post as many as the up ring will take (non-blocking) to keep it fed.
		while (posted < kRequestsPerBroker) {
			const uint64_t client_id = my_clients[rr % my_clients.size()];
			CorfuTokenRequest req{};
			req.client_id = client_id;
			req.batch_seq = next_batch_seq[client_id];
			req.num_msg = kMessagesPerRequest;
			req.total_size = kBytesPerRequest;
			req.broker_id = broker_id;
			req.pad = 0;

			if (!up.TryProduce(&req, sizeof(req))) break;  // ring full, drain grants first

			next_batch_seq[client_id]++;
			++rr;
			ctx->requests.push_back({req, Clock::now()});
			++posted;
		}

		// Drain any available grants.
		bool got = false;
		CorfuTokenGrant grant{};
		uint32_t glen = 0;
		while (down.TryConsume(&grant, sizeof(grant), &glen)) {
			CHECK_EQ(glen, sizeof(CorfuTokenGrant));
			ctx->grants.push_back({grant, Clock::now()});
			++received;
			got = true;
		}

		if (!got && posted >= kRequestsPerBroker) {
			Embarcadero::CXL::cpu_pause();
			if (std::chrono::steady_clock::now() > deadline) {
				std::fprintf(stderr, "FAIL: driver %u timed out (posted=%u received=%u)\n",
						broker_id, posted, received);
				return;
			}
		}
	}
	ctx->ok = true;
}

bool ValidateAll(const std::vector<DriverContext>& ctxs) {
	bool all_ok = true;

	// Driver completion (no timeout).
	for (const auto& ctx : ctxs) {
		if (!ctx.ok) {
			std::fprintf(stderr, "FAIL: driver-complete (broker %u): driver did not finish\n",
					ctx.broker_id);
			all_ok = false;
		} else {
			std::fprintf(stderr, "check: driver-complete (broker %u): ok\n", ctx.broker_id);
		}
	}

	// Check (a): per-client FIFO — total_order strictly increasing in submission order.
	// Per broker, grants arrive in submission order (SPSC ring), and each client is owned by
	// one broker, so iterating grants in receive order per client yields submission order.
	std::map<uint64_t, std::vector<uint64_t>> client_orders;
	for (const auto& ctx : ctxs) {
		for (const auto& g : ctx.grants) {
			client_orders[g.grant.client_id].push_back(g.grant.total_order);
		}
	}
	for (const auto& [cid, orders] : client_orders) {
		bool ok = true;
		for (size_t i = 1; i < orders.size(); ++i) {
			if (orders[i] <= orders[i - 1]) {
				std::fprintf(stderr,
						"FAIL: per-client-FIFO (client %llu): total_order %llu <= prev %llu at %zu\n",
						(unsigned long long)cid, (unsigned long long)orders[i],
						(unsigned long long)orders[i - 1], i);
				ok = false;
				break;
			}
		}
		if (ok) std::fprintf(stderr, "check: per-client-FIFO (client %llu): ok\n",
				(unsigned long long)cid);
		all_ok &= ok;
	}

	// Check (b): per-broker log_idx monotonically increasing by total_size (kBytesPerRequest).
	for (const auto& ctx : ctxs) {
		bool ok = true;
		bool have_prev = false;
		uint64_t prev = 0;
		for (const auto& g : ctx.grants) {
			if (g.grant.status != 0) continue;  // only OK grants carry meaningful log_idx
			if (have_prev) {
				if (g.grant.log_idx <= prev) {
					std::fprintf(stderr,
							"FAIL: per-broker-log-idx (broker %u): log_idx %llu <= prev %llu\n",
							ctx.broker_id, (unsigned long long)g.grant.log_idx,
							(unsigned long long)prev);
					ok = false;
					break;
				}
				if (g.grant.log_idx - prev != kBytesPerRequest) {
					std::fprintf(stderr,
							"FAIL: per-broker-log-idx (broker %u): stride %llu != %llu\n",
							ctx.broker_id, (unsigned long long)(g.grant.log_idx - prev),
							(unsigned long long)kBytesPerRequest);
					ok = false;
					break;
				}
			}
			prev = g.grant.log_idx;
			have_prev = true;
		}
		if (ok) std::fprintf(stderr, "check: per-broker-log-idx (broker %u): ok\n", ctx.broker_id);
		all_ok &= ok;
	}

	// Check (c): per-broker broker_batch_seq contiguous 0,1,2,...
	for (const auto& ctx : ctxs) {
		bool ok = true;
		uint64_t expected = 0;
		for (const auto& g : ctx.grants) {
			if (g.grant.status != 0) continue;
			if (g.grant.broker_batch_seq != expected) {
				std::fprintf(stderr,
						"FAIL: per-broker-batch-seq (broker %u): got %llu expected %llu\n",
						ctx.broker_id, (unsigned long long)g.grant.broker_batch_seq,
						(unsigned long long)expected);
				ok = false;
				break;
			}
			++expected;
		}
		if (ok) std::fprintf(stderr, "check: per-broker-batch-seq (broker %u): ok\n", ctx.broker_id);
		all_ok &= ok;
	}

	// Check (d): every grant OK, and echo fields correlate with the submitted request.
	for (const auto& ctx : ctxs) {
		bool ok = true;
		if (ctx.grants.size() != ctx.requests.size()) {
			std::fprintf(stderr, "FAIL: grant-count (broker %u): %zu grants != %zu requests\n",
					ctx.broker_id, ctx.grants.size(), ctx.requests.size());
			ok = false;
		}
		for (size_t i = 0; i < ctx.grants.size() && ok; ++i) {
			const auto& g = ctx.grants[i].grant;
			if (g.status != 0) {
				std::fprintf(stderr, "FAIL: grant-status (broker %u): non-OK status %u at %zu\n",
						ctx.broker_id, g.status, i);
				ok = false;
				break;
			}
			// SPSC ring preserves order; echo must match the i-th submitted request.
			const auto& r = ctx.requests[i].req;
			if (g.client_id != r.client_id || g.batch_seq != r.batch_seq) {
				std::fprintf(stderr,
						"FAIL: grant-correlation (broker %u): grant(c=%llu,b=%llu) != req(c=%llu,b=%llu) at %zu\n",
						ctx.broker_id, (unsigned long long)g.client_id,
						(unsigned long long)g.batch_seq, (unsigned long long)r.client_id,
						(unsigned long long)r.batch_seq, i);
				ok = false;
				break;
			}
		}
		if (ok) std::fprintf(stderr, "check: grant-ok-and-correlated (broker %u): ok\n",
				ctx.broker_id);
		all_ok &= ok;
	}

	return all_ok;
}

void ReportPerf(const std::vector<DriverContext>& ctxs) {
	uint64_t total_tokens = 0;
	uint64_t total_msgs = 0;
	Clock::time_point min_post = Clock::time_point::max();
	Clock::time_point max_recv = Clock::time_point::min();

	for (const auto& ctx : ctxs) {
		total_tokens += ctx.requests.size();
		total_msgs += ctx.requests.size() * kMessagesPerRequest;
		if (!ctx.requests.empty()) min_post = std::min(min_post, ctx.requests.front().post_ts);
		if (!ctx.grants.empty()) max_recv = std::max(max_recv, ctx.grants.back().recv_ts);
	}

	const int64_t elapsed_ns =
			std::chrono::duration_cast<std::chrono::nanoseconds>(max_recv - min_post).count();
	if (elapsed_ns <= 0) {
		std::fprintf(stderr, "perf: elapsed too small to measure\n");
		return;
	}
	const double tokens_sec = (double)total_tokens * 1e9 / (double)elapsed_ns;
	const double msgs_sec = (double)total_msgs * 1e9 / (double)elapsed_ns;
	std::fprintf(stderr,
			"perf: tokens=%llu msgs=%llu elapsed=%.3f ms | %.3f M tokens/s | %.3f M msgs/s "
			"(avg num_msg=%llu)\n",
			(unsigned long long)total_tokens, (unsigned long long)total_msgs,
			(double)elapsed_ns / 1e6, tokens_sec / 1e6, msgs_sec / 1e6,
			(unsigned long long)kMessagesPerRequest);
}

// Regression test for the deliverability-before-commit / no-HOL-block fix: one broker's
// down ring is deliberately wedged (its consumer never reads), and we assert the sequencer
// still serves the OTHER (healthy) broker to completion and that Stop()/Join() returns
// (does not hang). With the earlier blocking-Produce sequencer this test would hang in
// Join() and be killed by the ctest timeout.
bool TestWedgedBrokerDoesNotBlock() {
	MailboxParams p;
	p.num_brokers = 2;
	p.record_size = 128;
	p.up_capacity = 64;
	p.down_capacity = 4;  // small so the wedged broker's down ring fills quickly
	const size_t bytes = MailboxSegment::BytesNeeded(p);
	void* region = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(region != MAP_FAILED) << "mmap failed";
	auto seg = MailboxSegment::CreateInPlace(region, bytes, p);
	auto impl = std::make_unique<CorfuSequencerImpl>();
	CorfuMailboxSequencer sequencer(impl.get(), seg.get(), /*K=*/8);
	sequencer.StartThread();

	// Broker 0: WEDGED — posts a few requests but never reads its down ring, so down(0) fills
	// and stays full. It must not stall delivery to broker 1.
	std::thread wedged([&] {
		auto& up = seg->up(0);
		for (int i = 0; i < 32; ++i) {
			CorfuTokenRequest r{};
			r.client_id = 0; r.batch_seq = static_cast<uint64_t>(i);
			r.num_msg = 1; r.total_size = 64; r.broker_id = 0;
			if (!up.TryProduce(&r, sizeof(r))) break;
		}
		// deliberately never consume down(0)
	});

	// Broker 1: HEALTHY — post N and read N grants back.
	constexpr int N = 200;
	int recv1 = 0;
	bool b1_ok = false;
	std::thread healthy([&] {
		auto& up = seg->up(1);
		auto& down = seg->down(1);
		int posted = 0;
		uint64_t bs = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (recv1 < N) {
			while (posted < N) {
				CorfuTokenRequest r{};
				r.client_id = 1; r.batch_seq = bs; r.num_msg = 1; r.total_size = 64; r.broker_id = 1;
				if (!up.TryProduce(&r, sizeof(r))) break;
				++bs; ++posted;
			}
			CorfuTokenGrant g{}; uint32_t gl = 0;
			while (down.TryConsume(&g, sizeof(g), &gl)) ++recv1;
			if (std::chrono::steady_clock::now() > deadline) return;  // healthy broker starved
			Embarcadero::CXL::cpu_pause();
		}
		b1_ok = true;
	});

	wedged.join();
	healthy.join();
	sequencer.Stop();
	sequencer.Join();  // MUST return; a HOL-blocking sequencer would hang here.

	const bool ok = b1_ok && recv1 == N;
	if (ok) {
		std::fprintf(stderr, "check: wedged-broker-does-not-block: ok "
				"(healthy broker got %d/%d grants; Stop/Join returned)\n", recv1, N);
	} else {
		std::fprintf(stderr, "FAIL: wedged-broker-does-not-block "
				"(healthy_complete=%d received=%d/%d)\n", (int)b1_ok, recv1, N);
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

	// ONE shared ordering state machine + ONE poll thread (the co-located sequencer).
	auto impl = std::make_unique<CorfuSequencerImpl>();
	CorfuMailboxSequencer sequencer(impl.get(), segment.get(), /*K=*/64);
	sequencer.StartThread();

	// One driver thread per broker (single writer per up ring).
	std::vector<DriverContext> ctxs(kNumBrokers);
	std::vector<std::thread> drivers;
	drivers.reserve(kNumBrokers);
	for (uint32_t b = 0; b < kNumBrokers; ++b) {
		ctxs[b].broker_id = b;
		drivers.emplace_back([b, &segment, &ctxs] {
			RunBrokerDriver(b, segment.get(), &ctxs[b]);
		});
	}
	for (auto& t : drivers) t.join();

	// All producers are done; stop and drain the sequencer.
	sequencer.Stop();
	sequencer.Join();

	const bool bench_ok = ValidateAll(ctxs);
	ReportPerf(ctxs);

	const bool all_ok = bench_ok && wedged_ok;
	std::fprintf(stderr, "%s\n", all_ok ? "ALL CHECKS PASSED" : "ALL CHECKS FAILED");

	munmap(region, bytes);
	return all_ok ? 0 : 1;
}
