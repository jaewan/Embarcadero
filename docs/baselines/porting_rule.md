# The Ex Ante Baseline Porting Rule

**Status:** normative. Written *before* any baseline is ported (W1 on-paper closure, panel #4).
**Owner:** Track 03 (baseline CXL-transport ports).
**Governs:** every CXL adaptation of Corfu, Scalog, and LazyLog used in the E1 decomposition
waterfall and elsewhere in the evaluation.

This document is the contract that makes the E1 waterfall a *measurement* instead of a knob. It is
committed to the repository ahead of the ports so that no adaptation decision can be made after
seeing a number. Cite it in the paper's methodology intro (IV.0 rule 3) and in the fairness
appendix (`docs/baselines/fairness_appendix.md`).

---

## 1. The rule, in one sentence

> **Protocol-preserving substitution only: same messages, same state machine, transport swapped.**

A CXL "port" of a baseline may change *how bytes move between the participants* and nothing else.
The set of messages, their fields and meaning, the order in which they are exchanged, the state each
participant keeps, and the function that turns received messages into an ordering decision must be
**bit-for-bit the same protocol** as the baseline running over TCP/gRPC. Only the wire — gRPC over
TCP — is replaced by the CXL shared-memory mailbox (`src/cxl_transport/`).

## 2. Why this rule exists (what it protects)

The E1 waterfall has four legs:

```
  baseline-on-TCP  →  baseline-on-CXL-transport  →  Embarcadero-on-RDMA  →  Embarcadero-on-CXL
        (leg 1)              (leg 2, THIS RULE)          (leg 3, W5-A)          (leg 4)
```

Leg 2 exists to factor out **transport** from **architecture**. It answers: "how much of
Embarcadero's advantage is just that CXL is a faster wire, versus that Embarcadero's *ordering
architecture* is different?" That decomposition is only meaningful if leg 2 is *the same baseline,
faster wire* — nothing else.

Without an ex ante rule, leg 2 becomes a free parameter. Every liberty taken while "adapting" a
baseline to CXL — moving its coordinator's aggregation into a poll loop, letting the sequencer read
broker state directly instead of receiving reports, dropping the epoch barrier — quietly imports a
piece of Embarcadero's *architecture* into the baseline. In the limit you tune the baseline into
"Embarcadero minus the hold buffer," at which point leg 2 ≈ leg 4 and the waterfall proves nothing.
The rule forecloses that: **anything that changes a baseline's serialization structure is a
redesign, not a port, and is forbidden** for the waterfall.

This cuts both ways and is deliberately adversarial to *our own* thesis. Under this rule, E1 **can
shoot the author** (plan IV.2/E1): if the honest transport swap alone closes the latency knee, the
SLO headline is transport, not architecture, and the paper retreats to E4/E6. We publish whatever
leg 2 says. The rule's job is to guarantee that whatever it says is *true of the baseline*, not an
artifact of how generously or stingily we ported it.

## 3. What "protocol" means (the four preserved invariants)

For each baseline, the following four things are **frozen** by the baseline's original design and may
not be altered by a port. A change to any of them is a redesign.

1. **Message set & semantics.** The same logical messages, same fields, same meaning. You may
   re-encode them for the mailbox (a C struct instead of a protobuf) but you may not add, remove,
   split, merge, or repurpose a message, nor change what a field means.
2. **Exchange pattern & directionality.** Who initiates, who responds, unary vs. streaming,
   one-to-one vs. broadcast, and the *causal order* of the exchange are preserved. A unary
   request/response stays request/response; a per-epoch report→aggregate→broadcast stays
   report→aggregate→broadcast.
3. **State machine & decision function.** The state each participant keeps and the function that
   maps received messages to an ordering decision are identical: Corfu's monotonic token counter and
   per-client sequencing; Scalog's per-epoch cut with `min` across replicas per shard; LazyLog's
   binding-round delta computation. The arithmetic that produces `total_order` does not change.
4. **Coupling to durability & the ACK frontier.** Whatever the baseline couples ordering to
   (Scalog/LazyLog gate the ordered frontier on replication progress; see
   `SCALOG_LIMITATION.md` §5), that coupling is preserved. A port may not decouple ordering from
   durability — that *is* Embarcadero's architectural move (plan §6.3) and importing it is exactly
   the forbidden case.

If all four are preserved and only the transport differs, it is a port. If any one changes, it is a
redesign.

## 4. What may change (the transport, and only the transport)

Permitted, because they are properties of the wire, not the protocol:

- **Encoding for the mailbox.** Protobuf-on-gRPC → a fixed-layout POD record in a single-writer
  ring. The *fields* are the same; only their serialization changes.
- **Delivery mechanism.** A gRPC unary call → a request slot + response slot in the mailbox. A gRPC
  bidi stream → a pair of single-writer rings (one per direction). Flow control that gRPC did
  implicitly becomes explicit ring back-pressure (see §6 — this must be *added*, since the CXL rings
  are finite; it is a faithful realization of what gRPC already provided, not new protocol).
- **Notification.** Blocking gRPC completion → poll/`clwb`+`sfence` visibility on the ring head,
  consistent with the coherence-free discipline (single-writer ownership, monotonic updates,
  host-local flushes). This is how *every* participant on CXL learns of new bytes; it is transport.
- **Addressing.** IP:port → a named mailbox segment offset.

Permitted only if it is *transport-forced and semantics-neutral*: batching a baseline's messages to
amortize mailbox crossings **is allowed only if the baseline already batched at that boundary** (see
Corfu, §5.1 — Corfu tokens are already per-batch). Introducing a *new* batching boundary the
baseline did not have is a redesign, because batch granularity changes the serialization structure.

## 5. Per-baseline application

Each subsection states the frozen protocol (from the current gRPC implementation) and the exact
transport swap. The "redesign tripwires" list the tempting changes that are **forbidden**.

### 5.1 Corfu — token path

- **Frozen protocol.** Unary `GetTotalOrder(TotalOrderRequest) → TotalOrderResponse`
  (`corfu_sequencer_service.{h,cc}`). **The client requests a token for a batch *before* it writes the
  batch (token-before-write)**; the sequencer assigns a contiguous range off a single monotonic
  counter (`next_order_`) and returns it. Per-`client_id` serialization (one mutex per client) makes
  `total_order` a valid shuffle of per-client submission order even when a client fans out across
  brokers. Token granularity is **per batch** already.
- **Transport swap = proxy-relay only.** The sequencer is co-located in the CXL pod and its
  request/response hop runs over the CXL mailbox instead of gRPC. Because an external client cannot
  address CXL, the client's token request reaches the sequencer via its **ingress broker acting as a
  pure transport proxy**:
  `client →(network)→ broker →(CXL mailbox)→ sequencer →(CXL mailbox)→ broker →(network)→ client`.
  The client **still issues the request and still blocks until the token returns before its payload
  write is ordered/acked** — token-before-write is preserved on the client-visible path, and the
  **two client-side network round trips (token, then write) remain**. That 2-RTT client path is the
  legitimate architectural cost E1 is built to expose (the author response conceded it is irreducible
  for Corfu unless clients also attach to CXL); it must be **measured, not engineered away**. Only the
  wire of the sequencer-facing hop changes; sequencer-internal assignment (counter + per-client logic)
  is untouched. The pod-internal broker↔sequencer hop is what `CorfuMailboxSequencer` implements (E10).
- **E10 batched sequencer.** The batched CXL-Corfu sequencer (plan E10, the honest baseline
  reviewer C demanded; the per-message-mutex strawman is deleted) is the *same* protocol at the
  batch granularity Corfu already uses — this is a port, not a redesign, precisely because Corfu's
  token is already per-batch. Keep it comparable to Embarcadero's sequencer.
- **Redesign tripwires (forbidden):**
  - **Requester / token-RTT collapse — the panel #4 tripwire.** Having the broker request the token,
    write the payload, and ack the client in a *single* `client→broker` round trip — so the client no
    longer waits for a token before its write is ordered/acked — turns Corfu into **write-before-order
    (Embarcadero-minus-hold-buffer)** and makes leg 2 *"us,"* not *"Corfu on CXL."* The broker may act
    **only** as a transport proxy for the token request; it must never fold token acquisition into the
    write/ack path.
  - letting the sequencer *read* broker batch state from CXL instead of receiving a token request
    (removes the request message — that is Embarcadero's poll model);
  - assigning tokens per epoch instead of per request (changes granularity);
  - dropping the per-client mutex (changes the decision function / the FIFO shuffle it guarantees).
- **Ownership.** The client-visible token path must live in a **Track-03 Corfu baseline client**, not
  in Embarcadero's shared `src/client/publisher.*` (Track 01). Corfu today co-opts the shared
  publisher via `src/client/corfu_client.h`; the E1 port **forks a baseline client** instead. Any
  unavoidable hook in the shared publisher is *specced to Track 01*, not applied here.

### 5.2 Scalog — cut / report path

- **Frozen protocol.** `RegisterBroker`, then a bidi stream carrying `LocalCut` (per-epoch, per
  broker/replica logical offsets) up to the global sequencer and `GlobalCut` back down
  (`scalog_global_sequencer.{h,cc}`, `scalog_local_sequencer.{h,cc}`). Each epoch: local sequencers
  report their cut, the global sequencer aggregates and computes `min` across replicas per shard,
  emits the `GlobalCut`; local sequencers apply it to assign `total_order`. Ordering is gated on the
  `min` across replicas (durability coupling — preserve it).
- **Transport swap.** `RegisterBroker` → a registration slot in a control mailbox. The
  `LocalCut`-up stream → one single-writer ring per local sequencer carrying `LocalCut` PODs. The
  `GlobalCut`-down stream → one broadcast ring (or per-broker rings) carrying `GlobalCut` PODs. The
  global sequencer's aggregation (`min` across replicas per shard, per epoch) is unchanged; only the
  read/write of cuts moves from gRPC to rings.
- **Redesign tripwires (forbidden):** collapsing the epoch barrier so the sequencer emits order
  continuously as offsets arrive (removes the cut cadence — that is the architectural difference the
  knee measures); computing the global cut from broker state the sequencer *reads* rather than from
  received `LocalCut` messages; decoupling the ordered frontier from the replica `min` (removes the
  durability coupling); changing `min` to any other aggregator.

### 5.3 LazyLog — coordinator path

- **Frozen protocol.** `RegisterBroker`, then a bidi stream carrying `LocalProgress` (per-broker
  written/durable frontier) up and `GlobalBinding` down (`lazylog_global_sequencer.{h,cc}`,
  `lazylog_local_sequencer.{h,cc}`). Each binding round: local sequencers report progress, the
  coordinator computes available deltas per broker and emits a `GlobalBinding`; local sequencers
  apply it to assign `total_order`. Per fix 5c (`SCALOG_LIMITATION.md`), progress is `min` across
  all replicas in the set; per fix 5d there is no artificial binding cap. Preserve both.
- **Transport swap.** Same shape as Scalog: registration slot; one up-ring per local sequencer
  carrying `LocalProgress` PODs; one down-ring carrying `GlobalBinding` PODs. The coordinator's
  binding-round delta computation is unchanged; only the transport moves to rings.
- **Redesign tripwires (forbidden):** turning binding rounds into a continuous poll of broker
  frontiers (removes the coordinator round-trip on the ordering path); re-introducing the removed
  binding cap or narrowing the `min`-across-replicas progress (re-introduces the unfair advantage
  fix 5c/5d removed — fairness cuts both ways); decoupling binding from durability progress.

## 6. The shared CXL mailbox (one transport, three ports)

All three baselines are ported onto **one** shared-memory mailbox RPC library, built from *our own*
primitives so the fairness story is that they use the same coherence-free discipline Embarcadero
does — not a borrowed RDMA/CXL RPC stack:

- **Single-writer rings.** Each ring has exactly one writer (single-writer ownership — the CXL
  correctness discipline). Request/report directions get one ring per client/broker; response/cut/
  binding directions get a broadcast or per-broker ring.
- **Visibility.** Writer publishes with `clwb` + `sfence`; reader polls the head with monotonic,
  host-local reads. No cross-host atomics on the data path.
- **Explicit back-pressure.** gRPC provided flow control implicitly; finite CXL rings make it
  explicit. Back-pressure is a faithful realization of what gRPC already did (a full ring blocks the
  writer exactly as a full gRPC window would), **not** new protocol. This is the one place a port
  *adds* mechanism, and it is transport, not serialization structure.

The mailbox lives in `src/cxl_transport/` (new). It must not depend on, or be depended on by,
`src/embarlet/topic.{cc,h}` or the epoch collector (Track 01 ownership). Any primitive we genuinely
share with Embarcadero is added as a *new* file under `src/common/` or `src/cxl_transport/` and
flagged for Track 01 in the handoff — never by editing their files.

## 7. Calibration — turning "trust our port" into "our port matches theirs" (panel #10)

The rule constrains *what* we port; calibration proves we ported it *correctly*.

- **TCP calibration.** Each reimplementation, run on TCP, must reproduce its original paper's
  published numbers within a **stated tolerance** on comparable hardware. Where the original number
  exists, this converts "trust our port" into "our port matches theirs." Where no comparable number
  exists, say so explicitly.
- **Record in the fairness appendix.** `docs/baselines/fairness_appendix.md` records, per baseline:
  kept / changed / why, the calibration target, the measured value, the tolerance, and the verdict.
- **Author sanity check.** Draft short emails to the LazyLog and Scalog authors asking them to
  review the port descriptions. "Baseline authors reviewed our adaptation" is one appendix sentence
  worth having. (Flag to the human to send; do not send unprompted.)

## 8. The decision test (apply to every proposed change)

Before making any change while porting, answer:

1. **Does it add, remove, split, merge, or repurpose a message, or change a field's meaning?**
   → redesign. Forbidden.
2. **Does it change who initiates, the directionality, or the causal order of the exchange?**
   → redesign. Forbidden.
3. **Does it change the state kept or the function that produces `total_order`?**
   → redesign. Forbidden.
4. **Does it change what ordering is coupled to (durability / replica `min` / epoch cadence)?**
   → redesign. Forbidden.
5. **Does it only change how bytes move (encoding, ring vs. stream, poll vs. block, addressing) or
   add transport-forced back-pressure the baseline's implicit flow control already provided?**
   → port. Allowed.

If the honest answer to 1–4 is "yes," the change does not belong in the E1 waterfall baseline. If
such a change is genuinely interesting (e.g., a legitimately better CXL-native Corfu), it is a
*separate, clearly-labeled* system — not "CXL-Corfu" in the waterfall — and must be argued on its
own, never substituted for the port.

---

## References

- `Paper/improvement_plan.md`: W4 (this rule), E1 (waterfall), E10 (batched CXL-Corfu), IV.0 rule 3
  (methodology), IV.1 (per-baseline knee predictions), I.3 (concessions).
- `docs/baselines/SCALOG_LIMITATION.md`: the architectural convergence on CXL (§6) this rule
  formalizes, and the fairness fixes (§5) whose semantics ports must preserve.
- `docs/baselines/fairness_appendix.md`: per-baseline kept/changed/why + calibration + author-check
  status (companion to this rule).
