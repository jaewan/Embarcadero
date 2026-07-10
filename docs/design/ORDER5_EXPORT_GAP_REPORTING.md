# ORDER=5 export-ring lap reporting (O5-1)

**Status:** implemented (main). Mechanism = EDIT A + EDIT B + EDIT C + PR-2.

## Problem
The ORDER=5 per-broker export uses a fixed-size ring of immutable descriptors
(`num_slots_ = BATCHHEADERS_SIZE / sizeof(BatchHeader)`, a batch *count*). A subscriber that falls
`>= num_slots_` committed batches behind is *lapped*: the descriptor slot its cursor points at has
been overwritten by a newer committed batch (`header->batch_seq > next_export`). The old code
silently advanced the cursor past the skipped batches, dropping `total_order` values from that
subscriber's delivered stream — which the client's own validator treats as the sacred total-order
violation. The publisher **linger** (which shrinks batches at low load, multiplying batch count)
makes a lap arrive after fewer *bytes* of consumer lag, so this had to be made safe, not left latent.

## Mechanism
- **EDIT A** (`topic.cc ShouldFatalOnOrder5ExportOverrun`): the overrun is FATAL by default in
  debug/CI (a lap is a total-order-completeness bug — fail loudly in tests), OFF in release; the
  `EMBARCADERO_ORDER5_EXPORT_OVERRUN_FATAL` env overrides either default.
- **EDIT B** (`topic.cc GetBatchToExportWithMetadata`, `+ topic_manager`): an optional `export_gap`
  out-param (pointer, defaults null → no caller ripple) reports the skipped-batch count on a lap;
  zeroed at function entry, set non-zero only in the overrun branch.
- **EDIT C** (`network_manager.cc` per-connection export loop): on `export_gap > 0`, the next batch
  frame for **that connection only** is flagged `BATCH_META_FLAG_EXPORT_GAP`. Keeping-up subscribers
  take the mutually-exclusive `have_ring` branch and never set the flag — their frames are
  byte-identical to before.
- **PR-2** (`wire_formats.h`, `subscriber.cc`): the flag reuses the previously-reserved
  `BatchMetadata.flags` bit 0 (wire-compatible — peers predating this ignore it). The subscriber
  accepts the flag (latency parser rejects only *unknown* bits) and **reports** it (`LOG(WARNING)` +
  `ordered_export_gaps_reported_` counter, exposed via `GetOrderedExportGapsReported()`), re-anchoring
  delivery to the batch's `batch_total_order`. What *is* delivered stays correctly ordered; the gap
  is a resubscribe-style discontinuity, reported — never a silent hole.

## Guarantees preserved
Total order (single GOI), per-session FIFO, and the non-coherent CXL discipline are untouched: only
the lagging connection's `last_offset` is mutated (no shared/global cursor), and the flag rides that
connection's own next frame. Reviewed and verified (keeping-up subscriber provably byte-for-byte
unaffected).

## Known limitation (send-failure report loss)
`pending_export_gap` is a per-connection-thread local, cleared when the flag is stamped onto the frame
*before* the metadata/payload send. If that send fails and the connection tears down, this single lap
goes **unreported**: the thread (and its `pending_export_gap`) dies, and the client's reconnect starts
a fresh `SubscriberState` at the live window with no flag. This does **not** violate any ordering
guarantee — it only drops the *report* of one lap, and only when it coincides with a socket teardown,
which is itself a client-observable resubscribe (so not truly silent). A robust fix would persist the
gap across the reconnect keyed on client identity; deferred as low-value since the teardown already
signals the client. See the `[[O5-1 KNOWN LIMITATION]]` comment in `network_manager.cc`.

## Test gaps (follow-up)
The correctness of the read-seam siblings (CXL-1 torn GOI recovery read, CXL-2 stale replicated
frontier) and this lap path are build- and ORDER=5-regression-verified, but the *torn-read* /
*forced-lap* behavior is not yet unit-tested (a coherent testbed cannot exercise a non-coherent torn
read). Add `test/cv_replicated_offset_torn_read_test.cc` and `test/order5_export_overrun_test.cc`
(forced lap via a tiny `EMBARCADERO_BATCH_HEADERS_SIZE`, `EXPECT_DEATH` under debug + `export_gap>0`
+ client `GetOrderedExportGapsReported()>0` on the wire path).
