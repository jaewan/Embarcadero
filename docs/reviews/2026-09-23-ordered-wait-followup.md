# Ordered-consumer wait follow-up — September 23, 2026

The earlier whole-stack DRAM comparison reduced client memory and page faults, but measured more user CPU than the previous refactored version. That difference did not identify a single cause: the newer client audits concurrently with publishing, so publisher, receiver and audit work overlap. The measured ACK and audit phases vary substantially across otherwise valid trials.

## Code-path investigation and change

`Subscriber::ConsumeOrderedBatch()` used a 50 µs condition-variable timeout even when no contiguous ordered message was ready. The streaming audit repeatedly calls this method during publication. `StageOrderedMessages()` publishes a ready contiguous message and signals the consumer while holding `consume_mutex_`, so normal message arrival does not require a periodic 50 µs check. The consumer now waits up to its caller deadline with a **20 ms default fallback**. This fallback bounds shutdown and retention rechecks. The audit observes cancellation and parser/export failures at its own 20 ms consume-call boundary; other callers use their own deadlines. `EMBARCADERO_CONSUME_ORDERED_WAIT_US` remains an explicit override. The single-message `ConsumeOrdered()` path now caps each wait to its remaining caller deadline, fixing a possible timeout overshoot with a long override.

The implementation is limited to `src/client/subscriber.cc`; source5 changes no subscriber storage layout, descriptor accounting, wire protocol, broker, or publisher code. A new test checks that a 2 ms single-message call returns promptly under a 1 s configured wait and that asynchronously published parser data wakes a long batch wait. The full native Release build and all 53 CTests pass. Source5's subscriber fixture passes 24 tests under ASan/UBSan and TSan; its publisher fixture passes 15 under each sanitizer. The independent source review found no data lost-wakeup for normal publication and no new ownership or cap issue. The known unsynchronized terminal-signal window is bounded by the 20 ms default fallback and by the audit's 20 ms consume quantum; an explicit larger override can enlarge that bound.

Before this change, we tested batching the returned-descriptor recycler to remove roughly 522,000 shared pool lock acquisitions in a 2 GiB transfer. Its fixed one-broker comparison gave audited throughput ratio 0.958 with exploratory 95% interval 0.840–1.092 and no measurable client CPU benefit. The code was removed; that failed optimization hypothesis and its ten successful diagnostic trials remain under `bulk-recycle-n1-abba/`. Per-message pool locking remains a candidate for future profiling, not a demonstrated cause of the earlier CPU difference.

## Controlled measurement

Source4 is the frozen client before the wait change, SHA256 `d4c68cc3ba250fd9df19e100a9aa1a64823275b6a62a96f8d803e0d746f55098`. Source5 is the final wait-change snapshot, SHA256 `4a39f3a1dbdd5213fca2b6cce77f01d45d610f4ca5bf3c736569f9c84d9586a5`. Both complete clients ran against the **same source4 broker binary** on the local DRAM emulator. Both used streaming exact indexed audit, a 256 MiB subscriber byte limit, 64 GiB fresh shared-memory mappings, a 2 GiB transfer, 4 KiB messages, ORDER5/ACK1/RF0, client NUMA node 0 and broker node 1. Two qualifications were excluded and four balanced AB/BA pairs were fixed before the run. Every trial passed exact delivery, authoritative ACK, routing and normal-cleanup checks; no trial was replaced.

| Client metric | Source5 / source4 paired geometric ratio | Exploratory 95% interval |
|---|---:|---:|
| Audited end-to-end throughput, primary | 1.062 | 0.874–1.291 |
| ACK-completion throughput | 1.120 | 0.774–1.621 |
| User CPU seconds | 0.862 | 0.574–1.293 |
| System CPU seconds | 0.883 | 0.648–1.204 |
| Total CPU seconds | 0.882 | 0.749–1.040 |
| Minor page faults | 0.976 | 0.956–0.996 |
| Peak RSS | 1.005 | 1.000–1.011 |

The code removes an avoidable high-frequency idle wait. In this small whole-client comparison, throughput and CPU point estimates favor source5, but their intervals do **not** establish improvement or 5% throughput nonregression. RSS is effectively unchanged. Page faults are slightly lower in these exploratory runs; the study did not correct for multiple endpoints. ACK and remaining audit time still trade off across trials. We did not capture timed-wait counts or per-thread CPU, so the measured CPU difference cannot be assigned quantitatively to the wait change.

The source5 client was built from the workspace. Its full archived inventory matched that workspace at launch and Ninja reported no pending client build work. Independent link-input review found matching compiler and ISA contracts and external dependency bytes; only Subscriber has changed executable sections. Other object differences include embedded source-path strings, which can change link layout and remain a small comparison limitation. This is one local broker and a finite DRAM transfer; it says nothing about physical CXL, remote clients, replication or sustained capacity.

Evidence: `results/refactor-optimization/2026-09-23/ordered-wait-n1-abba/` (all ten manifests, logs, CSVs and independent analysis), `ordered-wait-link-input-audit.json`, `client-sanitizers-source5/`, `source5-full-build.log`, `source5-ctest.log`, `source5-ordered-wait-independent-review.json`, and `source-archives/`. The earlier whole-stack source14-to-source4 results remain separate.
