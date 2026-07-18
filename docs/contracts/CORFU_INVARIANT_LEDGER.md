# Corfu Baseline Invariant Ledger

**Created:** 2026-07-18
**Scope:** CXL-Corfu baseline hardening (client token path, coordinator transport, evaluation).
**Status legend:** `OPEN` (not yet enforced) / `ENFORCED-STATIC` (code enforces and isolated tests pass, publication-cluster validation pending) / `VALIDATED` (publication-cluster validation passed).

> NOTE (2026-07-18): `throughput_test` compiles and the isolated
> `corfu_ordered_token_gate_test`, `corfu_sequencer_fifo_smoke`,
> `corfu_token_proxy_smoke`, and `corfu_ordered_chain_smoke` tests pass. No
> cluster process was started or stopped while the concurrent experiment was
> running. Publication-cluster validation remains pending.

## Invariants

### C1 — Token-before-write
No Corfu payload byte may be sent to any broker before the corresponding token
grant for that batch has been received by the client.

- Ground truth: Corfu (NSDI'12) §3: clients first obtain a token (log position)
  from the sequencer, then write to the storage units at that position.
- Evidence baseline: `src/client/publisher.cc` PublishThread Corfu branch —
  `GetTotalOrder()` precedes `SendBatchData()`; `corfu_payload_before_grant_`
  counter + throw guard before payload send.
- Status: see ledger table below.

### C2 — Per-session token acquisition order
For one client session, token acquisition follows client submission/seal order
across ALL brokers (per-client FIFO), not just per-broker order.

- Evidence baseline: seal-time global ticket (`batch_seq` assigned at seal in
  `queue_buffer.cc`), `CorfuOrderedTokenGate` in `publisher.cc`.

### C3 — Unique, monotonic token ranges
Each successful append receives a unique, monotonically ordered token range
sufficient for all messages in its batch (range = num_msg entries, or
byte-range depending on address space definition).

- Evidence baseline: `corfu_sequencer_service.cc` / `corfu_mailbox_sequencer.cc`
  `fetch_add`-style allocation; `corfu_global_sequencer.cc`.

### C4 — No conflicting allocation on retry/duplicate
A retry or duplicate token request cannot allocate a conflicting token range or
perform a conflicting WriteOnce. (Duplicate grant of a *new* range to the same
logical batch = hole + duplicate data = violation.)

### C5 — Failure cannot silently break FIFO
RPC failure, cancellation, and shutdown cannot silently allow successor batches
to acquire tokens/send payloads as if the failed predecessor had succeeded.
Fail closed: successors must not proceed past a failed predecessor unless the
publisher is entering a terminal state and all waiters are reliably cancelled.

### C6 — No permanently missing ticket deadlock
Failure handling cannot leave a permanently missing ticket that deadlocks the
publisher (all later ticket-holders waiting forever on a ticket that will never
be published).

### C7 — Shutdown wakes all waiters, no spin loops
Shutdown wakes all blocked token requesters and terminates without unbounded
spin loops. Waiting must be on a condition variable (or equivalent), not
`std::this_thread::yield()` spinning.

### C8 — Retransmission preserves token/value identity
The current Corfu publisher has no payload retransmission path: an ambiguous or
failed append terminally aborts. If retransmission is added, it must carry the
SAME token (log position) and SAME payload bytes (WriteOnce identity).

### C9 — Protocol cost vs transport RTT separation
The implementation distinguishes protocol ordering cost from the selected
transport's RTT: the upstream coordinator transport is a named, configurable
dimension (grpc / mailbox(cxl)), and token-stage latency is independently
measurable and controllable through a separately recorded delay knob.

### C10 — Result provenance
Every paper-visible Corfu mode records enough metadata to prove which
transport, token policy, RF, ACK contract, and batching policy were used
(recorded per result row / run manifest).

## Ledger

| # | Finding | Invariant | Evidence (file:line) | Planned change | Static review | Dynamic validation |
|---|---------|-----------|----------------------|----------------|---------------|--------------------|
| L1 | Spin gate could hang during shutdown | C7, C6 | `corfu_ordered_token_gate.h`; gate test | Explicit CV wait with bounded shutdown observation and wake-all abort | ENFORCED-STATIC | isolated shutdown/wakeup PASS; cluster PENDING |
| L2 | Failure advanced the old gate and admitted successors | C5, C2 | `publisher.cc` Corfu gate wrappers; gate test | Terminal abort; completion linearizes against abort; late grants are burned | ENFORCED-STATIC | isolated abort-during-holder PASS; cluster PENDING |
| L3 | Payload-before-grant evidence was not a run-validity condition | C1 | `publisher.cc` token phase; `run_multiclient.sh` validator | Hard throw plus strict counter validation | ENFORCED-STATIC | parser sample PASS; cluster PENDING |
| L4 | Global ticket ordering and per-broker sequence assignment lacked an explicit boundary | C2, C3 | `publisher.cc` Corfu branch | Assign per-broker sequence and issue RPC while unique global turn is held | ENFORCED-STATIC | ordered gate PASS; cluster PENDING |
| L5 | Token/publish failure did not wake other publisher threads | C6, C7 | `CorfuAbortGate`; publish exit guard; destructor | Publisher-level terminal abort wakes all gate waiters | ENFORCED-STATIC | isolated abort/shutdown PASS; cluster PENDING |
| L6 | Coordinator transport and latency sensitivity were not explicit dimensions | C9, C10 | baseline transport config; token-delay code; sensitivity script | Select `grpc|cxl_mailbox`; record and sweep post-grant token-stage delay | ENFORCED-STATIC | component smokes PASS; publication sweep PENDING |
| L7 | Evaluation omitted token policy/counters from provenance | C10 | `run_multiclient.sh` run contract and `corfu_token_phase.csv` | Record gate policy, transport, delay, RF/ACK/batch, commit, and invariant counters | ENFORCED-STATIC | shell syntax/parser PASS; cluster PENDING |
| L8 | Ambiguous RPC completion could double-allocate on retry | C4, C8 | `corfu_client.h`; `corfu_token_proxy.cc` | Stable logical request identity plus proxy single-flight/cache; old requests reject after eviction | ENFORCED-STATIC | proxy smoke PASS; failure injection PENDING |
| L9 | Grant followed by failure creates a token hole | C3, C4 | `publisher.cc` grant/completion path | Count and burn issued range; terminally abort; never reroute/reuse | ENFORCED-STATIC | isolated late-grant PASS; cluster PENDING |
| L10 | Paper could attribute gRPC transport cost to the Corfu protocol | C9 | `Paper/Text/Sec7_Evaluation.tex` | Explicitly labels measured gap as Corfu plus gRPC and cites original RDMA cost | ENFORCED-STATIC | paper review PENDING |

The client-visible publisher-to-ingress hop remains gRPC in both transport
modes. `control_transport=cxl_mailbox` selects only the ingress-to-global-
sequencer hop. `EMBARCADERO_CORFU_TOKEN_DELAY_US` is a post-grant critical-path
delay while the ordered turn is held; it is a sensitivity mechanism, not a
network-loss, timeout, or queueing emulator.
