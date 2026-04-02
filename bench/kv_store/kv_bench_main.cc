// Shared-log KV store benchmark for SOSP evaluation.
//
// Measures what matters for a shared log paper:
//   1. Write throughput (pipelined appends through the log)
//   2. End-to-end write latency (publish → apply CDF)
//   3. Read-after-write throughput (mixed workload)
//   4. Throughput vs batch size
//   5. Throughput vs number of clients (future: multi-process)
//
// Design: Pipelines writes without per-op sync, matching how Tango/CORFU/SCALOG
// benchmarks work.  Sync only happens periodically or at the end.

#include "distributed_kv_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

struct BenchConfig {
	std::string sequencer = "EMBARCADERO";
	int order = -1;      // -1 = auto
	int ack = 1;
	int rf = 1;
	uint64_t record_count = 1000000;
	uint64_t operation_count = 1000000;
	size_t value_size = 100;
	int batch_size = 1;
	double write_ratio = 1.0;   // 1.0 = write-only, 0.5 = 50/50
	std::string broker_ip = "127.0.0.1";
	uint64_t warmup_ops = 10000;
	std::string output_dir = "./results/";
	std::string run_id = "auto";
	int pub_threads = 3;
	size_t pub_msg_size = 65536;
	int log_level = 1;
	int sync_interval = 0;  // 0 = sync only at end (max pipeline)
	std::string sync_barrier = "apply";  // apply|ack for intermediate syncs
	bool latency = false;   // enable per-op latency tracking (use for latency runs)
};

struct LatencyStats {
	double p50 = 0, p95 = 0, p99 = 0, p999 = 0, p9999 = 0;
	double mean = 0;
	size_t count = 0;
};

LatencyStats computeStats(std::vector<double>& v) {
	LatencyStats s;
	s.count = v.size();
	if (v.empty()) return s;
	std::sort(v.begin(), v.end());
	s.mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
	auto pct = [&](double p) {
		return v[static_cast<size_t>(p * (v.size() - 1))];
	};
	s.p50 = pct(0.50);
	s.p95 = pct(0.95);
	s.p99 = pct(0.99);
	s.p999 = pct(0.999);
	s.p9999 = pct(0.9999);
	return s;
}

std::string generateRunId() {
	auto now = std::chrono::system_clock::now();
	auto t = std::chrono::system_clock::to_time_t(now);
	std::tm tm{};
	localtime_r(&t, &tm);
	char buf[64];
	strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
	return buf;
}

std::string execCmd(const char* cmd) {
	std::array<char, 256> buf;
	std::string result;
	FILE* pipe = popen(cmd, "r");
	if (!pipe) return "unknown";
	while (fgets(buf.data(), buf.size(), pipe)) result += buf.data();
	pclose(pipe);
	while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
		result.pop_back();
	return result;
}

std::string getHostname() {
	char buf[256];
	if (gethostname(buf, sizeof(buf)) == 0) return buf;
	return "unknown";
}

int orderForSequencer(const std::string& seq) {
	if (seq == "EMBARCADERO") return 5;
	if (seq == "SCALOG") return 1;
	if (seq == "CORFU") return 2;
	if (seq == "LAZYLOG") return 2;
	return 0;
}

std::string makeKey(uint64_t n) {
	char buf[32];
	snprintf(buf, sizeof(buf), "k%012lu", static_cast<unsigned long>(n));
	return buf;
}

std::string makeValue(size_t len, std::mt19937_64& rng) {
	static const char chars[] =
		"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	std::string v(len, 'x');
	for (size_t i = 0; i < len; i++) v[i] = chars[rng() % 62];
	return v;
}

bool waitForBarrier(DistributedKVStore& store, const BenchConfig& cfg, size_t pending_opid) {
	if (cfg.sync_barrier == "apply") {
		store.waitForSyncWithLog(pending_opid);
		return true;
	}
	if (cfg.sync_barrier == "ack") {
		return store.waitForAckedWrites(pending_opid);
	}
	LOG(ERROR) << "Unknown sync_barrier: " << cfg.sync_barrier;
	return false;
}

bool runBenchmark(BenchConfig& cfg) {
	if (cfg.run_id == "auto") cfg.run_id = generateRunId();
	if (cfg.order < 0) cfg.order = orderForSequencer(cfg.sequencer);

	std::string run_dir = cfg.output_dir + "/" + cfg.sequencer +
	                       "_wr" + std::to_string(static_cast<int>(cfg.write_ratio * 100)) +
	                       "_rf" + std::to_string(cfg.rf) +
	                       "_b" + std::to_string(cfg.batch_size) +
	                       "_" + cfg.run_id;
	std::filesystem::create_directories(run_dir);

	{
		std::ofstream meta(run_dir + "/metadata.txt");
		meta << "sequencer=" << cfg.sequencer << "\norder=" << cfg.order
		     << "\nack=" << cfg.ack << "\nrf=" << cfg.rf
		     << "\nrecord_count=" << cfg.record_count
		     << "\noperation_count=" << cfg.operation_count
		     << "\nvalue_size=" << cfg.value_size
		     << "\nbatch_size=" << cfg.batch_size
		     << "\nwrite_ratio=" << cfg.write_ratio
		     << "\nsync_interval=" << cfg.sync_interval
		     << "\nsync_barrier=" << cfg.sync_barrier
		     << "\nlatency_tracking=" << cfg.latency
		     << "\nbroker_ip=" << cfg.broker_ip
		     << "\nwarmup_ops=" << cfg.warmup_ops
		     << "\nhostname=" << getHostname()
		     << "\ngit_commit=" << execCmd("git rev-parse --short HEAD 2>/dev/null")
		     << "\ngit_dirty=" << execCmd("git diff --quiet && echo clean || echo dirty")
		     << "\n";
	}

	SequencerType seq_type = parseSequencerType(cfg.sequencer);
	DistributedKVStore::Config kv_cfg;
	kv_cfg.seq_type = seq_type;
	kv_cfg.order = cfg.order;
	kv_cfg.ack_level = cfg.ack;
	kv_cfg.replication_factor = cfg.rf;
	kv_cfg.publisher_threads = cfg.pub_threads;
	kv_cfg.publisher_message_size = cfg.pub_msg_size;
	kv_cfg.broker_ip = cfg.broker_ip;
	kv_cfg.manage_cluster = true;
	kv_cfg.track_latency = cfg.latency;

	LOG(INFO) << "Creating KV store: " << cfg.sequencer
	          << " order=" << cfg.order << " ack=" << cfg.ack
	          << " rf=" << cfg.rf << " broker=" << cfg.broker_ip;

	DistributedKVStore store(kv_cfg);

	std::mt19937_64 rng(42);
	std::string template_value = makeValue(cfg.value_size, rng);

	// ========== LOAD PHASE ==========
	// Pipeline inserts with periodic barriers. A single end-of-load sync can stall on ORDER=5
	// when the publisher outruns the subscriber/log-apply path (bounded CXL / broker batching).
	// Tighter sync helps SCALOG / slow-apply paths keep up with the publisher during load.
	const uint64_t kLoadSyncEvery =
	    (cfg.record_count > 256 && cfg.batch_size <= 4) ? 64u : UINT64_MAX;
	LOG(INFO) << "Loading " << cfg.record_count << " records (pipelined, sync_every="
	          << (kLoadSyncEvery == UINT64_MAX ? 0 : kLoadSyncEvery) << ")...";
	auto load_t0 = std::chrono::steady_clock::now();
	size_t last_opid = 0;
	uint64_t pending_since_sync = 0;
	uint64_t load_entries = 0;

	for (uint64_t i = 0; i < cfg.record_count; i += cfg.batch_size) {
		uint64_t end = std::min(i + static_cast<uint64_t>(cfg.batch_size), cfg.record_count);
		std::vector<KeyValue> batch;
		batch.reserve(end - i);
		for (uint64_t j = i; j < end; j++) {
			batch.push_back({makeKey(j), template_value});
		}
		last_opid = store.multiPut(batch);
		load_entries++;
		pending_since_sync += batch.size();
		if (pending_since_sync >= kLoadSyncEvery) {
			store.waitForSyncWithLog();
			pending_since_sync = 0;
		}
	}
	store.waitForSyncWithLog();

	auto load_t1 = std::chrono::steady_clock::now();
	double load_sec = std::chrono::duration<double>(load_t1 - load_t0).count();
	double load_tput = static_cast<double>(cfg.record_count) / load_sec;
	LOG(INFO) << "Load: " << cfg.record_count << " records in "
	          << std::fixed << std::setprecision(2) << load_sec << "s  ("
	          << std::setprecision(0) << load_tput << " ops/s)"
	          << "  store_size=" << store.storeSize();

	store.collectApplyLatenciesAndReset();

	// ========== WARMUP ==========
	if (cfg.warmup_ops > 0) {
		LOG(INFO) << "Warmup: " << cfg.warmup_ops << " ops...";
		std::uniform_int_distribution<uint64_t> key_dist(0, cfg.record_count - 1);
		uint64_t warmup_entries = 0;
		for (uint64_t i = 0; i < cfg.warmup_ops; i++) {
			last_opid = store.put(makeKey(key_dist(rng)), template_value);
			warmup_entries++;
		}
		store.waitForSyncWithLog(last_opid);
		store.collectApplyLatenciesAndReset();
		load_entries += warmup_entries;
	}

	// ========== RUN PHASE ==========
	//
	// Two modes depending on write_ratio:
	//   write_ratio=1.0: pure write throughput (what CORFU/SCALOG report)
	//   write_ratio<1.0: mixed read/write (shows SMR application behavior)
	//
	// Writes are pipelined (fire without per-op sync).
	// We sync at sync_interval boundaries, or only at the end if sync_interval=0.
	// This matches the evaluation methodology of Tango, CORFU, and SCALOG.
	LOG(INFO) << "Run: " << cfg.operation_count << " ops, write_ratio="
	          << cfg.write_ratio << ", batch=" << cfg.batch_size
	          << ", sync_interval=" << cfg.sync_interval;

	std::uniform_int_distribution<uint64_t> key_dist(0, cfg.record_count - 1);
	std::uniform_real_distribution<double> op_dist(0.0, 1.0);

	std::vector<double> write_latencies_us;
	std::vector<double> read_latencies_us;
	if (cfg.latency) {
		write_latencies_us.reserve(
			static_cast<size_t>(cfg.operation_count * cfg.write_ratio) + 1);
		if (cfg.write_ratio < 1.0) {
			read_latencies_us.reserve(
				static_cast<size_t>(cfg.operation_count * (1.0 - cfg.write_ratio)) + 1);
		}
	}

	uint64_t writes = 0, reads = 0;
	size_t pending_opid = 0;
	uint64_t ops_since_sync = 0;
	uint64_t run_write_entries = 0;
	bool sync_failed = false;
	bool run_aborted = false;

	auto run_t0 = std::chrono::steady_clock::now();

	uint64_t i = 0;
	while (i < cfg.operation_count) {
		bool do_write = (cfg.write_ratio >= 1.0) || (op_dist(rng) < cfg.write_ratio);

		if (do_write) {
			uint64_t batch_end = std::min(
				i + static_cast<uint64_t>(cfg.batch_size), cfg.operation_count);
			std::vector<KeyValue> batch;
			batch.reserve(batch_end - i);
			for (uint64_t j = i; j < batch_end; j++) {
				batch.push_back({makeKey(key_dist(rng)), template_value});
			}
			if (cfg.latency) {
				auto t0 = std::chrono::steady_clock::now();
				pending_opid = store.publishBatch(batch);
				auto t1 = std::chrono::steady_clock::now();
				double pub_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
				double per_op = pub_us / batch.size();
				for (size_t b = 0; b < batch.size(); b++) {
					write_latencies_us.push_back(per_op);
				}
			} else {
				pending_opid = store.publishBatch(batch);
			}
			run_write_entries++;
			writes += batch.size();
			ops_since_sync += batch.size();
			i = batch_end;
		} else {
			if (cfg.latency) {
				auto t0 = std::chrono::steady_clock::now();
				store.get(makeKey(key_dist(rng)));
				auto t1 = std::chrono::steady_clock::now();
				read_latencies_us.push_back(
					std::chrono::duration<double, std::micro>(t1 - t0).count());
			} else {
				store.get(makeKey(key_dist(rng)));
			}
			reads++;
			ops_since_sync++;
			i++;
		}

		if (cfg.sync_interval > 0 &&
		    ops_since_sync >= static_cast<uint64_t>(cfg.sync_interval)) {
			if (!waitForBarrier(store, cfg, pending_opid)) {
				sync_failed = true;
				run_aborted = true;
				break;
			}
			ops_since_sync = 0;
		}
	}

	// Final sync: wait for all pipelined writes to be applied
	if (pending_opid > 0) {
		store.waitForSyncWithLog(pending_opid);
	}

	auto run_t1 = std::chrono::steady_clock::now();
	double run_sec = std::chrono::duration<double>(run_t1 - run_t0).count();
	double total_ops = static_cast<double>(writes + reads);
	double throughput = total_ops / run_sec;
	double write_throughput = static_cast<double>(writes) / run_sec;

	// Verification: confirm all writes were actually applied
	size_t final_store_size = store.storeSize();
	uint64_t applied_entries = store.getAppliedLocalOpCount();
	uint64_t expected_applied_entries = load_entries + run_write_entries;
	bool run_valid = true;
	if (sync_failed) {
		LOG(ERROR) << "VERIFICATION FAILED: intermediate " << cfg.sync_barrier
		           << " barrier timed out";
		run_valid = false;
	}
	if (cfg.record_count > 0 && final_store_size != cfg.record_count) {
		LOG(ERROR) << "VERIFICATION FAILED: store_size=" << final_store_size
		           << " expected=" << cfg.record_count;
		run_valid = false;
	}
	if (applied_entries != expected_applied_entries) {
		LOG(ERROR) << "VERIFICATION FAILED: applied_entries=" << applied_entries
		           << " expected=" << expected_applied_entries;
		run_valid = false;
	}
	if (run_sec < 0.01 && writes > 100) {
		LOG(WARNING) << "Run duration suspiciously short (" << run_sec
		             << "s) — results may reflect startup transients, not steady state";
	}

	// Collect the internal apply latencies (publish -> consumer applies)
	auto apply_lats = store.collectApplyLatenciesAndReset();
	for (auto& v : apply_lats) v *= 1000.0;
	auto apply_stats = computeStats(apply_lats);

	auto write_stats = computeStats(write_latencies_us);
	auto read_stats = computeStats(read_latencies_us);

	LOG(INFO) << "=== Results ===";
	LOG(INFO) << "Runtime: " << std::fixed << std::setprecision(3) << run_sec << " s";
	LOG(INFO) << "Ops: " << writes << " writes + " << reads << " reads = "
	          << static_cast<uint64_t>(total_ops);
	LOG(INFO) << "Throughput: " << std::setprecision(0) << throughput << " ops/s  ("
	          << write_throughput << " write ops/s)";
	LOG(INFO) << "Store size: " << final_store_size
	          << "  last_applied_total_order=" << store.getLastAppliedIndex()
	          << "  applied_entries=" << applied_entries << "/" << expected_applied_entries
	          << "  sync_barrier=" << cfg.sync_barrier
	          << "  run_aborted=" << (run_aborted ? "YES" : "NO")
	          << "  valid=" << (run_valid ? "YES" : "NO");
	if (write_stats.count > 0) {
		LOG(INFO) << "Publish latency (us): p50=" << std::setprecision(1)
		          << write_stats.p50 << " p95=" << write_stats.p95
		          << " p99=" << write_stats.p99 << " p99.9=" << write_stats.p999;
	}
	if (apply_stats.count > 0) {
		LOG(INFO) << "Apply latency (us):   p50=" << std::setprecision(1)
		          << apply_stats.p50 << " p95=" << apply_stats.p95
		          << " p99=" << apply_stats.p99 << " p99.9=" << apply_stats.p999;
	}
	if (read_stats.count > 0) {
		LOG(INFO) << "Read latency (us):    p50=" << std::setprecision(1)
		          << read_stats.p50 << " p95=" << read_stats.p95
		          << " p99=" << read_stats.p99;
	}

	// ========== CSV OUTPUT ==========
	{
		std::ofstream csv(run_dir + "/summary.csv");
		csv << "sequencer,order,ack,rf,record_count,operation_count,"
		    << "value_size,batch_size,write_ratio,sync_interval,sync_barrier,"
		    << "runtime_sec,throughput_ops_sec,write_throughput_ops_sec,"
		    << "writes,reads,store_size,published_entries,applied_entries,valid,"
		    << "pub_p50_us,pub_p95_us,pub_p99_us,pub_p999_us,"
		    << "apply_p50_us,apply_p95_us,apply_p99_us,apply_p999_us,"
		    << "read_p50_us,read_p99_us\n";
		csv << cfg.sequencer << "," << cfg.order << "," << cfg.ack << "," << cfg.rf
		    << "," << cfg.record_count << "," << cfg.operation_count
		    << "," << cfg.value_size << "," << cfg.batch_size
		    << "," << cfg.write_ratio << "," << cfg.sync_interval
		    << "," << cfg.sync_barrier
		    << "," << std::fixed << std::setprecision(6) << run_sec
		    << "," << std::setprecision(0) << throughput
		    << "," << write_throughput
		    << "," << writes << "," << reads
		    << "," << final_store_size
		    << "," << expected_applied_entries
		    << "," << applied_entries
		    << "," << (run_valid ? 1 : 0)
		    << "," << std::setprecision(1)
		    << write_stats.p50 << "," << write_stats.p95
		    << "," << write_stats.p99 << "," << write_stats.p999
		    << "," << apply_stats.p50 << "," << apply_stats.p95
		    << "," << apply_stats.p99 << "," << apply_stats.p999
		    << "," << read_stats.p50 << "," << read_stats.p99
		    << "\n";
	}

	// Raw apply latency samples for CDF plotting
	if (!apply_lats.empty()) {
		std::ofstream f(run_dir + "/apply_latency_us.csv");
		f << "latency_us\n";
		for (auto v : apply_lats) f << std::fixed << std::setprecision(2) << v << "\n";
	}

	LOG(INFO) << "Results: " << run_dir;
	return run_valid;
}

}  // namespace

int main(int argc, char* argv[]) {
	google::InitGoogleLogging(argv[0]);
	google::InstallFailureSignalHandler();
	FLAGS_logtostderr = 1;

	cxxopts::Options options("kv_bench", "Shared-Log KV Store Benchmark");
	options.add_options()
		("sequencer", "EMBARCADERO|SCALOG|CORFU|LAZYLOG",
		 cxxopts::value<std::string>()->default_value("EMBARCADERO"))
		("order", "Order level (-1=auto, 0,1,2,5)",
		 cxxopts::value<int>()->default_value("-1"))
		("ack", "ACK level (0|1|2)",
		 cxxopts::value<int>()->default_value("1"))
		("rf", "Replication factor",
		 cxxopts::value<int>()->default_value("1"))
		("record_count", "Records to pre-load",
		 cxxopts::value<uint64_t>()->default_value("1000000"))
		("operation_count", "Operations in run phase",
		 cxxopts::value<uint64_t>()->default_value("1000000"))
		("value_size", "Value size in bytes",
		 cxxopts::value<size_t>()->default_value("100"))
		("batch_size", "KV ops per log append",
		 cxxopts::value<int>()->default_value("1"))
		("write_ratio", "Fraction of writes (1.0=write-only, 0.5=50/50)",
		 cxxopts::value<double>()->default_value("1.0"))
		("sync_interval", "Sync every N ops (0=end-only, max pipeline)",
		 cxxopts::value<int>()->default_value("0"))
		("sync_barrier", "Intermediate sync barrier (apply|ack)",
		 cxxopts::value<std::string>()->default_value("apply"))
		("latency", "Enable per-op latency tracking (adds overhead; use for latency runs)")
		("broker_ip", "Broker address",
		 cxxopts::value<std::string>()->default_value("127.0.0.1"))
		("warmup_ops", "Warmup ops (not measured)",
		 cxxopts::value<uint64_t>()->default_value("10000"))
		("output_dir", "Output directory",
		 cxxopts::value<std::string>()->default_value("./results/"))
		("run_id", "Run ID (auto=timestamp)",
		 cxxopts::value<std::string>()->default_value("auto"))
		("pub_threads", "Publisher threads",
		 cxxopts::value<int>()->default_value("3"))
		("pub_msg", "Publisher message buffer",
		 cxxopts::value<size_t>()->default_value("65536"))
		("l,log_level", "VLOG level",
		 cxxopts::value<int>()->default_value("1"))
		("h,help", "Print usage");

	auto result = options.parse(argc, argv);
	if (result.count("help")) {
		std::cout << options.help() << std::endl;
		return 0;
	}

	BenchConfig cfg;
	cfg.sequencer = result["sequencer"].as<std::string>();
	cfg.order = result["order"].as<int>();
	cfg.ack = result["ack"].as<int>();
	cfg.rf = result["rf"].as<int>();
	cfg.record_count = result["record_count"].as<uint64_t>();
	cfg.operation_count = result["operation_count"].as<uint64_t>();
	cfg.value_size = result["value_size"].as<size_t>();
	cfg.batch_size = result["batch_size"].as<int>();
	cfg.write_ratio = result["write_ratio"].as<double>();
	cfg.sync_interval = result["sync_interval"].as<int>();
	cfg.sync_barrier = result["sync_barrier"].as<std::string>();
	cfg.latency = result.count("latency") > 0;
	cfg.broker_ip = result["broker_ip"].as<std::string>();
	cfg.warmup_ops = result["warmup_ops"].as<uint64_t>();
	cfg.output_dir = result["output_dir"].as<std::string>();
	cfg.run_id = result["run_id"].as<std::string>();
	cfg.pub_threads = result["pub_threads"].as<int>();
	cfg.pub_msg_size = result["pub_msg"].as<size_t>();
	cfg.log_level = result["log_level"].as<int>();
	FLAGS_v = cfg.log_level;

	LOG(INFO) << "=== Shared-Log KV Benchmark ===";
	LOG(INFO) << cfg.sequencer << " order=" << cfg.order << " ack=" << cfg.ack
	          << " rf=" << cfg.rf;
	LOG(INFO) << "ops=" << cfg.operation_count << " batch=" << cfg.batch_size
	          << " write_ratio=" << cfg.write_ratio
	          << " sync_interval=" << cfg.sync_interval
	          << " sync_barrier=" << cfg.sync_barrier
	          << " latency=" << cfg.latency;

	return runBenchmark(cfg) ? 0 : 1;
}
