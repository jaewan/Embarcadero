# Slow-replica ordering/replication experiment

This experiment tests whether follower persistence lies on the ordering-visible
acknowledgment path. It intentionally delays a replica sync operation without
pausing the broker, sequencer, network receive path, or other replica threads.

## Paper contract

| System | Ordering-visible ACK | Durable ACK | Injection |
|---|---|---|---|
| Embarcadero | ACK1, GOI commit | ACK2, completion-vector advance | 5 s before replica `fdatasync` |
| CXL-Scalog | ACK1, global-cut progress | Not exposed independently by this port | 500 ms before one follower `fdatasync` |

The systems use different injection durations, so the paper compares each
system to its own baseline and does not compare their absolute latency.

LazyLog is not reported. Its paced fault workload did not satisfy the
fail-closed delivery checker; CSVs from those incomplete runs are diagnostic
artifacts, not measurements.

## Run

Embarcadero, three paired baseline/slow trials:

```bash
NUM_TRIALS=3 \
INJECT_SYNC_SLEEP_MS=5000 \
OUTDIR=data/latency/slow_replica \
bash scripts/run_slow_replica_heterogeneity.sh
```

CXL-Scalog, three paired baseline/slow ACK1 trials:

```bash
SEQUENCER=SCALOG \
ORDER=1 \
RUN_ACK2=0 \
NUM_TRIALS=3 \
TARGET_MBPS=250 \
INJECT_SYNC_SLEEP_MS=500 \
SLOW_BROKER_INDEX=1 \
SLOW_SOURCE_BROKER_INDEX=0 \
OUTDIR=data/latency/slow_replica_scalog_ack1_official_806b1809 \
bash scripts/run_slow_replica_heterogeneity.sh
```

The driver fails closed unless the benchmark succeeds, the requested stage CSV
exists, and a slow Scalog cell contains targeted `[SCALOG_SYNC_SLEEP]`
evidence for the configured source/target pair.

## Canonical aggregation

```bash
python3 PaperScripts/build_slow_replica_summary.py \
  --embar-root data/latency/slow_replica \
  --scalog-root data/latency/slow_replica_scalog_ack1_official_806b1809 \
  --csv data/latency/slow_replica/paper_stage_rows.csv \
  --json data/latency/slow_replica/paper_summary.json
```

The aggregator requires exactly three source trials for every reported cell and
records the SHA-256 digest of every stage CSV.

## Validated medians

| System | Metric | Baseline P99 | Slow P99 | Ratio |
|---|---|---:|---:|---:|
| Embarcadero | ACK1 | 1,091 us | 1,126 us | 1.032x |
| Embarcadero | ACK2 | 411,054 us | 10,475,900 us | 25.485x |
| CXL-Scalog | ACK1 | 268,305 us | 1,256,450 us | 4.683x |

Interpretation: Embarcadero's ordering-visible ACK remains within trial noise
while its durable ACK waits for persistence. CXL-Scalog's ordering-visible ACK
inherits the targeted follower-sync delay because the persisted frontier gates
the global cut.
