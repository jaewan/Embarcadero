// Shared-log KV store benchmark for SOSP evaluation.
//
// Measures what matters for a shared log paper:
//   1. Write throughput (pipelined appends through the log)
//   2. End-to-end write latency (publish → apply CDF)
//   3. Read-after-write throughput (mixed workload)
//   4. Throughput vs batch size
//   5. Throughput vs number of clients (future: multi-process)
//
// YCSB workloads A-F are supported via --workload=<A|B|C|D|E|F>.
// Key distributions: uniform (default) or zipf (--key_dist=zipf).
//
// Design: Pipelines writes without per-op sync, matching how Tango/CORFU/SCALOG
// benchmarks work.  Sync only happens periodically or at the end.

#include "distributed_kv_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

// ---------------------------------------------------------------------------
// ZipfDistribution
//
// Standard Zipf with exponent theta: P(k) ∝ 1/k^theta for k = 1..N.
// Sampling via binary search on a pre-computed normalized CDF table.
//
// For large N (>1M) the CDF is built over kMaxTableEntries=1M quantiles
// and the result is interpolated back into [0, N-1].
//
// Special cases:
//   N == 0          -> always returns 0
//   theta == 0      -> uniform over [0, N-1] (no table built)
// ---------------------------------------------------------------------------
class ZipfDistribution {
public:
	static constexpr size_t kMaxTableEntries = 1000000;

	ZipfDistribution() : n_(0), theta_(0.99) {}

	ZipfDistribution(uint64_t n, double theta = 0.99)
		: n_(n), theta_(theta) {
		if (n_ == 0) return;
		build();
	}

	// Returns a sample in [0, N-1].
	uint64_t sample(std::mt19937_64& rng) const {
		if (n_ == 0) return 0;
		if (theta_ == 0.0 || cdf_.empty()) {
			// Uniform fallback
			return rng() % n_;
		}
		std::uniform_real_distribution<double> u(0.0, 1.0);
		double r = u(rng);
		// Binary search for smallest index where cdf_[i] >= r
		auto it = std::lower_bound(cdf_.begin(), cdf_.end(), r);
		size_t idx = static_cast<size_t>(it - cdf_.begin());
		if (idx >= cdf_.size()) idx = cdf_.size() - 1;
		// Map table index back to key index in [0, N-1]
		if (cdf_.size() == static_cast<size_t>(n_)) {
			return static_cast<uint64_t>(idx);
		}
		// Interpolated: table covers kMaxTableEntries buckets over N items
		double frac = static_cast<double>(idx) / static_cast<double>(cdf_.size() - 1);
		uint64_t key = static_cast<uint64_t>(frac * static_cast<double>(n_ - 1));
		return std::min(key, n_ - 1);
	}

private:
	uint64_t n_;
	double theta_;
	std::vector<double> cdf_;

	void build() {
		if (theta_ == 0.0) return;  // uniform — no table needed
		size_t table_size = (n_ <= static_cast<uint64_t>(kMaxTableEntries))
			? static_cast<size_t>(n_)
			: kMaxTableEntries;

		cdf_.resize(table_size);
		double sum = 0.0;
		for (size_t i = 0; i < table_size; ++i) {
			double rank = static_cast<double>(i + 1);
			cdf_[i] = 1.0 / std::pow(rank, theta_);
			sum += cdf_[i];
		}
		// Normalize to CDF
		double running = 0.0;
		for (size_t i = 0; i < table_size; ++i) {
			running += cdf_[i];
			cdf_[i] = running / sum;
		}
		cdf_.back() = 1.0;  // guard against floating-point rounding
	}
};

struct BenchConfig {
	std::string sequencer = "EMBARCADERO";
	int order = -1;      // -1 = auto
	int ack = 1;
	int rf = 0;  // Default RF=0: no replication client created, avoids 5s Connect() block on single-node eval
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

	// YCSB extensions
	std::string workload = "";        // empty = use write_ratio directly (backward compat)
	std::string key_dist = "uniform"; // uniform|zipf|latest
	double zipf_theta = 0.99;
	int scan_len = 100;               // number of keys per scan (workload E)
};

// ---------------------------------------------------------------------------
// Apply YCSB workload presets. Called after CLI options are parsed.
// Sets write_ratio and key_dist according to standard YCSB definitions.
// Does NOT change write_ratio when --workload is not set (backward compat).
// ---------------------------------------------------------------------------
void applyWorkloadPreset(BenchConfig& cfg) {
	if (cfg.workload.empty()) return;
	// Normalize to uppercase for comparison
	std::string wl = cfg.workload;
	wl[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(wl[0])));

	if (wl == "A") {
		// 50% reads, 50% updates, uniform key dist
		cfg.write_ratio = 0.5;
		cfg.key_dist = "uniform";
	} else if (wl == "B") {
		// 95% reads, 5% updates, uniform key dist
		cfg.write_ratio = 0.05;
		cfg.key_dist = "uniform";
	} else if (wl == "C") {
		// 100% reads, uniform key dist
		cfg.write_ratio = 0.0;
		cfg.key_dist = "uniform";
	} else if (wl == "D") {
		// 95% reads, 5% inserts, latest key dist
		cfg.write_ratio = 0.05;
		cfg.key_dist = "latest";
	} else if (wl == "E") {
		// 5% inserts, 95% scans, uniform key dist
		cfg.write_ratio = 0.05;
		cfg.key_dist = "uniform";
	} else if (wl == "F") {
		// 50% reads + 50% RMW; no pure writes
		cfg.write_ratio = 0.0;
		cfg.key_dist = "uniform";
	} else {
		LOG(WARNING) << "Unknown workload '" << cfg.workload << "' — ignoring preset";
	}
}

// ---------------------------------------------------------------------------
// Sample a key index using the configured distribution.
// latest_insert_idx: highest key index written so far (used for "latest" dist).
// ---------------------------------------------------------------------------
uint64_t sampleKey(const BenchConfig& cfg,
                   const ZipfDistribution& zipf,
                   std::mt19937_64& rng,
                   uint64_t latest_insert_idx) {
	if (cfg.key_dist == "zipf") {
		return zipf.sample(rng);
	}
	if (cfg.key_dist == "latest") {
		// 80% of reads go to the newest 20% of keys; 20% are uniform.
		uint64_t base = (latest_insert_idx > 0) ? latest_insert_idx : cfg.record_count - 1;
		uint64_t recent_window = std::max(uint64_t{1}, base / 5);
		uint64_t recent_start = (base >= recent_window) ? (base - recent_window + 1) : 0;
		std::uniform_real_distribution<double> p(0.0, 1.0);
		if (p(rng) < 0.8) {
			uint64_t range = base - recent_start + 1;
			return recent_start + (rng() % range);
		}
		return rng() % (base + 1);
	}
	// Default: uniform over [0, record_count - 1]
	return rng() % cfg.record_count;
}

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

	// Normalize workload label for run_dir naming
	std::string wl_label = cfg.workload;
	if (!wl_label.empty())
		wl_label[0] = static_cast<char>(
			std::toupper(static_cast<unsigned char>(wl_label[0])));

	std::string run_dir = cfg.output_dir + "/" + cfg.sequencer +
	                       (wl_label.empty()
	                            ? "_wr" + std::to_string(static_cast<int>(cfg.write_ratio * 100))
	                            : "_wl" + wl_label) +
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
		     << "\nworkload=" << cfg.workload
		     << "\nkey_dist=" << cfg.key_dist
		     << "\nzipf_theta=" << cfg.zipf_theta
		     << "\nscan_len=" << cfg.scan_len
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
	store.setScanRecordCount(cfg.record_count);

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
		std::uniform_int_distribution<uint64_t> wu_key_dist(0, cfg.record_count - 1);
		uint64_t warmup_entries = 0;
		for (uint64_t i = 0; i < cfg.warmup_ops; i++) {
			last_opid = store.put(makeKey(wu_key_dist(rng)), template_value);
			warmup_entries++;
		}
		store.waitForSyncWithLog(last_opid);
		store.collectApplyLatenciesAndReset();
		load_entries += warmup_entries;
	}

	// ========== RUN PHASE ==========
	//
	// Workload-aware dispatch:
	//   Workload E: 5% inserts + 95% scans
	//   Workload F: 50% reads + 50% RMW (no pure writes)
	//   Others (A/B/C/D) and manual write_ratio mode: write or read per write_ratio
	//
	// Writes are pipelined (fire without per-op sync).
	// We sync at sync_interval boundaries, or only at the end if sync_interval=0.

	std::string wl_upper = cfg.workload;
	if (!wl_upper.empty())
		wl_upper[0] = static_cast<char>(
			std::toupper(static_cast<unsigned char>(wl_upper[0])));
	const bool is_workload_D = (wl_upper == "D");
	const bool is_workload_E = (wl_upper == "E");
	const bool is_workload_F = (wl_upper == "F");

	LOG(INFO) << "Run: " << cfg.operation_count << " ops, write_ratio="
	          << cfg.write_ratio << ", batch=" << cfg.batch_size
	          << ", workload=" << (cfg.workload.empty() ? "manual" : cfg.workload)
	          << ", key_dist=" << cfg.key_dist
	          << (cfg.key_dist == "zipf" ? " theta=" + std::to_string(cfg.zipf_theta) : "")
	          << (is_workload_E ? " scan_len=" + std::to_string(cfg.scan_len) : "")
	          << ", sync_interval=" << cfg.sync_interval;

	// Build Zipf sampler (N=0 disables Zipf table; sample() falls back to uniform)
	ZipfDistribution zipf_dist(
		(cfg.key_dist == "zipf") ? cfg.record_count : 0,
		cfg.zipf_theta);

	std::uniform_real_distribution<double> op_dist(0.0, 1.0);

	std::vector<double> write_latencies_us;
	std::vector<double> read_latencies_us;
	if (cfg.latency) {
		// Workload F has write_ratio=0.0 (used to detect the workload) but issues
		// RMWs for 50% of operations.  Reserving operation_count*0.0+1 = 1 entry
		// then pushing ~operation_count/2 samples causes O(log N) reallocations
		// that inflate measured RMW latency.  Similarly, read_latencies_us should
		// only be sized for the read half, not the full operation_count.
		if (is_workload_F) {
			write_latencies_us.reserve(cfg.operation_count / 2 + 1);
			read_latencies_us.reserve(cfg.operation_count / 2 + 1);
		} else if (is_workload_E) {
			// E: 5% inserts (writes), 95% scans (reads)
			write_latencies_us.reserve(
				static_cast<size_t>(cfg.operation_count * cfg.write_ratio) + 1);
			read_latencies_us.reserve(
				static_cast<size_t>(cfg.operation_count * (1.0 - cfg.write_ratio)) + 1);
		} else {
			write_latencies_us.reserve(
				static_cast<size_t>(cfg.operation_count * cfg.write_ratio) + 1);
			if (cfg.write_ratio < 1.0) {
				read_latencies_us.reserve(
					static_cast<size_t>(cfg.operation_count * (1.0 - cfg.write_ratio)) + 1);
			}
		}
	}

	uint64_t writes = 0, reads = 0, scans = 0, rmws = 0;
	size_t pending_opid = 0;
	uint64_t ops_since_sync = 0;
	uint64_t run_write_entries = 0;
	bool sync_failed = false;
	bool run_aborted = false;

	// For workload D: track the highest key index ever inserted (during run phase).
	// The load phase covered [0, record_count-1]; new inserts go above that.
	uint64_t latest_insert_idx = cfg.record_count - 1;

	auto run_t0 = std::chrono::steady_clock::now();

	uint64_t i = 0;
	while (i < cfg.operation_count) {
		double dice = op_dist(rng);

		// ---- Workload E: inserts (5%) vs scans (95%) ----
		if (is_workload_E) {
			bool do_insert = (dice < cfg.write_ratio);
			if (do_insert) {
				++latest_insert_idx;
				std::string new_key = makeKey(latest_insert_idx);
				if (cfg.latency) {
					auto t0 = std::chrono::steady_clock::now();
					pending_opid = store.put(new_key, template_value);
					auto t1 = std::chrono::steady_clock::now();
					write_latencies_us.push_back(
						std::chrono::duration<double, std::micro>(t1 - t0).count());
				} else {
					pending_opid = store.put(new_key, template_value);
				}
				run_write_entries++;
				writes++;
				ops_since_sync++;
			} else {
				uint64_t start_id = sampleKey(cfg, zipf_dist, rng, latest_insert_idx);
				std::string start_key = makeKey(start_id);
				// YCSB E specifies a uniformly random scan length in [1, 100] per
				// operation, not a fixed cfg.scan_len.  Using a fixed length doubles
				// average work per scan and makes results incomparable to standard YCSB E.
				int scan_len = 1 + static_cast<int>(rng() % 100);
				if (cfg.latency) {
					auto t0 = std::chrono::steady_clock::now();
					store.scan(start_key, scan_len);
					auto t1 = std::chrono::steady_clock::now();
					read_latencies_us.push_back(
						std::chrono::duration<double, std::micro>(t1 - t0).count());
				} else {
					store.scan(start_key, scan_len);
				}
				scans++;
				ops_since_sync++;
			}
			i++;

		// ---- Workload F: reads (50%) vs RMW (50%) ----
		} else if (is_workload_F) {
			bool do_rmw = (dice >= 0.5);
			uint64_t key_id = sampleKey(cfg, zipf_dist, rng, latest_insert_idx);
			std::string key = makeKey(key_id);
			if (do_rmw) {
				if (cfg.latency) {
					auto t0 = std::chrono::steady_clock::now();
					pending_opid = store.readModifyWrite(key, cfg.value_size);
					auto t1 = std::chrono::steady_clock::now();
					write_latencies_us.push_back(
						std::chrono::duration<double, std::micro>(t1 - t0).count());
				} else {
					pending_opid = store.readModifyWrite(key, cfg.value_size);
				}
				run_write_entries++;
				rmws++;
				ops_since_sync++;
			} else {
				if (cfg.latency) {
					auto t0 = std::chrono::steady_clock::now();
					store.get(key);
					auto t1 = std::chrono::steady_clock::now();
					read_latencies_us.push_back(
						std::chrono::duration<double, std::micro>(t1 - t0).count());
				} else {
					store.get(key);
				}
				reads++;
				ops_since_sync++;
			}
			i++;

		// ---- Standard workloads (A/B/C/D) and manual write_ratio mode ----
		} else {
			bool do_write = (cfg.write_ratio >= 1.0) || (dice < cfg.write_ratio);

			if (do_write && is_workload_D) {
				// YCSB D "insert": write a brand-new key beyond the loaded dataset so
				// that the "latest" read distribution is actually biased toward recently
				// written keys.  Using sampleKey() here would overwrite existing keys,
				// making the workload equivalent to an update-biased-read benchmark
				// rather than a recent-insert-biased-read benchmark.
				++latest_insert_idx;
				std::string new_key = makeKey(latest_insert_idx);
				if (cfg.latency) {
					auto t0 = std::chrono::steady_clock::now();
					pending_opid = store.put(new_key, template_value);
					auto t1 = std::chrono::steady_clock::now();
					write_latencies_us.push_back(
						std::chrono::duration<double, std::micro>(t1 - t0).count());
				} else {
					pending_opid = store.put(new_key, template_value);
				}
				// Keep the store's scan key-space bound in sync so scan() and
			// sampleKey("latest") see the newly inserted key.
				store.setScanRecordCount(latest_insert_idx + 1);
				run_write_entries++;
				writes++;
				ops_since_sync++;
				i++;
			} else if (do_write) {
				uint64_t batch_end = std::min(
					i + static_cast<uint64_t>(cfg.batch_size), cfg.operation_count);
				std::vector<KeyValue> batch;
				batch.reserve(batch_end - i);
				for (uint64_t j = i; j < batch_end; j++) {
					uint64_t key_id = sampleKey(cfg, zipf_dist, rng, latest_insert_idx);
					batch.push_back({makeKey(key_id), template_value});
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
				uint64_t key_id = sampleKey(cfg, zipf_dist, rng, latest_insert_idx);
				std::string key = makeKey(key_id);
				if (cfg.latency) {
					auto t0 = std::chrono::steady_clock::now();
					store.get(key);
					auto t1 = std::chrono::steady_clock::now();
					read_latencies_us.push_back(
						std::chrono::duration<double, std::micro>(t1 - t0).count());
				} else {
					store.get(key);
				}
				reads++;
				ops_since_sync++;
				i++;
			}
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
	double total_ops = static_cast<double>(writes + reads + scans + rmws);
	double throughput = total_ops / run_sec;
	double write_throughput = static_cast<double>(writes + rmws) / run_sec;

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
	// Workload E inserts new keys beyond record_count, so store may be larger.
	if (!is_workload_E && cfg.record_count > 0 && final_store_size != cfg.record_count) {
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
	LOG(INFO) << "Ops: " << writes << " writes + " << reads << " reads + "
	          << scans << " scans + " << rmws << " rmws = "
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
		    << "value_size,workload,key_dist,zipf_theta,scan_len,"
		    << "batch_size,write_ratio,sync_interval,sync_barrier,"
		    << "runtime_sec,throughput_ops_sec,write_throughput_ops_sec,"
		    << "writes,reads,scans,rmws,store_size,published_entries,applied_entries,valid,"
		    << "pub_p50_us,pub_p95_us,pub_p99_us,pub_p999_us,"
		    << "apply_p50_us,apply_p95_us,apply_p99_us,apply_p999_us,"
		    << "read_p50_us,read_p99_us\n";
		csv << cfg.sequencer << "," << cfg.order << "," << cfg.ack << "," << cfg.rf
		    << "," << cfg.record_count << "," << cfg.operation_count
		    << "," << cfg.value_size
		    << "," << cfg.workload
		    << "," << cfg.key_dist
		    << "," << cfg.zipf_theta
		    << "," << cfg.scan_len
		    << "," << cfg.batch_size
		    << "," << cfg.write_ratio << "," << cfg.sync_interval
		    << "," << cfg.sync_barrier
		    << "," << std::fixed << std::setprecision(6) << run_sec
		    << "," << std::setprecision(0) << throughput
		    << "," << write_throughput
		    << "," << writes << "," << reads
		    << "," << scans << "," << rmws
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
		("rf", "Replication factor (0=no replication; RF>=1 requires a replica process to be running)",
		 cxxopts::value<int>()->default_value("0"))
		("record_count", "Records to pre-load",
		 cxxopts::value<uint64_t>()->default_value("1000000"))
		("operation_count", "Operations in run phase",
		 cxxopts::value<uint64_t>()->default_value("1000000"))
		("value_size", "Value size in bytes",
		 cxxopts::value<size_t>()->default_value("100"))
		("batch_size", "KV ops per log append",
		 cxxopts::value<int>()->default_value("1"))
		("write_ratio", "Fraction of writes (1.0=write-only, 0.5=50/50); overridden by --workload",
		 cxxopts::value<double>()->default_value("1.0"))
		("workload", "YCSB workload preset: A|B|C|D|E|F (overrides write_ratio and key_dist)",
		 cxxopts::value<std::string>()->default_value(""))
		("key_dist", "Key distribution: uniform|zipf|latest (workload presets set this)",
		 cxxopts::value<std::string>()->default_value("uniform"))
		("zipf_theta", "Zipf skew exponent (0=uniform, default 0.99)",
		 cxxopts::value<double>()->default_value("0.99"))
		("scan_len", "Keys per scan op (workload E, default 100)",
		 cxxopts::value<int>()->default_value("100"))
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
	cfg.workload = result["workload"].as<std::string>();
	cfg.key_dist = result["key_dist"].as<std::string>();
	cfg.zipf_theta = result["zipf_theta"].as<double>();
	cfg.scan_len = result["scan_len"].as<int>();
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

	// Apply YCSB preset overrides (must happen after all CLI parsing)
	applyWorkloadPreset(cfg);

	LOG(INFO) << "=== Shared-Log KV Benchmark ===";
	LOG(INFO) << cfg.sequencer << " order=" << cfg.order << " ack=" << cfg.ack
	          << " rf=" << cfg.rf;
	LOG(INFO) << "ops=" << cfg.operation_count << " batch=" << cfg.batch_size
	          << " write_ratio=" << cfg.write_ratio
	          << " workload=" << (cfg.workload.empty() ? "manual" : cfg.workload)
	          << " key_dist=" << cfg.key_dist
	          << " zipf_theta=" << cfg.zipf_theta
	          << " scan_len=" << cfg.scan_len
	          << " sync_interval=" << cfg.sync_interval
	          << " sync_barrier=" << cfg.sync_barrier
	          << " latency=" << cfg.latency;

	return runBenchmark(cfg) ? 0 : 1;
}
