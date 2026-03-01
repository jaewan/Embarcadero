#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

OUTDIR=${OUTDIR:-data/latency/repro_manifest}
mkdir -p "$OUTDIR"

MANIFEST="$OUTDIR/manifest.txt"
COMMANDS="$OUTDIR/commands.sh"
HASHES="$OUTDIR/script_hashes.txt"
RERUN="$OUTDIR/RERUN.md"
FILES="$OUTDIR/files_snapshot.txt"
SNAPSHOT_DIR="$OUTDIR/snapshot"
mkdir -p "$SNAPSHOT_DIR/scripts/plot" "$SNAPSHOT_DIR/config" "$SNAPSHOT_DIR/docs"

{
  echo "generated_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_commit=$(git rev-parse HEAD)"
  echo "git_branch=$(git rev-parse --abbrev-ref HEAD)"
  echo "git_status_short_begin"
  git status --short
  echo "git_status_short_end"
  echo "cmake_cache_collect_latency=$(grep -E '^COLLECT_LATENCY_STATS:' build/CMakeCache.txt 2>/dev/null || true)"
} > "$MANIFEST"

cat > "$COMMANDS" <<'EOF'
#!/bin/bash
set -euo pipefail

# Build
cmake -S . -B build -DCOLLECT_LATENCY_STATS=ON
cmake --build build -j

# Step 6 (ORDER=5 anomaly checks)
scripts/run_order5_anomaly_checks.sh

# Step 7 (Scalog deferred in current scope)
MATRIX="EMBARCADERO:0:1:none:unordered_visible_ack EMBARCADERO:5:1:none:per_client_fifo_ack CORFU:2:1:chain:total_order_ack" \
  scripts/run_baseline_matrix.sh

# Step 8 (validated stable frontier points in this environment)
TRIALS_PER_POINT=5 SWEEP_TARGETS="1000 1500 2000" TOTAL_BYTES=4294967296 ORDER=5 ACK=1 SEQUENCER=EMBARCADERO CONFIG=config/embarcadero.yaml CLIENT_CONFIG=config/client.yaml POINT_MAX_ATTEMPTS=2 SWEEP_TIMEOUT_CAP_SEC=180 \
  scripts/run_paper_latency_load_sweep.sh

# Step 9 (Embarcadero canonical ordering ladder)
SEQUENCER=EMBARCADERO ORDERS="0 5" ACK_LEVELS="0 1 2" TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 NUM_BROKERS=4 TEST_TYPE=5 THREADS_PER_BROKER=4 \
  scripts/run_ordering_durability_ladder.sh

# Step 10 (baseline + injected slow replica)
OUTDIR=data/latency/slow_replica_step10_embarcadero SEQUENCER=EMBARCADERO ORDER=5 ACK=1 NUM_BROKERS=4 MESSAGE_SIZE=1024 TOTAL_MESSAGE_SIZE=1073741824 TEST_TYPE=2 THREADS_PER_BROKER=4 SLOW_BROKER_INDEX=3 INJECT_AFTER_SEC=2 PAUSE_SEC=4 POINT_MAX_ATTEMPTS=2 \
  scripts/run_slow_replica_heterogeneity.sh
EOF

chmod +x "$COMMANDS"

{
  sha256sum scripts/run_order5_anomaly_checks.sh
  sha256sum scripts/run_baseline_matrix.sh
  sha256sum scripts/run_paper_latency_load_sweep.sh
  sha256sum scripts/run_throughput_latency_sweep.sh
  sha256sum scripts/run_ordering_durability_ladder.sh
  sha256sum scripts/run_slow_replica_heterogeneity.sh
  sha256sum scripts/freeze_repro_package.sh
  sha256sum scripts/plot/aggregate_latency_sweep_ci.py
  sha256sum scripts/plot/plot_throughput_latency.py
  sha256sum config/embarcadero.yaml
  sha256sum config/client.yaml
  sha256sum docs/LATENCY_EXPERIMENT_CHECKLIST.md
} > "$HASHES"

cp scripts/run_order5_anomaly_checks.sh "$SNAPSHOT_DIR/scripts/"
cp scripts/run_baseline_matrix.sh "$SNAPSHOT_DIR/scripts/"
cp scripts/run_paper_latency_load_sweep.sh "$SNAPSHOT_DIR/scripts/"
cp scripts/run_throughput_latency_sweep.sh "$SNAPSHOT_DIR/scripts/"
cp scripts/run_ordering_durability_ladder.sh "$SNAPSHOT_DIR/scripts/"
cp scripts/run_slow_replica_heterogeneity.sh "$SNAPSHOT_DIR/scripts/"
cp scripts/freeze_repro_package.sh "$SNAPSHOT_DIR/scripts/"
cp scripts/plot/aggregate_latency_sweep_ci.py "$SNAPSHOT_DIR/scripts/plot/"
cp scripts/plot/plot_throughput_latency.py "$SNAPSHOT_DIR/scripts/plot/"
cp config/embarcadero.yaml "$SNAPSHOT_DIR/config/"
cp config/client.yaml "$SNAPSHOT_DIR/config/"
cp docs/LATENCY_EXPERIMENT_CHECKLIST.md "$SNAPSHOT_DIR/docs/"

find "$SNAPSHOT_DIR" -type f | sort > "$FILES"

cat > "$RERUN" <<'EOF'
# Reproduction Guide

## Scope
- Systems: Embarcadero + Corfu (Scalog deferred in current run scope)
- Host assumptions: same machine profile and NUMA layout used by project scripts
- Build flag: `COLLECT_LATENCY_STATS=ON`

## Clean Start
```bash
pkill -9 -f "./embarlet" || true
pkill -9 -f "throughput_test" || true
rm -f /tmp/embarlet_*_ready || true
```

## Build
```bash
cmake -S . -B build -DCOLLECT_LATENCY_STATS=ON
cmake --build build -j
```

## Execute All
```bash
bash data/latency/repro_manifest/commands.sh
```

## Guardrail Sanity
```bash
NUM_BROKERS=4 TEST_TYPE=5 ORDER=0 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 SEQUENCER=EMBARCADERO scripts/run_throughput.sh
NUM_BROKERS=4 TEST_TYPE=5 ORDER=5 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 SEQUENCER=EMBARCADERO scripts/run_throughput.sh
```
EOF

echo "Repro manifest written:"
echo "  $MANIFEST"
echo "  $COMMANDS"
echo "  $HASHES"
echo "  $RERUN"
echo "  $FILES"
