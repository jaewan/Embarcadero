-------------------------------- MODULE Embarcadero --------------------------------
(***************************************************************************)
(* Track 02 --- TLA+ specification of the Embarcadero session protocol.      *)
(*                                                                          *)
(* SINGLE source of truth for the state machine.  Each W2 scenario is a      *)
(* separate .cfg that toggles the adversary/feature CONSTANT flags and       *)
(* selects which safety invariants to check (see spec/README.md).            *)
(*                                                                          *)
(* Model checking is SAFETY-ONLY; availability/liveness (stall -> retransmit  *)
(* -> fence, and the durable frontier eventually advancing) is modelled      *)
(* structurally but argued in prose.  spec/README.md states EXACTLY which     *)
(* clauses of the paper's guarantee theorem are machine-checked here.         *)
(*                                                                          *)
(* Mechanisms modelled:                                                      *)
(*   - Sessions & per-session FIFO (D1)                                       *)
(*   - Two-phase PBR publication + broker crash pre/post publish (3.1, W2#1)  *)
(*   - Client retransmit to a different broker (D1, W2#2)                     *)
(*   - Sequencer dedup via expected_seq; late/dup rejected, never reordered   *)
(*   - Hold buffer + cascade release; assign-at-release (D4)                  *)
(*   - Session fencing on hold expiry / client crash (D1; never gap-skip)     *)
(*   - Exactly-once across fencing: session re-open + suffix resubmit (D1)    *)
(*   - Spatial guard + wrap-fence (praised, kept)                            *)
(*   - Control-block epoch + sequencer failover w/ GOI truncation + PBR       *)
(*     rescan from the durable frontier (D3/D7, W2#7)                         *)
(*   - Lease false positive / SIGSTOP pause -> duplicate discarded (D2, W2#4) *)
(*   - Completion-vector ACK path with a per-signal epoch tag; broker ACK-    *)
(*     relay epoch check (D5/D2).  The zombie stale-CV advance is a real      *)
(*     always-enabled action; the epoch check on the relay path is the fix.   *)
(*   - Stale-CV ACK relay after election (D2, W2#5 --- the priority bug)      *)
(*   - CXL-module loss in the ACK1->ACK2 window (D5, W2#8) with a speculative  *)
(*     (ACK1) reader, so the theorem's reader-agreement / sole-divergence     *)
(*     clause is machine-checked (payload visibility may differ at a durable   *)
(*     position; order/identity never does within an epoch).                  *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Clients,                \* set of client ids (model values, symmetric)
    Brokers,                \* set of broker ids (model values, symmetric)
    MaxSeq,                 \* each client submits client_seq in 1..MaxSeq, in order
    GOICap,                 \* GOI capacity ahead of the replication floor (wrap-fence)
    MaxEpoch,               \* max control-block epoch (number of failovers allowed)
    MaxReopen,              \* max session re-opens per client (exactly-once test)
    EnableBrokerCrash,      \* adversary: broker host crash (W2#1/#2)
    EnableFailover,         \* adversary: sequencer failover / new epoch (W2#5/#7)
    EnableStaleCVRelay,     \* adversary: zombie S_old advances CV post-election (W2#5)
    EnableAckEpochCheck,    \* FIX (D2): broker validates CV-signal epoch on ACK relay
    EnableModuleLoss,       \* adversary: CXL-module loss in ACK1->ACK2 window (W2#8)
    EnableLeasePause,       \* adversary: broker SIGSTOP pause/resume (W2#4)
    EnableReopen,           \* feature: fenced-but-live client re-opens + resubmits (D1)
    EnableSpecReader        \* feature: speculative (ACK1) reader for reader-agreement

ASSUME MaxSeq \in Nat /\ MaxSeq >= 1
ASSUME GOICap \in Nat /\ GOICap >= 1
ASSUME MaxEpoch \in Nat /\ MaxReopen \in Nat
ASSUME /\ EnableBrokerCrash   \in BOOLEAN
       /\ EnableFailover      \in BOOLEAN
       /\ EnableStaleCVRelay  \in BOOLEAN
       /\ EnableAckEpochCheck \in BOOLEAN
       /\ EnableModuleLoss    \in BOOLEAN
       /\ EnableLeasePause    \in BOOLEAN
       /\ EnableReopen        \in BOOLEAN
       /\ EnableSpecReader    \in BOOLEAN

VARIABLES
    clientNext,         \* [Clients -> Nat] next client_seq the client will submit
    net,                \* set of in-flight batch messages [client,cseq,broker]
    pbrReserved,        \* [Brokers -> SUBSET slot] Phase-1 reserved, not yet written
    pbr,                \* [Brokers -> Seq(slot)] Phase-2 written, visible to sequencer
    collectCursor,      \* [Brokers -> Nat] sequencer scan cursor into pbr[b]
    expectedSeq,        \* [Clients -> Nat] sequencer session table: next cseq to commit
    hold,               \* [Clients -> SUBSET held] gap-held entries [cseq,epoch]
    goi,                \* Seq(entry) the single total order [client,cseq,epoch,lost]
    sessionFenced,      \* [Clients -> BOOL] session currently fenced
    fenceHWM,           \* [Clients -> Nat] committed high-water mark captured at fence
    sessEpoch,          \* [Clients -> Nat] session generation (bumped on re-open, D1)
    replicationFloor,   \* Nat: number of leading GOI entries that are durable (ACK2/CV)
    crashed,            \* SUBSET Brokers that have crashed
    paused,             \* SUBSET Brokers currently SIGSTOP-paused (lease false positive)
    cbEpoch,            \* control-block epoch (incremented on failover)
    cvSig,              \* SUBSET [client,cseq,epoch]: completion-vector advance signals.
                        \*   Legit advances carry the current epoch; a zombie S_old
                        \*   carries its old epoch.  The ACK relay applies the epoch check.
    acked,              \* [Clients -> SUBSET cseq] client-visible ACKs (CV/durable path)
    orphaned,           \* SUBSET [client,cseq] entries truncated by a failover
    specSnap,           \* Seq([client,cseq]): what the speculative (ACK1) reader delivered,
                        \*   position by position (index k = GOI position it read)
    retxUsed            \* [Clients -> SUBSET cseq] cseqs already retransmitted once

vars == <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq, hold,
          goi, sessionFenced, fenceHWM, sessEpoch, replicationFloor, crashed,
          paused, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

(***************************************************************************)
(* Helpers                                                                  *)
(***************************************************************************)

HeldSeqs(c) == { r.cseq : r \in hold[c] }

AtBroker(b, c, s) ==
    \/ \E r \in pbrReserved[b] : r.client = c /\ r.cseq = s
    \/ \E i \in 1..Len(pbr[b]) : pbr[b][i].client = c /\ pbr[b][i].cseq = s

GoiIdx(c)        == { j \in 1..Len(goi) : goi[j].client = c }
CommittedSeqs(c) == { goi[j].cseq : j \in GoiIdx(c) }

IsCommittedLive(c, s) ==
    \E j \in 1..Len(goi) : goi[j].client = c /\ goi[j].cseq = s /\ ~goi[j].lost

IsDurableLive(c, s) ==
    \E j \in 1..Len(goi) :
        /\ j <= replicationFloor
        /\ goi[j].client = c /\ goi[j].cseq = s /\ ~goi[j].lost

CanCommit == (Len(goi) - replicationFloor) < GOICap

SetMax(S) == CHOOSE m \in S : \A x \in S : x <= m
Min2(a, b) == IF a < b THEN a ELSE b

NewExpected(kept, c) ==
    LET S == { kept[j].cseq : j \in {k \in 1..Len(kept) : kept[k].client = c} }
    IN IF S = {} THEN 1 ELSE SetMax(S) + 1

(***************************************************************************)
(* Init                                                                     *)
(***************************************************************************)
Init ==
    /\ clientNext       = [c \in Clients |-> 1]
    /\ net              = {}
    /\ pbrReserved      = [b \in Brokers |-> {}]
    /\ pbr              = [b \in Brokers |-> << >>]
    /\ collectCursor    = [b \in Brokers |-> 0]
    /\ expectedSeq      = [c \in Clients |-> 1]
    /\ hold             = [c \in Clients |-> {}]
    /\ goi              = << >>
    /\ sessionFenced    = [c \in Clients |-> FALSE]
    /\ fenceHWM         = [c \in Clients |-> 0]
    /\ sessEpoch        = [c \in Clients |-> 0]
    /\ replicationFloor = 0
    /\ crashed          = {}
    /\ paused           = {}
    /\ cbEpoch          = 0
    /\ cvSig            = {}
    /\ acked            = [c \in Clients |-> {}]
    /\ orphaned         = {}
    /\ specSnap         = << >>
    /\ retxUsed         = [c \in Clients |-> {}]

(***************************************************************************)
(* Client actions (D1)                                                      *)
(***************************************************************************)
ClientSubmit(c, b) ==
    /\ clientNext[c] <= MaxSeq
    /\ ~sessionFenced[c]
    /\ b \notin crashed
    /\ net' = net \cup {[client |-> c, cseq |-> clientNext[c], broker |-> b]}
    /\ clientNext' = [clientNext EXCEPT ![c] = @ + 1]
    /\ UNCHANGED <<pbrReserved, pbr, collectCursor, expectedSeq, hold, goi,
                   sessionFenced, fenceHWM, sessEpoch, replicationFloor, crashed,
                   paused, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

ClientRetransmit(c, s, b) ==
    /\ s \in 1..(clientNext[c] - 1)
    /\ ~sessionFenced[c]
    /\ s \notin retxUsed[c]
    /\ b \notin crashed
    /\ [client |-> c, cseq |-> s, broker |-> b] \notin net
    /\ net' = net \cup {[client |-> c, cseq |-> s, broker |-> b]}
    /\ retxUsed' = [retxUsed EXCEPT ![c] = @ \cup {s}]
    /\ UNCHANGED <<clientNext, pbrReserved, pbr, collectCursor, expectedSeq, hold,
                   goi, sessionFenced, fenceHWM, sessEpoch, replicationFloor, crashed,
                   paused, cbEpoch, cvSig, acked, orphaned, specSnap>>

(***************************************************************************)
(* Broker actions --- two-phase PBR publication                             *)
(***************************************************************************)
BrokerReserve(b) ==
    /\ b \notin crashed
    /\ b \notin paused
    /\ \E m \in net :
        /\ m.broker = b
        /\ ~AtBroker(b, m.client, m.cseq)
        /\ pbrReserved' = [pbrReserved EXCEPT ![b] =
                             @ \cup {[client |-> m.client, cseq |-> m.cseq, epoch |-> cbEpoch]}]
        /\ net' = net \ {m}
    /\ UNCHANGED <<clientNext, pbr, collectCursor, expectedSeq, hold, goi,
                   sessionFenced, fenceHWM, sessEpoch, replicationFloor, crashed,
                   paused, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

BrokerWrite(b) ==
    /\ b \notin crashed
    /\ b \notin paused
    /\ \E slot \in pbrReserved[b] :
        /\ pbr' = [pbr EXCEPT ![b] = Append(@, slot)]
        /\ pbrReserved' = [pbrReserved EXCEPT ![b] = @ \ {slot}]
    /\ UNCHANGED <<clientNext, net, collectCursor, expectedSeq, hold, goi,
                   sessionFenced, fenceHWM, sessEpoch, replicationFloor, crashed,
                   paused, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

BrokerCrash(b) ==
    /\ EnableBrokerCrash
    /\ b \notin crashed
    /\ crashed' = crashed \cup {b}
    /\ pbrReserved' = [pbrReserved EXCEPT ![b] = {}]
    /\ net' = { m \in net : m.broker # b }
    /\ UNCHANGED <<clientNext, pbr, collectCursor, expectedSeq, hold, goi,
                   sessionFenced, fenceHWM, sessEpoch, replicationFloor, paused,
                   cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

BrokerPause(b) ==
    /\ EnableLeasePause
    /\ b \notin paused
    /\ b \notin crashed
    /\ paused' = paused \cup {b}
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   hold, goi, sessionFenced, fenceHWM, sessEpoch, replicationFloor,
                   crashed, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

BrokerResume(b) ==
    /\ b \in paused
    /\ paused' = paused \ {b}
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   hold, goi, sessionFenced, fenceHWM, sessEpoch, replicationFloor,
                   crashed, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

(***************************************************************************)
(* Sequencer actions --- collect, dedup, hold, assign-at-release            *)
(***************************************************************************)
SeqCollectDiscard(b) ==
    /\ collectCursor[b] < Len(pbr[b])
    /\ LET i == collectCursor[b] + 1
           e == pbr[b][i]
       IN /\ ( sessionFenced[e.client]
               \/ e.cseq < expectedSeq[e.client]
               \/ e.cseq \in HeldSeqs(e.client) )
          /\ collectCursor' = [collectCursor EXCEPT ![b] = i]
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, expectedSeq, hold, goi,
                   sessionFenced, fenceHWM, sessEpoch, replicationFloor, crashed,
                   paused, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

SeqCollectCommit(b) ==
    /\ collectCursor[b] < Len(pbr[b])
    /\ CanCommit
    /\ LET i == collectCursor[b] + 1
           e == pbr[b][i]
           c == e.client
       IN /\ ~sessionFenced[c]
          /\ e.cseq = expectedSeq[c]
          /\ goi' = Append(goi, [client |-> c, cseq |-> e.cseq, epoch |-> cbEpoch, lost |-> FALSE])
          /\ expectedSeq' = [expectedSeq EXCEPT ![c] = @ + 1]
          /\ collectCursor' = [collectCursor EXCEPT ![b] = i]
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, hold, sessionFenced,
                   fenceHWM, sessEpoch, replicationFloor, crashed, paused, cbEpoch,
                   cvSig, acked, orphaned, specSnap, retxUsed>>

SeqCollectHold(b) ==
    /\ collectCursor[b] < Len(pbr[b])
    /\ LET i == collectCursor[b] + 1
           e == pbr[b][i]
           c == e.client
       IN /\ ~sessionFenced[c]
          /\ e.cseq > expectedSeq[c]
          /\ e.cseq \notin HeldSeqs(c)
          /\ hold' = [hold EXCEPT ![c] = @ \cup {[cseq |-> e.cseq, epoch |-> cbEpoch]}]
          /\ collectCursor' = [collectCursor EXCEPT ![b] = i]
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, expectedSeq, goi,
                   sessionFenced, fenceHWM, sessEpoch, replicationFloor, crashed,
                   paused, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

SeqReleaseHeld(c) ==
    /\ ~sessionFenced[c]
    /\ CanCommit
    /\ \E r \in hold[c] :
        /\ r.cseq = expectedSeq[c]
        /\ goi' = Append(goi, [client |-> c, cseq |-> r.cseq, epoch |-> cbEpoch, lost |-> FALSE])
        /\ expectedSeq' = [expectedSeq EXCEPT ![c] = @ + 1]
        /\ hold' = [hold EXCEPT ![c] = @ \ {r}]
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor,
                   sessionFenced, fenceHWM, sessEpoch, replicationFloor, crashed,
                   paused, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

(***************************************************************************)
(* Fencing (D1) --- hold expiry / client crash mid-stream                   *)
(***************************************************************************)
Fence(c) ==
    /\ ~sessionFenced[c]
    /\ hold[c] # {}
    /\ sessionFenced' = [sessionFenced EXCEPT ![c] = TRUE]
    /\ fenceHWM'      = [fenceHWM EXCEPT ![c] = expectedSeq[c] - 1]
    /\ hold'          = [hold EXCEPT ![c] = {}]
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   goi, sessEpoch, replicationFloor, crashed, paused, cbEpoch,
                   cvSig, acked, orphaned, specSnap, retxUsed>>

\* Exactly-once across fencing (D1): a fenced-but-LIVE client re-opens under a new
\* session generation and resubmits exactly the uncommitted suffix (from its HWM).
\* Dedup + in-order commit make this exactly-once: no batch is committed twice and
\* the committed prefix stays contiguous (checked by NoDupCommit + PerSessionPrefix).
Reopen(c) ==
    /\ EnableReopen
    /\ sessionFenced[c]
    /\ sessEpoch[c] < MaxReopen
    /\ sessionFenced' = [sessionFenced EXCEPT ![c] = FALSE]
    /\ sessEpoch'     = [sessEpoch EXCEPT ![c] = @ + 1]
    /\ clientNext'    = [clientNext EXCEPT ![c] = fenceHWM[c] + 1]
    /\ retxUsed'      = [retxUsed EXCEPT ![c] = {}]
    /\ UNCHANGED <<net, pbrReserved, pbr, collectCursor, expectedSeq, hold, goi,
                   fenceHWM, replicationFloor, crashed, paused, cbEpoch, cvSig,
                   acked, orphaned, specSnap>>

(***************************************************************************)
(* Replication + completion-vector ACK path (D5/D2)                         *)
(***************************************************************************)

\* Chain replication advances the durable (ACK2) frontier by one, in order.
Replicate ==
    /\ replicationFloor < Len(goi)
    /\ replicationFloor' = replicationFloor + 1
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   hold, goi, sessionFenced, fenceHWM, sessEpoch, crashed, paused,
                   cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

\* Legit completion-vector advance: the tail replica advances the CV for a
\* committed+durable+live batch, tagged with the CURRENT control-block epoch.
CVAdvance(c, s) ==
    /\ IsDurableLive(c, s)
    /\ [client |-> c, cseq |-> s, epoch |-> cbEpoch] \notin cvSig
    /\ cvSig' = cvSig \cup {[client |-> c, cseq |-> s, epoch |-> cbEpoch]}
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   hold, goi, sessionFenced, fenceHWM, sessEpoch, replicationFloor,
                   crashed, paused, cbEpoch, acked, orphaned, specSnap, retxUsed>>

\* STALE-CV ADVANCE (the priority bug's cause, D2/W2#5): a zombie S_old, after
\* election, advances the CV for a batch the new epoch truncated (`orphaned`),
\* tagged with its OWN (old) epoch.  This is a REAL, always-enabled action --- the
\* zombie is not prevented from writing.  Safety is preserved not by stopping the
\* zombie but by the broker's epoch check on the relay path (AckRelay below).
StaleCVAdvance(c, s) ==
    /\ EnableStaleCVRelay
    /\ cbEpoch >= 1
    /\ [client |-> c, cseq |-> s] \in orphaned
    /\ [client |-> c, cseq |-> s, epoch |-> cbEpoch - 1] \notin cvSig
    /\ cvSig' = cvSig \cup {[client |-> c, cseq |-> s, epoch |-> cbEpoch - 1]}
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   hold, goi, sessionFenced, fenceHWM, sessEpoch, replicationFloor,
                   crashed, paused, cbEpoch, acked, orphaned, specSnap, retxUsed>>

\* Broker ACK relay.  Relays a client ACK on seeing a CV signal for (c,s).  THE FIX
\* (D2): when EnableAckEpochCheck, the broker validates the active control-block
\* epoch --- it relays only for a CV signal carrying the current epoch, so a stale
\* signal from a zombie S_old (old epoch) is refused.  With the check off, any CV
\* signal is relayed, reproducing the bug (see stale_cv_bug_demo.cfg).
AckRelay(c, s) ==
    /\ \E sig \in cvSig :
        /\ sig.client = c /\ sig.cseq = s
        /\ (EnableAckEpochCheck => sig.epoch = cbEpoch)
    /\ s \notin acked[c]
    /\ acked' = [acked EXCEPT ![c] = @ \cup {s}]
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   hold, goi, sessionFenced, fenceHWM, sessEpoch, replicationFloor,
                   crashed, paused, cbEpoch, cvSig, orphaned, specSnap, retxUsed>>

\* Speculative (ACK1) reader (D5): reads the ordered frontier ahead of durability
\* and records, position by position, the (client,cseq) it delivered.  Used to
\* machine-check the theorem's reader-agreement / sole-divergence clause.
SpecRead ==
    /\ EnableSpecReader
    /\ Len(specSnap) < Len(goi)
    /\ LET k == Len(specSnap) + 1
       IN specSnap' = Append(specSnap, [client |-> goi[k].client, cseq |-> goi[k].cseq])
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   hold, goi, sessionFenced, fenceHWM, sessEpoch, replicationFloor,
                   crashed, paused, cbEpoch, cvSig, acked, orphaned, retxUsed>>

\* CXL-module loss in the ACK1->ACK2 window (D5/W2#8): a committed-but-not-durable
\* entry loses its payload.  Order/identity survive (GOI position and (client,cseq)
\* unchanged); only visibility (lost) flips.
ModuleLoss(j) ==
    /\ EnableModuleLoss
    /\ j \in 1..Len(goi)
    /\ j > replicationFloor
    /\ ~goi[j].lost
    /\ goi' = [goi EXCEPT ![j].lost = TRUE]
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, collectCursor, expectedSeq,
                   hold, sessionFenced, fenceHWM, sessEpoch, replicationFloor,
                   crashed, paused, cbEpoch, cvSig, acked, orphaned, specSnap, retxUsed>>

(***************************************************************************)
(* Sequencer failover (D3/D7, W2#7)                                          *)
(*                                                                          *)
(* New epoch.  GOI truncates to the durable frontier (speculative, non-       *)
(* durable entries are discarded by epoch --- the ACK1 speculative window);   *)
(* truncated batches become `orphaned` (their PBR slots survive in CXL).      *)
(* S_new rebuilds `expectedSeq` from the surviving GOI and rescans the PBRs.   *)
(* Speculative reads beyond the durable frontier are epoch-scoped: the reader  *)
(* rewinds to the durable frontier (its speculative-zone snapshot is dropped   *)
(* --- it must re-read under the new epoch).                                   *)
(***************************************************************************)
SequencerFailover ==
    /\ EnableFailover
    /\ cbEpoch < MaxEpoch
    /\ LET keep    == replicationFloor
           kept    == SubSeq(goi, 1, keep)
           removed == { [client |-> goi[j].client, cseq |-> goi[j].cseq]
                          : j \in (keep + 1)..Len(goi) }
       IN /\ goi'         = kept
          /\ orphaned'    = orphaned \cup removed
          /\ expectedSeq' = [c \in Clients |-> NewExpected(kept, c)]
          /\ specSnap'    = SubSeq(specSnap, 1, Min2(Len(specSnap), keep))
    /\ cbEpoch'       = cbEpoch + 1
    /\ hold'          = [c \in Clients |-> {}]
    /\ collectCursor' = [b \in Brokers |-> 0]
    /\ UNCHANGED <<clientNext, net, pbrReserved, pbr, sessionFenced, fenceHWM,
                   sessEpoch, replicationFloor, crashed, paused, cvSig, acked, retxUsed>>

(***************************************************************************)
(* Next / Spec                                                              *)
(***************************************************************************)
Next ==
    \/ \E c \in Clients, b \in Brokers : ClientSubmit(c, b)
    \/ \E c \in Clients, s \in 1..MaxSeq, b \in Brokers : ClientRetransmit(c, s, b)
    \/ \E b \in Brokers : BrokerReserve(b)
    \/ \E b \in Brokers : BrokerWrite(b)
    \/ \E b \in Brokers : BrokerCrash(b)
    \/ \E b \in Brokers : BrokerPause(b)
    \/ \E b \in Brokers : BrokerResume(b)
    \/ \E b \in Brokers : SeqCollectDiscard(b)
    \/ \E b \in Brokers : SeqCollectCommit(b)
    \/ \E b \in Brokers : SeqCollectHold(b)
    \/ \E c \in Clients : SeqReleaseHeld(c)
    \/ \E c \in Clients : Fence(c)
    \/ \E c \in Clients : Reopen(c)
    \/ Replicate
    \/ \E c \in Clients, s \in 1..MaxSeq : CVAdvance(c, s)
    \/ \E c \in Clients, s \in 1..MaxSeq : StaleCVAdvance(c, s)
    \/ \E c \in Clients, s \in 1..MaxSeq : AckRelay(c, s)
    \/ SpecRead
    \/ \E j \in 1..(Cardinality(Clients) * MaxSeq) : ModuleLoss(j)
    \/ SequencerFailover

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Type invariant                                                           *)
(***************************************************************************)
CursorBound == Cardinality(Clients) * MaxSeq * Cardinality(Brokers)
TypeOK ==
    /\ clientNext \in [Clients -> 1..(MaxSeq + 1)]
    /\ \A m \in net : m.client \in Clients /\ m.cseq \in 1..MaxSeq /\ m.broker \in Brokers
    /\ expectedSeq \in [Clients -> 1..(MaxSeq + 1)]
    /\ sessionFenced \in [Clients -> BOOLEAN]
    /\ fenceHWM \in [Clients -> 0..MaxSeq]
    /\ sessEpoch \in [Clients -> 0..MaxReopen]
    /\ replicationFloor \in 0..Len(goi)
    /\ crashed \subseteq Brokers
    /\ paused \subseteq Brokers
    /\ cbEpoch \in 0..MaxEpoch
    /\ collectCursor \in [Brokers -> 0..CursorBound]
    /\ acked \in [Clients -> SUBSET (1..MaxSeq)]
    /\ retxUsed \in [Clients -> SUBSET (1..MaxSeq)]

(***************************************************************************)
(* Safety invariants (checked by TLC)                                       *)
(***************************************************************************)

\* No batch (client,cseq) is committed to the GOI twice.  This is also the
\* exactly-once guarantee across session fencing/re-open (with EnableReopen).
NoDupCommit ==
    \A i, j \in 1..Len(goi) :
        (i # j) => ~(goi[i].client = goi[j].client /\ goi[i].cseq = goi[j].cseq)

\* THE CORE GUARANTEE.  Each session's committed GOI entries are a strictly-
\* increasing, contiguous-from-1 submission-order prefix (never reordered).
PerSessionPrefix ==
    \A c \in Clients :
        LET idx == GoiIdx(c)
        IN /\ \A i, j \in idx : (i < j) => goi[i].cseq < goi[j].cseq
           /\ CommittedSeqs(c) = 1..Cardinality(idx)

\* Late / duplicate arrivals rejected, never reordered: every committed entry is
\* below its session's current expected pointer.
LateNeverReordered ==
    \A c \in Clients : \A j \in GoiIdx(c) : goi[j].cseq < expectedSeq[c]

\* Once a session is fenced (and not re-opened), no batch beyond its captured
\* high-water mark appears in the log.
FencedSuffixNeverCommitted ==
    \A c \in Clients :
        sessionFenced[c] => \A j \in GoiIdx(c) : goi[j].cseq <= fenceHWM[c]

\* Spatial guard / wrap-fence.
NoWrap == (Len(goi) - replicationFloor) <= GOICap

\* ACKed iff committed (D5).  Every client-visible ACK is a committed, durable,
\* live batch.  The stale-CV bug (D2/W2#5) violates this when the broker skips the
\* ACK-relay epoch check.
AckedIffCommitted ==
    \A c \in Clients : \A s \in acked[c] : IsDurableLive(c, s)

\* READER AGREEMENT / SOLE DIVERGENCE (D5, theorem clause 3).  Any position the
\* speculative (ACK1) reader delivered that has since become durable agrees with
\* the durable order on identity (client,cseq) --- i.e. order/positions never
\* reorder.  The only thing left unconstrained is payload visibility (`lost`):
\* that is the sole permitted divergence.  (Speculative reads in a zone later
\* truncated by failover are epoch-scoped and re-read; see SequencerFailover.)
ReaderAgreement ==
    \A k \in 1..Len(specSnap) :
        (k <= replicationFloor) =>
            /\ k <= Len(goi)
            /\ goi[k].client = specSnap[k].client
            /\ goi[k].cseq   = specSnap[k].cseq

\* Control-block epoch stays in range / monotone.
EpochBound == cbEpoch \in 0..MaxEpoch

Safety ==
    /\ NoDupCommit
    /\ PerSessionPrefix
    /\ LateNeverReordered
    /\ FencedSuffixNeverCommitted
    /\ NoWrap
    /\ AckedIffCommitted
    /\ ReaderAgreement
    /\ EpochBound

=============================================================================
