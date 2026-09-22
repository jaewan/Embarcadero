#include "topic.h"
#include "topic_order5_internal.h"
#include "common/ack_rf_policy.h"
#include "common/performance_utils.h"
#include "common/stage_trace.h"
#include "common/wire_formats.h"
#include "common/order_level.h"
#include "common/env_flags.h"
#include "common/fault_injection.h"
#include "order5_tr_trace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace Embarcadero {
using namespace topic_internal;

// Subscriber export and reconstruction of ORDER5 export descriptors.
// State remains owned by Topic; this file adds no independent runtime state.

bool Topic::GetBatchToExport(
		size_t &expected_batch_offset,
		void* &batch_addr,
		size_t &batch_size) {
	if (num_slots_ == 0) return false;
	BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(start_batch_header) + BATCHHEADERS_SIZE);
	// [[RING_WRAP]] expected_batch_offset can grow without bound; wrap to slot index
	size_t slot_index = expected_batch_offset % num_slots_;
	BatchHeader* header = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * slot_index);
	// [[CXL_NON_COHERENT]] Invalidate before reading ordered so export sees sequencer writes
	CXL::flush_cacheline(header);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(header) + 64);
	CXL::full_fence();  // MFENCE required: CLFLUSHOPT only ordered by MFENCE (Intel SDM §8.2.5)
	if (header->ordered == 0) {
		return false;
	}
	size_t off = header->batch_off_to_export;
	// [[BOUNDS]] Validate batch_off_to_export (matches disk_manager.cc pattern)
	if (off >= BATCHHEADERS_SIZE || off % sizeof(BatchHeader) != 0) {
		LOG(WARNING) << "[GetBatchToExport B" << broker_id_ << "] Invalid batch_off_to_export=" << off;
		expected_batch_offset++;
		return false;
	}
	BatchHeader* actual = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header) + off);
	if (actual < start_batch_header || actual >= ring_end) {
		LOG(WARNING) << "[GetBatchToExport B" << broker_id_ << "] Export chain outside ring (off=" << off << ")";
		expected_batch_offset++;
		return false;
	}
	// [[CXL_NON_COHERENT]] Invalidate actual batch so we see sequencer-written total_size/log_idx
	CXL::flush_cacheline(actual);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(actual) + 64);
	CXL::full_fence();  // MFENCE required for CLFLUSHOPT ordering
	header = actual;

	// Scalog ORDER=1 export must never expose a batch beyond the ordered frontier used by ACK1.
	// Keep export/ACK visibility contracts identical by refusing batches that violate this invariant.
	if (seq_type_ == SCALOG && order_ == kOrderPerBroker) {
		volatile uint64_t* ordered_ptr = &tinode_->offsets[broker_id_].ordered;
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(ordered_ptr)));
		CXL::load_fence();
		const uint64_t ordered_frontier = *ordered_ptr;
		const uint64_t batch_end_logical =
			static_cast<uint64_t>(header->start_logical_offset) + static_cast<uint64_t>(header->num_msg);
		if (batch_end_logical > ordered_frontier) {
			LOG(WARNING) << "[Scalog Export Guard B" << broker_id_
			             << "] blocking batch beyond ordered frontier"
			             << " start=" << header->start_logical_offset
			             << " num_msg=" << header->num_msg
			             << " batch_end=" << batch_end_logical
			             << " ordered=" << ordered_frontier;
			return false;
		}
	}

	batch_size = header->total_size;
	batch_addr = header->log_idx + reinterpret_cast<uint8_t*>(cxl_addr_);
	expected_batch_offset++;

	return true;
}

bool Topic::GetBatchToExportWithMetadata(
		size_t &expected_batch_offset,
		void* &batch_addr,
		size_t &batch_size,
		size_t &batch_total_order,
		uint32_t &num_messages,
		uint64_t* export_gap) {
	// [[O5-1 EDIT B]] export_gap (optional out): number of committed export batches SKIPPED because
	// a lagging subscriber lapped the export ring on this call (0 on the common path). The caller
	// (network layer) uses a non-zero value to terminalize+flag that connection so the gap is
	// reported to the client, never silently dropped. Pointer-default keeps all other callers intact.
	if (export_gap) *export_gap = 0;
	// Corfu ORDER=2 does not advance CompletionVector like ORDER=5/3 paths.
	// Fall back to legacy ordered-slot export using monotonic expected_batch_offset.
	if (seq_type_ == CORFU && order_ == 2 && num_slots_ > 0 && cxl_addr_) {
		BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
		size_t slot = expected_batch_offset % num_slots_;
		BatchHeader* header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * slot);
		CXL::flush_cacheline(header);
		CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(header) + 64);
		CXL::full_fence();
		if (header->pbr_absolute_index != expected_batch_offset) {
			if (header->pbr_absolute_index > expected_batch_offset) {
				// Gap: later batch arrived, wait for missing sequence.
				return false;
			}
			LOG(ERROR) << "Corfu ORDER=2 export freshness violation: slot=" << slot
			           << " expected_seq=" << expected_batch_offset
			           << " observed_seq=" << header->pbr_absolute_index;
			return false;
		}
		if (header->ordered == 0 || header->num_msg == 0 || header->total_size == 0) {
			return false;
		}
		batch_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + header->log_idx;
		batch_size = header->total_size;
		batch_total_order = header->total_order;
		num_messages = header->num_msg;
		expected_batch_offset++;
		return true;
	}
	// LazyLog ORDER=2 currently exports from the ordered slot ring and does not
	// drive CompletionVector progress. Reuse legacy slot export and derive
	// metadata directly from the batch payload.
	if (seq_type_ == LAZYLOG && order_ == Embarcadero::kOrderTotal) {
		if (!GetBatchToExport(expected_batch_offset, batch_addr, batch_size)) {
			return false;
		}
		if (batch_addr == nullptr || batch_size < sizeof(MessageHeader)) {
			return false;
		}

		auto* first = reinterpret_cast<MessageHeader*>(batch_addr);
		batch_total_order = first->total_order;

		uint32_t counted = 0;
		size_t remaining = batch_size;
		uint8_t* cursor = reinterpret_cast<uint8_t*>(batch_addr);
		while (remaining >= sizeof(MessageHeader)) {
			auto* hdr = reinterpret_cast<MessageHeader*>(cursor);
			const size_t stride = static_cast<size_t>(hdr->next_msg_diff);
			if (stride == 0 || stride > remaining) {
				break;
			}
			++counted;
			cursor += stride;
			remaining -= stride;
		}
		num_messages = (counted > 0) ? counted : 1;
		return true;
	}
	// Scalog ORDER=1 (per-broker) and ORDER=2 (total): export from the ordered slot ring.
	// Use BatchHeader::num_msg as the authoritative batch count; re-walking the payload via
	// next_msg_diff can undercount a partially visible batch and desync the ordered subscriber
	// stream mid-payload.
	if (seq_type_ == SCALOG && (order_ == Embarcadero::kOrderTotal ||
	                          order_ == Embarcadero::kOrderPerBroker)) {
		if (num_slots_ == 0) {
			return false;
		}
		BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
		BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(start_batch_header) + BATCHHEADERS_SIZE);
		size_t slot_index = expected_batch_offset % num_slots_;
		BatchHeader* header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * slot_index);
		CXL::flush_cacheline(header);
		CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(header) + 64);
		CXL::full_fence();
		if (header->ordered == 0) {
			return false;
		}
		size_t off = header->batch_off_to_export;
		if (off >= BATCHHEADERS_SIZE || off % sizeof(BatchHeader) != 0) {
			LOG(WARNING) << "[Scalog GetBatchToExportWithMetadata B" << broker_id_
			             << "] Invalid batch_off_to_export=" << off;
			expected_batch_offset++;
			return false;
		}
		BatchHeader* actual = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(header) + off);
		if (actual < start_batch_header || actual >= ring_end) {
			LOG(WARNING) << "[Scalog GetBatchToExportWithMetadata B" << broker_id_
			             << "] Export chain outside ring (off=" << off << ")";
			expected_batch_offset++;
			return false;
		}
		CXL::flush_cacheline(actual);
		CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(actual) + 64);
		CXL::full_fence();

		batch_addr = actual->log_idx + reinterpret_cast<uint8_t*>(cxl_addr_);
		batch_size = actual->total_size;
		num_messages = actual->num_msg;
		expected_batch_offset++;
		if (batch_addr == nullptr || batch_size < sizeof(MessageHeader) || num_messages == 0) {
			return false;
		}

		auto* first = reinterpret_cast<MessageHeader*>(batch_addr);
		batch_total_order = first->total_order;
		return true;
	}

	// [[ORDER5_GOI_SOURCED_EXPORT 2026-07-23]] Deliver from the GOI — the single
	// dense total order — per the design (§Read Path: "a subscriber tails
	// committed_seq, scans the GOI"). The prior implementation shipped a per-broker
	// compacted export-descriptor ring that CommitEpoch wrote into the SAME
	// BatchHeader region the producer PBR publishes into, via a cursor independent
	// of the producer's. Under bursty hold-buffer commits a committed batch's export
	// descriptor could fail to land in its ring slot while committed_seq (the GOI
	// frontier) advanced past it, permanently stranding that batch from the
	// subscriber — a silent, un-flagged delivery gap that hung the ordered-consume
	// path (root-caused 2026-07-23 in the paper's own regime: committed_seq=603 while
	// a committed batch's export descriptor was never written; client stalled forever
	// one 64-msg batch short). The GOI is single-writer (sequencer only), monotonic,
	// and gated by committed_seq plus the global_seq==index readiness token (I3), so
	// it cannot drop, tear, or reorder a committed entry. Each broker connection ships
	// the entries it owns (broker_id==broker_id_); the client merges the per-broker
	// subsequences by total_order exactly as before, but the inputs are now complete
	// by construction. expected_batch_offset is repurposed as this connection's
	// GOI-index cursor (was the export-descriptor sequence).
	const bool trace_order5 = ShouldEnableOrder5Trace() && (order_ == 5);
	if (export_gap) *export_gap = 0;  // GOI is dense: no ring-lap gap is possible.
	if (cxl_addr_ == nullptr) return false;

	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::invalidate_cacheline_for_read(control_block);
	CXL::load_fence();
	const uint64_t committed_seq = control_block->committed_seq.load(std::memory_order_acquire);
	if (committed_seq == UINT64_MAX) return false;  // nothing committed yet

	GOIEntry* goi = reinterpret_cast<GOIEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kGOIOffset);
	size_t cursor = expected_batch_offset;
	while (cursor <= committed_seq) {
		GOIEntry* entry = &goi[cursor];
		ReadGOIEntryFresh(entry);
		// I3 readiness: below committed_seq the sequencer has already flushed the
		// entry, so global_seq==cursor holds; a mismatch is a torn/transient read and
		// is retried (never delivered), not skipped.
		if (entry->global_seq != cursor) break;
		// Ship only the entries this broker owns; peers' connections ship theirs.
		if (static_cast<int>(entry->broker_id) != broker_id_) { ++cursor; continue; }
		// Empty/skip GOI entries (aged-out or zero-count) carry no payload.
		if (entry->message_count == 0 || entry->payload_size == 0) { ++cursor; continue; }
		const size_t log_idx = entry->blog_offset;
		const size_t total_size = entry->payload_size;
		if (log_idx >= CXL_SIZE || total_size > (CXL_SIZE - log_idx)) {
			LOG_EVERY_N(ERROR, 1000) << "[ORDER5_GOI_EXPORT B" << broker_id_
				<< "] out-of-bounds payload at goi=" << cursor
				<< " blog_offset=" << log_idx << " size=" << total_size << "; skipping";
			++cursor;
			continue;
		}
		batch_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + log_idx;
		batch_size = total_size;
		batch_total_order = entry->total_order;
		num_messages = entry->message_count;
		expected_batch_offset = cursor + 1;
		if (trace_order5) {
			LOG(INFO) << "[ORDER5_TRACE_EXPORT_BATCH B" << broker_id_ << "]"
			          << " source=goi goi_index=" << cursor
			          << " total_order=" << batch_total_order
			          << " num_messages=" << num_messages
			          << " batch_size=" << batch_size
			          << " committed_seq=" << committed_seq;
		}
		return true;
	}
	// No owned, ready entry at/below committed_seq this call. Persist how far we
	// scanned so committed non-owned/empty entries are not re-examined next call.
	expected_batch_offset = cursor;
	return false;
}

void Topic::DumpOrder5ExportStall(size_t next_export) {
	if (order_ != 5 || cxl_addr_ == nullptr || num_slots_ == 0) return;

	// What the reader is looking at: the export-ring slot for next_export.
	BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
	const size_t slot = next_export % num_slots_;
	BatchHeader* h = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(ring_start) + slot * sizeof(BatchHeader));
	CXL::flush_cacheline(h);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(h) + 64);
	CXL::load_fence();

	// What the sequencer has actually written: the per-broker export-write frontier
	// (how many export descriptors CommitEpoch has emitted for this broker).
	uint64_t export_written;
	{
		absl::MutexLock lk(&export_cursor_mu_);
		export_written = export_sequence_by_broker_[broker_id_];
	}

	ControlBlock* cb = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::invalidate_cacheline_for_read(cb);
	CXL::load_fence();
	const uint64_t committed_seq = cb->committed_seq.load(std::memory_order_acquire);

	LOG(ERROR) << "[ORDER5_EXPORT_STALL] broker=" << broker_id_
	           << " reader_next_export=" << next_export
	           << " seq_export_written=" << export_written
	           << " (written>reader? " << (export_written > next_export ? "YES-ring/deliver-side"
	                                                                      : "NO-sequencer-side") << ")"
	           << " committed_seq=" << committed_seq
	           << " slot=" << slot
	           << " slot.batch_seq=" << h->batch_seq
	           << " slot.ordered=" << h->ordered
	           << " slot.num_msg=" << h->num_msg
	           << " slot.total_order=" << h->total_order
	           << " slot.flags=0x" << std::hex << h->flags << std::dec
	           << " slot.pbr_abs=" << h->pbr_absolute_index
	           << " (slot.batch_seq==reader_next_export? "
	           << (h->batch_seq == next_export ? "YES-present-but-gated" : "NO-overwritten/diverged")
	           << ")";
}

void Topic::InitExportCursorForBroker(int broker_id) {
	if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS) return;
	BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
	absl::MutexLock lock(&export_cursor_mu_);
	export_cursor_by_broker_[broker_id] = ring_start;
	export_sequence_by_broker_[broker_id] = 0;
}

void Topic::RebuildOrder5ExportDescriptorsFromGOI(uint64_t recovered_next_goi) {
	if (recovered_next_goi == 0 || cxl_addr_ == nullptr || tinode_ == nullptr) return;

	GOIEntry* goi = reinterpret_cast<GOIEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kGOIOffset);
	uint64_t replayed = 0;
	for (uint64_t i = 0; i < recovered_next_goi; ++i) {
		GOIEntry* entry = &goi[i];
		CXL::invalidate_cacheline_for_read(entry);
		CXL::invalidate_cacheline_for_read(reinterpret_cast<const uint8_t*>(entry) + 64);
		CXL::load_fence();
		if (entry->global_seq != i) continue;

		const int b = static_cast<int>(entry->broker_id);
		if (b < 0 || b >= NUM_MAX_BROKERS || entry->message_count == 0 || entry->payload_size == 0) {
			continue;
		}
		const size_t batch_headers_offset = tinode_->offsets[b].batch_headers_offset;
		if (batch_headers_offset == 0) continue;

		BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cxl_addr_) + batch_headers_offset);
		BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(ring_start) + BATCHHEADERS_SIZE);
		BatchHeader* cur = export_cursor_by_broker_[b];
		if (cur == nullptr || cur < ring_start || cur >= ring_end) {
			cur = ring_start;
			export_cursor_by_broker_[b] = cur;
		}

		const uint64_t export_seq = export_sequence_by_broker_[b]++;
		cur->batch_seq = export_seq;
		cur->client_id = static_cast<uint32_t>(entry->client_id);
		cur->broker_id = static_cast<uint32_t>(b);
		cur->epoch_created = entry->epoch_sequenced;
		cur->session_epoch = static_cast<uint16_t>(entry->session_epoch & 0xFFFFu);
		cur->session_epoch32 = entry->session_epoch;
		cur->batch_id = entry->batch_id;
		cur->pbr_absolute_index = entry->pbr_index;
		cur->start_logical_offset =
			(entry->cumulative_message_count >= entry->message_count)
				? (entry->cumulative_message_count - entry->message_count)
				: 0;
		cur->batch_off_to_export = 0;
		cur->log_idx = entry->blog_offset;
		cur->total_size = entry->payload_size;
		cur->num_msg = entry->message_count;
		cur->total_order = entry->total_order;
		cur->ordered = 1;
		cur->batch_complete = 0;
		cur->publish_commit = kBatchHeaderPublishUncommitted;
		cur->flags = kBatchHeaderFlagRetired;
		CXL::store_fence();
		CXL::flush_cacheline(cur);
		CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(cur) + 64);
		CXL::store_fence();

		BatchHeader* next_cursor = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(cur) + sizeof(BatchHeader));
		if (next_cursor >= ring_end) next_cursor = ring_start;
		export_cursor_by_broker_[b] = next_cursor;
		++replayed;
	}
	LOG(INFO) << "RebuildOrder5ExportDescriptorsFromGOI: recovered_next_goi="
	          << recovered_next_goi << " replayed_descriptors=" << replayed;
}

}  // namespace Embarcadero
