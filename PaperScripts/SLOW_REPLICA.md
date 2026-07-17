# Slow-Replica Heterogeneity Experiment

## Scientific Claim

> **Appendix Table — "Slow replica stalls ordering":**
> No for Embarcadero, Yes for Scalog/LazyLog.

Embarcadero separates _ordering_ (ACK1: sequencer assigns a logical offset to a
batch) from _durability_ (ACK2: all replicas confirm the payload is persisted).
A slow or stopped replica can delay ACK2 (the durable prefix advances only when
all replicas report completion) but must **not** delay ACK1 (the sequencer
advances independently).

In Scalog the global cut is the element-wise minimum over all shards' local
sequence numbers.  A stalled replica cannot advance its local sequence number,
so the global cut — and therefore both ACK1 and ACK2 — stalls.

---

## Experimental Setup

| Parameter | Value |
|---|---|
| Sequencer | `EMBARCADERO` or `SCALOG` |
| Brokers | 4 (broker 0 = head/sequencer) |
| Slow replica | Broker 1 (first follower), SIGSTOP via `kill -STOP` |
| Pause after start | `INJECT_AFTER_SEC=2` seconds |
| Pause duration | `PAUSE_SEC=4` seconds |
| Replication factor | 2 |
| Message size | 1024 bytes |
| Total data | 1 GiB |

### Why broker 1?

Broker 0 is the head/sequencer.  SIGSTOP on broker 1 (first follower) stalls:
- **Embarcadero**: broker 1's CXL→DRAM replication copy and completion-vector
  update.  Broker 0's sequencer continues assigning logical offsets (ACK1 path
  is unaffected).  ACK2 (durable prefix) waits on broker 1 → stalls.
- **Scalog**: broker 1's progress report to the global sequencer.  The global
  cut uses `element_wise_minimum`, so broker 1's stall holds back the global
  ordered sequence number → both ACK1 and ACK2 stall.

### ACK level note

`publisher.cc` only emits `append_send_to_ordered` (ACK1 metric) when
`ack_level == 1`, and only emits `append_send_to_ack` (ACK2 metric) when
`ack_level >= 2`.  The script therefore runs **two sub-trials per mode**: one
at ACK=1 to capture the ordering latency, and one at ACK=2 to capture the
durable-ack latency.

---

## How to Run

```bash
# Embarcadero (default)
bash scripts/run_slow_replica_heterogeneity.sh

# CXL-Scalog comparison
SEQUENCER=SCALOG bash scripts/run_slow_replica_heterogeneity.sh
```

Output is written to `data/latency/slow_replica/`:

```
data/latency/slow_replica/
  slow_replica_comparison.csv     # machine-readable result
  EMBARCADERO/
    baseline/
      stage_latency_summary_ack1.csv   # ACK1 sub-trial
      stage_latency_summary_ack2.csv   # ACK2 sub-trial
    slow_injected/
      stage_latency_summary_ack1.csv
      stage_latency_summary_ack2.csv
  SCALOG/   (if run with SEQUENCER=SCALOG)
    ...
```

### Expected result

```
System        | Mode           | ACK1-P99us | ACK2-P99us | ACK1-delta% | ACK2-delta%
EMBARCADERO   | baseline       |        ~50 |       ~200 | ---         | ---
EMBARCADERO   | slow_injected  |        ~50 |     >>1000 | ~0%         | >>100%
SCALOG        | baseline       |        ~50 |       ~200 | ---         | ---
SCALOG        | slow_injected  |     >>1000 |     >>1000 | >>100%      | >>100%
```

- ACK1 delta ≈ 0% for Embarcadero validates "ordering unaffected by follower stall."
- ACK1 delta >> 0% for Scalog validates "follower stall blocks global cut → ordering stalls."
- ACK2 delta >> 0% for both is expected and confirms the measurement is live.

---

## Caveat

SIGSTOP on a broker process stalls **all threads**, including the primary
ingestion path.  A more precise measurement would pause only the replica sink
thread inside the follower broker.  The current approach is a **conservative
test**: if ACK1 is unaffected even when the _entire_ follower broker is stopped,
the claim holds _a fortiori_ — in a real slow-replica scenario only the replica
sink would lag, leaving the ingestion path free.

---

## Relationship to Appendix Table

| System | ACK1 stalls on slow replica? | ACK2 stalls on slow replica? |
|---|---|---|
| Embarcadero | **No** (sequencer independent) | Yes (durable prefix waits) |
| Scalog | **Yes** (global cut blocked) | Yes |
| LazyLog | Yes (similar to Scalog) | Yes |

This script measures the Embarcadero and Scalog rows directly.
