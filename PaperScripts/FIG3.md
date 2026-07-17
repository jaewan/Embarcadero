# Fig 3 — Failure under the prefix-safe contract

## Claim

After killing one of four log servers mid-run, Embarcadero ORDER=5 enforces the
**prefix-safe hold**: gapped sessions stall until repair/reroute (or fence). A
sensitivity panel with holding disabled shows survivors keep making progress
(arrival order), isolating the cost of the contract from fabric/CXL limits.

Paper: `fig:failure_throughput` / `Paper/Figures/failure_combined.pdf`
(Sec 7.3).

## Driver

| Piece | Path |
|-------|------|
| Campaign | `PaperScripts/run_fig3_failure.sh` |
| Cell runner | `scripts/run_failures.sh` |
| Plot | `scripts/publication/plot_failure_combined.py` |

Outputs under `data/paper_eval/fig3/fig3_failure/`.

## Fixed knobs

| Knob | Value |
|------|-------|
| Brokers | 4 |
| Kill | 1 broker, wall-clock **1800 ms** after publish start |
| Bytes / trial | 20 GiB |
| Message size | 1024 B |
| Publisher | remote `c4` (default) |
| Head | `10.10.10.10` |
| CXL zero | `metadata`, size 72 GiB |
| Measure interval | 100 ms ACK windows |
| Panel (a) | `HOLD_MODE=arrival_order` → ORDER=4 |
| Panel (b) | `HOLD_MODE=prefix_safe` → ORDER=5 + long session lease |

## How to run

```bash
# Full two-panel Fig3 (builds+syncs throughput_test, then both panels)
bash PaperScripts/run_fig3_failure.sh

# Contract panel only
SKIP_NOHOLD=1 bash PaperScripts/run_fig3_failure.sh

# Local client smoke
SCENARIO=local TOTAL_MESSAGE_SIZE=$((2*1024*1024*1024)) \
  FAILURE_AFTER_MS=1000 bash PaperScripts/run_fig3_failure.sh
```

Do **not** overlap Fig2 (`/tmp/embarcadero_paper_fig2.lock`) or another Fig3.

## Single-panel debug

```bash
HOLD_MODE=prefix_safe SCENARIO=remote \
  FAILURE_DATA_DIR=data/paper_eval/fig3/debug/prefix_safe \
  bash scripts/run_failures.sh
```

## Notes

- `EMBARCADERO_FAILURE_AFTER_MS` is implemented in `publisher.cc` (wall-clock
  kill). Older binaries ignored it and only used `failure_percentage` — rebuild
  before running.
- RF defaults to 0 / ACK=1 for the striping+hold contract figure (not Fig2 RF2).
- Detection in the prototype is still TCP/gRPC (~660 ms band), not the CXL lease.
