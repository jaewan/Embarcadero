// Standalone deterministic checks for Corfu global sequencer ordering (no GTest dependency).
// Build: corfu_sequencer_fifo_smoke (see src/CMakeLists.txt). Exit 0 on success, 1 on failure.

#include <grpcpp/grpcpp.h>

#include "corfu_sequencer.grpc.pb.h"
#include "cxl_manager/corfu_sequencer_service.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

using corfusequencer::CorfuSequencer;
using corfusequencer::TotalOrderRequest;
using corfusequencer::TotalOrderResponse;

static int fail(const char* msg) {
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return 1;
}

static std::unique_ptr<grpc::Server> StartTestServer(CorfuSequencerImpl* service, int* out_port) {
	int selected_port = 0;
	grpc::ServerBuilder builder;
	builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &selected_port);
	builder.RegisterService(service);
	std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
	if (!server || selected_port <= 0) {
		return nullptr;
	}
	*out_port = selected_port;
	return server;
}

static int RunTwoClientsInterleaved(const std::string& addr) {
	auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
	std::unique_ptr<CorfuSequencer::Stub> stub = CorfuSequencer::NewStub(channel);

	struct Obs {
		uint64_t client_id;
		uint64_t batch_seq;
		uint64_t total_order;
	};
	std::vector<Obs> obs;
	const int kRounds = 64;

	uint64_t seq_a = 0;
	uint64_t seq_b = 0;
	for (int i = 0; i < kRounds; ++i) {
		auto round_trip = [&](uint64_t cid, uint64_t bseq, int broker) -> bool {
			TotalOrderRequest req;
			req.set_client_id(cid);
			req.set_batchseq(bseq);
			req.set_num_msg(1);
			req.set_total_size(128);
			req.set_broker_id(static_cast<uint32_t>(broker));
			TotalOrderResponse resp;
			grpc::ClientContext ctx;
			if (!stub->GetTotalOrder(&ctx, req, &resp).ok()) {
				return false;
			}
			obs.push_back({cid, bseq, resp.total_order()});
			return true;
		};
		if (!round_trip(100, seq_a++, 0)) {
			return fail("GetTotalOrder failed (Alice)");
		}
		if (!round_trip(200, seq_b++, 0)) {
			return fail("GetTotalOrder failed (Bob)");
		}
	}

	for (uint64_t cid : {100ull, 200ull}) {
		std::vector<Obs> sub;
		for (const auto& o : obs) {
			if (o.client_id == cid) {
				sub.push_back(o);
			}
		}
		std::sort(sub.begin(), sub.end(), [](const Obs& x, const Obs& y) {
			return x.total_order < y.total_order;
		});
		for (size_t j = 1; j < sub.size(); ++j) {
			if (sub[j].batch_seq <= sub[j - 1].batch_seq) {
				return fail("intra-client inversion (two-client interleave test)");
			}
		}
	}
	return 0;
}

static int RunSingleClientTwoBrokersConcurrent(const std::string& addr) {
	auto ch0 = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
	auto ch1 = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
	std::unique_ptr<CorfuSequencer::Stub> s0 = CorfuSequencer::NewStub(ch0);
	std::unique_ptr<CorfuSequencer::Stub> s1 = CorfuSequencer::NewStub(ch1);

	const uint64_t kClient = 42;
	uint64_t prev_max = 0;
	bool have_prev = false;

	pthread_barrier_t bar;
	if (pthread_barrier_init(&bar, nullptr, 2) != 0) {
		return fail("pthread_barrier_init");
	}

	for (int wave = 0; wave < 128; ++wave) {
		uint64_t o0 = 0;
		uint64_t o1 = 0;
		bool ok0 = true;
		bool ok1 = true;

		auto run0 = [&] {
			pthread_barrier_wait(&bar);
			TotalOrderRequest req;
			req.set_client_id(kClient);
			req.set_batchseq(static_cast<uint64_t>(wave));
			req.set_num_msg(1);
			req.set_total_size(64);
			req.set_broker_id(0);
			TotalOrderResponse resp;
			grpc::ClientContext ctx;
			ok0 = s0->GetTotalOrder(&ctx, req, &resp).ok();
			o0 = resp.total_order();
		};
		auto run1 = [&] {
			pthread_barrier_wait(&bar);
			TotalOrderRequest req;
			req.set_client_id(kClient);
			req.set_batchseq(static_cast<uint64_t>(wave));
			req.set_num_msg(1);
			req.set_total_size(64);
			req.set_broker_id(1);
			TotalOrderResponse resp;
			grpc::ClientContext ctx;
			ok1 = s1->GetTotalOrder(&ctx, req, &resp).ok();
			o1 = resp.total_order();
		};
		std::thread t0(run0);
		std::thread t1(run1);
		t0.join();
		t1.join();

		if (!ok0 || !ok1) {
			pthread_barrier_destroy(&bar);
			return fail("GetTotalOrder failed (concurrent two-broker)");
		}

		const uint64_t lo = std::min(o0, o1);
		const uint64_t hi = std::max(o0, o1);
		if (hi != lo + 1) {
			pthread_barrier_destroy(&bar);
			return fail("expected consecutive total_order per wave");
		}
		if (have_prev && lo <= prev_max) {
			pthread_barrier_destroy(&bar);
			return fail("wave isolation / monotonicity broken");
		}
		if (!have_prev && lo != 0) {
			pthread_barrier_destroy(&bar);
			return fail("expected first total_order 0");
		}
		have_prev = true;
		prev_max = hi;
	}
	pthread_barrier_destroy(&bar);
	return 0;
}

int main() {
	// Fresh sequencer state per suite (next_order_ is process-global per instance).
	{
		CorfuSequencerImpl service;
		int port = 0;
		std::unique_ptr<grpc::Server> server = StartTestServer(&service, &port);
		if (!server) {
			return fail("failed to start in-process gRPC server (suite 1)");
		}
		const std::string addr = "127.0.0.1:" + std::to_string(port);
		if (RunTwoClientsInterleaved(addr) != 0) {
			server->Shutdown();
			return 1;
		}
		server->Shutdown();
	}
	{
		CorfuSequencerImpl service;
		int port = 0;
		std::unique_ptr<grpc::Server> server = StartTestServer(&service, &port);
		if (!server) {
			return fail("failed to start in-process gRPC server (suite 2)");
		}
		const std::string addr = "127.0.0.1:" + std::to_string(port);
		if (RunSingleClientTwoBrokersConcurrent(addr) != 0) {
			server->Shutdown();
			return 1;
		}
		server->Shutdown();
	}
	std::fprintf(stderr, "OK: corfu_sequencer_fifo_smoke passed\n");
	return 0;
}
