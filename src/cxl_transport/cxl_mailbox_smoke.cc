// Smoke test for the CXL mailbox transport (Track 03 / W4).
//
// Exercises the single-writer ring + duplex segment discipline without any testbed
// resource: the cross-process test uses an anonymous MAP_SHARED region inherited across
// fork(), so it needs neither /dev/shm nor the testbed lock. Run the POSIX-shm variants
// (which do map /dev/shm) with `--shm`, under the flock.
//
// All in-memory ring/segment buffers are allocated 64-byte aligned (posix_memalign):
// std::vector<uint8_t> guarantees only 16-byte alignment on x86-64, which would push the
// cache-line-aligned headers off their lines and NOT exercise the real CXL layout.
//
// Checks:
//   1. Ring FIFO + payload integrity (single thread).
//   2. Ring back-pressure: a full ring rejects TryProduce until the consumer drains.
//   3. Concurrent SPSC: producer + consumer threads move a high volume in order,
//      reported with a wall-clock msgs/sec figure (tracks the P1-5 flush-domination fix).
//   4. Segment duplex round-trip modeling the Corfu token exchange (request up,
//      response down, correlation id inside the record).
//   5. Cross-process duplex over anonymous MAP_SHARED (fork), modeling a broker and a
//      coordinator in separate address spaces.
//   5b. (--shm) Cross-process POSIX-shm where the child attaches at a DIFFERENT base
//       address, proving the offset-only layout under independent mappings.
//   6. BroadcastDownNonBlocking fairness: a deliberately-full broker ring does not block
//      delivery to the healthy ones (P1-3 head-of-line decoupling).

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "cxl_transport/cxl_mailbox.h"
#include "cxl_transport/cxl_mailbox_ring.h"

using Embarcadero::cxl_transport::BroadcastStatus;
using Embarcadero::cxl_transport::MailboxParams;
using Embarcadero::cxl_transport::MailboxRing;
using Embarcadero::cxl_transport::MailboxSegment;
using Embarcadero::cxl_transport::PerRingStatus;

namespace {

int g_failures = 0;
#define CHECK_TRUE(cond, msg)                                        \
	do {                                                             \
		if (!(cond)) { LOG(ERROR) << "FAIL: " << (msg); ++g_failures; } \
	} while (0)

// 64-byte aligned buffer with RAII cleanup. posix_memalign (POSIX.1-2008) — the codebase
// already links rt/pthreads. Zero-initialized before use.
using Aligned64 = std::unique_ptr<uint8_t, void (*)(void*)>;
Aligned64 Allocate64Aligned(size_t bytes) {
	void* ptr = nullptr;
	const int rc = posix_memalign(&ptr, 64, bytes);
	PCHECK(rc == 0) << "posix_memalign(64, " << bytes << ") failed: " << rc;
	PCHECK(ptr != nullptr);
	std::memset(ptr, 0, bytes);
	return Aligned64(reinterpret_cast<uint8_t*>(ptr), &free);
}

// A tiny POD control record the transport carries opaquely (stands in for a baseline's
// TokenRequest/TokenGrant / LocalCut / LocalProgress record).
struct Msg {
	uint64_t corr_id;
	uint64_t value;
};

void TestRingFifoAndPayload() {
	const uint32_t cap = 8, rec = sizeof(Msg);
	auto buf = Allocate64Aligned(MailboxRing::BytesNeeded(rec, cap));
	MailboxRing ring = MailboxRing::Create(buf.get(), MailboxRing::BytesNeeded(rec, cap), rec, cap);

	for (uint64_t i = 0; i < 5; ++i) {
		Msg m{i, i * 100};
		CHECK_TRUE(ring.TryProduce(&m, sizeof(m)), "produce should succeed");
	}
	for (uint64_t i = 0; i < 5; ++i) {
		Msg out{};
		uint32_t len = 0;
		CHECK_TRUE(ring.TryConsume(&out, sizeof(out), &len), "consume should succeed");
		CHECK_TRUE(len == sizeof(Msg), "len matches");
		CHECK_TRUE(out.corr_id == i && out.value == i * 100, "FIFO order + payload intact");
	}
	Msg out{};
	uint32_t len = 0;
	CHECK_TRUE(!ring.TryConsume(&out, sizeof(out), &len), "empty ring yields nothing");
	LOG(INFO) << "[1] ring FIFO + payload: ok";
}

void TestRingBackpressure() {
	const uint32_t cap = 4, rec = sizeof(Msg);
	auto buf = Allocate64Aligned(MailboxRing::BytesNeeded(rec, cap));
	MailboxRing ring = MailboxRing::Create(buf.get(), MailboxRing::BytesNeeded(rec, cap), rec, cap);

	for (uint32_t i = 0; i < cap; ++i) {
		Msg m{i, i};
		CHECK_TRUE(ring.TryProduce(&m, sizeof(m)), "fill to capacity");
	}
	Msg over{99, 99};
	CHECK_TRUE(!ring.TryProduce(&over, sizeof(over)), "full ring applies back-pressure");
	Msg out{};
	uint32_t len = 0;
	CHECK_TRUE(ring.TryConsume(&out, sizeof(out), &len), "drain one");
	CHECK_TRUE(ring.TryProduce(&over, sizeof(over)), "space freed after drain");
	LOG(INFO) << "[2] ring back-pressure: ok";
}

// Large multi-cache-line records (record_size > one line): exercises the FlushRange /
// InvalidateRange + full_fence overflow path that packed 16-byte records never touch. This
// is the path real segment traffic uses (default record_size=512 for Scalog/LazyLog cuts),
// and where the load_fence->full_fence correctness fix lives. Concurrent SPSC so the
// producer's flush and the consumer's invalidate race across the payload lines.
void TestLargeRecordOverflow() {
	const uint32_t cap = 256, rec = 200;  // 200B payload -> spans 4 cache lines incl header
	const uint64_t N = 200'000;
	auto buf = Allocate64Aligned(MailboxRing::BytesNeeded(rec, cap));
	MailboxRing ring = MailboxRing::Create(buf.get(), MailboxRing::BytesNeeded(rec, cap), rec, cap);

	// Each record: first 8 bytes = sequence i, remaining bytes = a per-record byte pattern
	// derived from i, so a torn/stale read anywhere in the payload is detected.
	auto fill = [](uint8_t* p, uint64_t i) {
		std::memcpy(p, &i, sizeof(i));
		for (uint32_t k = sizeof(i); k < 200; ++k) p[k] = static_cast<uint8_t>((i + k) & 0xFF);
	};
	std::thread producer([&] {
		uint8_t rec_buf[200];
		for (uint64_t i = 0; i < N; ++i) { fill(rec_buf, i); ring.Produce(rec_buf, 200); }
	});
	uint64_t received = 0, bad = 0;
	std::thread consumer([&] {
		uint8_t out[200];
		uint8_t expect[200];
		uint32_t len = 0;
		while (received < N) {
			if (ring.TryConsume(out, sizeof(out), &len)) {
				fill(expect, received);
				if (len != 200 || std::memcmp(out, expect, 200) != 0) ++bad;
				++received;
			}
		}
	});
	producer.join();
	consumer.join();
	CHECK_TRUE(received == N, "all large records received");
	CHECK_TRUE(bad == 0, "no torn/stale multi-line payload");
	LOG(INFO) << "[3b] large multi-line records (" << N << " x 200B, overflow path): ok";
}

void TestConcurrentSpsc() {
	const uint32_t cap = 1024, rec = sizeof(Msg);
	const uint64_t N = 2'000'000;
	auto buf = Allocate64Aligned(MailboxRing::BytesNeeded(rec, cap));
	MailboxRing ring = MailboxRing::Create(buf.get(), MailboxRing::BytesNeeded(rec, cap), rec, cap);

	// Time only the active producer/consumer loop (ring is already initialized).
	const auto start = std::chrono::high_resolution_clock::now();
	std::thread producer([&] {
		for (uint64_t i = 0; i < N; ++i) {
			Msg m{i, ~i};
			ring.Produce(&m, sizeof(m));
		}
	});
	uint64_t received = 0, bad = 0;
	std::thread consumer([&] {
		Msg out{};
		uint32_t len = 0;
		while (received < N) {
			if (ring.TryConsume(&out, sizeof(out), &len)) {
				if (out.corr_id != received || out.value != ~received) ++bad;
				++received;
			}
		}
	});
	producer.join();
	consumer.join();
	const auto end = std::chrono::high_resolution_clock::now();
	const double secs = std::chrono::duration<double>(end - start).count();
	const double mps = secs > 0 ? (static_cast<double>(N) / secs) : 0.0;

	CHECK_TRUE(received == N, "all records received");
	CHECK_TRUE(bad == 0, "no out-of-order / corrupt records");
	LOG(INFO) << "[3] concurrent SPSC (" << N << " msgs): ok, "
	          << (mps / 1e6) << " M msgs/sec (" << secs << " s)";
}

// Model Corfu's unary token exchange over the duplex segment: a coordinator thread
// grants a monotonically increasing token per request, echoing the correlation id.
void TestSegmentDuplexRoundtrip() {
	MailboxParams p;
	p.num_brokers = 4;
	p.record_size = sizeof(Msg);
	p.up_capacity = 256;
	p.down_capacity = 256;
	auto buf = Allocate64Aligned(MailboxSegment::BytesNeeded(p));
	auto coord = MailboxSegment::CreateInPlace(buf.get(), MailboxSegment::BytesNeeded(p), p);
	auto broker = MailboxSegment::AttachInPlace(buf.get(), MailboxSegment::BytesNeeded(p));  // same region

	const uint64_t kReqPerBroker = 10'000;
	std::atomic<bool> stop{false};
	std::thread coordinator([&] {
		uint64_t next_token = 0;
		while (!stop.load(std::memory_order_relaxed)) {
			for (uint32_t b = 0; b < p.num_brokers; ++b) {
				Msg req{};
				uint32_t len = 0;
				if (coord->up(b).TryConsume(&req, sizeof(req), &len)) {
					Msg grant{req.corr_id, next_token++};  // same counter, per porting rule
					coord->down(b).Produce(&grant, sizeof(grant));
				}
			}
		}
	});

	uint64_t bad = 0;
	std::vector<std::thread> brokers;
	for (uint32_t b = 0; b < p.num_brokers; ++b) {
		brokers.emplace_back([&, b] {
			for (uint64_t i = 0; i < kReqPerBroker; ++i) {
				Msg req{(static_cast<uint64_t>(b) << 40) | i, 0};
				broker->up(b).Produce(&req, sizeof(req));
				Msg resp{};
				uint32_t len = 0;
				while (!broker->down(b).TryConsume(&resp, sizeof(resp), &len)) {
					Embarcadero::CXL::cpu_pause();
				}
				if (resp.corr_id != req.corr_id) ++bad;  // response matched to request
			}
		});
	}
	for (auto& t : brokers) t.join();
	stop.store(true);
	coordinator.join();
	CHECK_TRUE(bad == 0, "every response correlated to its request");
	LOG(INFO) << "[4] segment duplex round-trip (" << p.num_brokers * kReqPerBroker
	          << " req/resp): ok";
}

// Cross-process duplex over an anonymous MAP_SHARED region inherited across fork().
// No /dev/shm, no ports, no lock. Proves the POD/offset-only layout works across
// address spaces.
void TestCrossProcess() {
	MailboxParams p;
	p.num_brokers = 1;
	p.record_size = sizeof(Msg);
	p.up_capacity = 256;
	p.down_capacity = 256;
	const size_t bytes = MailboxSegment::BytesNeeded(p);
	void* region = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
	                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	PCHECK(region != MAP_FAILED);
	auto coord = MailboxSegment::CreateInPlace(region, bytes, p);  // init before fork

	const uint64_t kN = 50'000;
	pid_t pid = fork();
	PCHECK(pid >= 0);
	if (pid == 0) {
		// Child = coordinator: echo token grants.
		auto c = MailboxSegment::AttachInPlace(region, bytes);
		uint64_t granted = 0;
		while (granted < kN) {
			Msg req{};
			uint32_t len = 0;
			if (c->up(0).TryConsume(&req, sizeof(req), &len)) {
				Msg grant{req.corr_id, granted++};
				c->down(0).Produce(&grant, sizeof(grant));
			}
		}
		_exit(0);
	}
	// Parent = broker.
	auto b = MailboxSegment::AttachInPlace(region, bytes);
	uint64_t bad = 0;
	for (uint64_t i = 0; i < kN; ++i) {
		Msg req{i, 0};
		b->up(0).Produce(&req, sizeof(req));
		Msg resp{};
		uint32_t len = 0;
		while (!b->down(0).TryConsume(&resp, sizeof(resp), &len)) {
			Embarcadero::CXL::cpu_pause();
		}
		if (resp.corr_id != i) ++bad;
	}
	int status = 0;
	waitpid(pid, &status, 0);
	CHECK_TRUE(bad == 0, "cross-process responses correlated");
	CHECK_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "coordinator child exited cleanly");
	munmap(region, bytes);
	LOG(INFO) << "[5] cross-process duplex (" << kN << " req/resp over fork): ok";
}

// BroadcastDownNonBlocking fairness (P1-3): fill one broker's down ring to capacity and
// verify a broadcast still delivers to the other (healthy) brokers instead of blocking.
void TestBroadcastFairness() {
	MailboxParams p;
	p.num_brokers = 3;
	p.record_size = sizeof(Msg);
	p.up_capacity = 8;
	p.down_capacity = 4;  // small so we can wedge one ring cheaply
	auto buf = Allocate64Aligned(MailboxSegment::BytesNeeded(p));
	auto seg = MailboxSegment::CreateInPlace(buf.get(), MailboxSegment::BytesNeeded(p), p);

	// Wedge broker 1's down ring by filling it and never draining.
	Msg filler{7, 7};
	for (uint32_t i = 0; i < p.down_capacity; ++i) {
		CHECK_TRUE(seg->down(1).TryProduce(&filler, sizeof(filler)), "prefill wedged ring");
	}

	// Broadcast with a small spin bound so the wedged ring reports WEDGED quickly while the
	// healthy rings (0 and 2) succeed.
	Msg cut{42, 100};
	BroadcastStatus st = seg->BroadcastDownNonBlocking(&cut, sizeof(cut), /*spin_bound=*/64);
	CHECK_TRUE(st.per_ring[0] == PerRingStatus::SUCCESS, "healthy ring 0 delivered");
	CHECK_TRUE(st.per_ring[2] == PerRingStatus::SUCCESS, "healthy ring 2 delivered");
	CHECK_TRUE(st.per_ring[1] == PerRingStatus::WEDGED, "wedged ring 1 reported, not blocking");
	CHECK_TRUE(st.wedged_count == 1, "exactly one wedged ring");

	// The healthy rings really hold the broadcast record (draining the prefill first on 1
	// is not needed to verify 0/2).
	Msg out{};
	uint32_t len = 0;
	CHECK_TRUE(seg->down(0).TryConsume(&out, sizeof(out), &len) && out.corr_id == 42,
	           "ring 0 has the GlobalCut");
	CHECK_TRUE(seg->down(2).TryConsume(&out, sizeof(out), &len) && out.corr_id == 42,
	           "ring 2 has the GlobalCut");

	// Ring 1 is still full/wedged. The convenience BroadcastDown must also RETURN (not hang)
	// on a wedged ring and report it — regression lock for the P1-3 fidelity/liveness fix.
	Msg cut2{43, 200};
	BroadcastStatus st2 = seg->BroadcastDown(&cut2, sizeof(cut2));
	CHECK_TRUE(st2.wedged_count == 1 && st2.per_ring[1] == PerRingStatus::WEDGED,
	           "BroadcastDown returns on a wedged broker instead of hanging");
	LOG(INFO) << "[6] broadcast fairness (one wedged ring does not block others): ok";
}

// Optional POSIX-shm variant (maps /dev/shm) — run under the testbed lock with --shm.
void TestShmCreateAttach() {
	MailboxParams p;
	p.num_brokers = 2;
	p.record_size = sizeof(Msg);
	const std::string name = "/embarcadero_mailbox_smoke_" + std::to_string(getpid());
	auto coord = MailboxSegment::CreateShm(name, p);
	auto broker = MailboxSegment::AttachShm(name);
	Msg m{42, 7};
	broker->up(0).Produce(&m, sizeof(m));
	Msg out{};
	uint32_t len = 0;
	while (!coord->up(0).TryConsume(&out, sizeof(out), &len)) {}
	CHECK_TRUE(out.corr_id == 42 && out.value == 7, "shm-backed round trip");
	LOG(INFO) << "[shm] POSIX-shm create/attach: ok";
}

// Optional POSIX-shm cross-process variant: parent CreateShm, child AttachShm in a forked
// process. The child's fresh mmap almost certainly lands at a DIFFERENT virtual address,
// proving the offset-only layout is address-independent. Run under the lock with --shm.
void TestCrossProcPosixShmDifferentBase() {
	MailboxParams p;
	p.num_brokers = 1;
	p.record_size = sizeof(Msg);
	p.up_capacity = 256;
	p.down_capacity = 256;
	const std::string name = "/embarcadero_mailbox_crossproc_" + std::to_string(getpid());
	auto coord = MailboxSegment::CreateShm(name, p);  // parent owns + will unlink

	const uint64_t kN = 10'000;
	pid_t pid = fork();
	PCHECK(pid >= 0);
	if (pid == 0) {
		// Child = coordinator: attach the SAME shm at a fresh (different) base address.
		auto c = MailboxSegment::AttachShm(name);
		uint64_t granted = 0;
		while (granted < kN) {
			Msg req{};
			uint32_t len = 0;
			if (c->up(0).TryConsume(&req, sizeof(req), &len)) {
				Msg grant{req.corr_id, granted++};
				c->down(0).Produce(&grant, sizeof(grant));
			}
		}
		_exit(0);
	}
	// Parent = broker, using its own mapping.
	uint64_t bad = 0;
	for (uint64_t i = 0; i < kN; ++i) {
		Msg req{i, 0};
		coord->up(0).Produce(&req, sizeof(req));
		Msg resp{};
		uint32_t len = 0;
		while (!coord->down(0).TryConsume(&resp, sizeof(resp), &len)) {
			Embarcadero::CXL::cpu_pause();
		}
		if (resp.corr_id != i) ++bad;
	}
	int status = 0;
	waitpid(pid, &status, 0);
	CHECK_TRUE(bad == 0, "cross-process (different base) responses correlated");
	CHECK_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "coordinator exited cleanly");
	LOG(INFO) << "[5b] cross-process POSIX-shm at different base (" << kN << " msgs): ok";
}

}  // namespace

int main(int argc, char** argv) {
	google::InitGoogleLogging(argv[0]);
	FLAGS_logtostderr = 1;
	bool run_shm = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--shm") == 0) run_shm = true;
	}

	TestRingFifoAndPayload();
	TestRingBackpressure();
	TestLargeRecordOverflow();
	TestConcurrentSpsc();
	TestSegmentDuplexRoundtrip();
	TestCrossProcess();
	TestBroadcastFairness();
	if (run_shm) {
		TestShmCreateAttach();
		TestCrossProcPosixShmDifferentBase();
	}

	if (g_failures == 0) {
		LOG(INFO) << "ALL CXL MAILBOX SMOKE TESTS PASSED";
		return 0;
	}
	LOG(ERROR) << g_failures << " CHECK(S) FAILED";
	return 1;
}
