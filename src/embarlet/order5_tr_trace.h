// order5_tr_trace.h — disabled-by-default instrumentation to separate the
// ORDER=5 *observation* period (P, PBR scanner pass) from the *release/commit*
// period (tau, epoch seal), and to time the gap hold/release lifecycle.
//
// Motivation (paper Sec2/Sec4 "T/R" contradiction): a held suffix is released
// only through sealed-epoch processing, so the number of release/commit
// opportunities during a skew window T is T/tau, NOT T/P. This tracer measures
// P, tau, and the per-gap {detect, release, seals-during-gap, passes-during-gap,
// hold occupancy} so the claim can be replaced with measured distributions.
//
// Design: OFF unless EMBAR_ORDER5_TR_TRACE=1. When on, each producing thread
// writes fixed-size records to its OWN file (<EMBAR_ORDER5_TR_TRACE_CSV>.pid<PID>.t<id>),
// buffered and flushed every kFlushEvery records + at thread exit. No locks on
// the hot path, no shared buffer, no data race, and SIGKILL-robust (loses at most
// kFlushEvery records/thread) — so it does not depend on graceful broker shutdown.
// Never writes to stdout or the paper CSV. When off, every Record* call is a
// single relaxed atomic-bool load and return.
//
// CSV columns (one schema for all rows; unused fields are 0):
//   type,broker,steady_ns,session_or_epoch,seq_field,epoch_index,scan_pass_total,hold_or_nummsg
//   seal        : session_or_epoch=epoch_index; epoch_index=epoch_index
//   scan_pass   : session_or_epoch=pass_index;  scan_pass_total=cumulative passes
//   gap_detect  : session_or_epoch=session_key; seq_field=missing_seq;  epoch_index,scan_pass_total,hold_or_nummsg=hold_occupancy
//   gap_release : session_or_epoch=session_key; seq_field=next_expected; epoch_index,scan_pass_total,hold_or_nummsg=hold_occupancy
//   commit      : session_or_epoch=session_key; seq_field=committed_hwm; hold_or_nummsg=num_msg
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace Embarcadero {

class Order5TrTrace {
 public:
	enum Type : uint8_t { kSeal = 0, kScanPass = 1, kGapDetect = 2, kGapRelease = 3, kCommit = 4 };
	struct Rec {
		uint64_t ns, s, seq, epoch, pass, hold;
		uint32_t broker;
		uint8_t type;
	};
	static constexpr size_t kFlushEvery = 2048;

	static Order5TrTrace& Instance() { static Order5TrTrace inst; return inst; }
	bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
	uint64_t ScanPassTotal() const { return scan_pass_total_.load(std::memory_order_relaxed); }
	const std::string& base_path() const { return base_; }
	uint32_t NextThreadId() { return next_tid_.fetch_add(1, std::memory_order_relaxed); }

	struct ThreadBuf {
		FILE* f = nullptr;
		std::vector<Rec> recs;
		ThreadBuf() { recs.reserve(kFlushEvery + 64); }
		~ThreadBuf() { flush(); if (f) std::fclose(f); }
		void open_if_needed() {
			if (f) return;
			Order5TrTrace& tr = Order5TrTrace::Instance();
			std::string p = tr.base_path() + ".pid" + std::to_string((long)getpid()) +
				".t" + std::to_string((long)tr.NextThreadId());
			f = std::fopen(p.c_str(), "w");
			if (f) std::fprintf(f,
				"type,broker,steady_ns,session_or_epoch,seq_field,epoch_index,scan_pass_total,hold_or_nummsg\n");
		}
		void flush() {
			if (recs.empty()) return;
			open_if_needed();
			if (f) {
				for (const Rec& r : recs) {
					const char* t = r.type == kSeal ? "seal" : r.type == kScanPass ? "scan_pass" :
						r.type == kGapDetect ? "gap_detect" : r.type == kGapRelease ? "gap_release" : "commit";
					std::fprintf(f, "%s,%u,%llu,%llu,%llu,%llu,%llu,%llu\n", t, r.broker,
						(unsigned long long)r.ns, (unsigned long long)r.s, (unsigned long long)r.seq,
						(unsigned long long)r.epoch, (unsigned long long)r.pass, (unsigned long long)r.hold);
				}
				std::fflush(f);
			}
			recs.clear();
		}
		inline void push(const Rec& r) { recs.push_back(r); if (recs.size() >= kFlushEvery) flush(); }
	};
	static ThreadBuf& TLS() { thread_local ThreadBuf buf; return buf; }

	inline void RecordSeal(uint64_t epoch_index, uint64_t ns) {
		if (!enabled()) return;
		TLS().push(Rec{ns, epoch_index, 0, epoch_index, 0, 0, 0, kSeal});
	}
	inline void RecordScanPass(uint32_t broker, uint64_t pass_index, uint64_t ns) {
		if (!enabled()) return;
		uint64_t total = scan_pass_total_.fetch_add(1, std::memory_order_relaxed) + 1;
		TLS().push(Rec{ns, pass_index, 0, 0, total, 0, broker, kScanPass});
	}
	inline void RecordGapDetect(uint32_t broker, uint64_t session_key, uint64_t ns,
			uint64_t missing_seq, uint64_t epoch_index, uint64_t scan_pass_total, uint64_t hold_occ) {
		if (!enabled()) return;
		TLS().push(Rec{ns, session_key, missing_seq, epoch_index, scan_pass_total, hold_occ, broker, kGapDetect});
	}
	inline void RecordGapRelease(uint32_t broker, uint64_t session_key, uint64_t ns,
			uint64_t next_expected, uint64_t epoch_index, uint64_t scan_pass_total, uint64_t hold_occ) {
		if (!enabled()) return;
		TLS().push(Rec{ns, session_key, next_expected, epoch_index, scan_pass_total, hold_occ, broker, kGapRelease});
	}
	inline void RecordCommit(uint32_t broker, uint64_t session_key, uint64_t ns,
			uint64_t committed_hwm, uint64_t num_msg) {
		if (!enabled()) return;
		TLS().push(Rec{ns, session_key, committed_hwm, 0, 0, num_msg, broker, kCommit});
	}
	// Best-effort flush of the calling thread's buffer at graceful shutdown.
	void Flush() { if (enabled()) TLS().flush(); }

 private:
	Order5TrTrace() {
		const char* on = std::getenv("EMBAR_ORDER5_TR_TRACE");
		enabled_.store(on && on[0] == '1', std::memory_order_relaxed);
		const char* p = std::getenv("EMBAR_ORDER5_TR_TRACE_CSV");
		base_ = (p && p[0]) ? p : "order5_tr_trace.csv";
	}
	std::atomic<bool> enabled_{false};
	std::atomic<uint64_t> scan_pass_total_{0};
	std::atomic<uint32_t> next_tid_{0};
	std::string base_;
};

}  // namespace Embarcadero
