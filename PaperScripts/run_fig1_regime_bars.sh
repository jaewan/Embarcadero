#!/usr/bin/env bash
# Run the fixed-N=2 throughput-regime matrix on one clean commit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    echo "ERROR: tracked worktree must be clean" >&2
    exit 1
fi

commit="$(git rev-parse HEAD)"
short="$(git rev-parse --short=8 HEAD)"
campaign="${CAMPAIGN_ID:-fig1_regime_bars_${short}}"
out_root="${OUT_ROOT:-$ROOT/data/paper_eval/fig1/$campaign}"
rf0_tag="${RF0_RUN_TAG:-${campaign}_rf0}"
trials="${NUM_TRIALS:-3}"

common=(
    NUM_TRIALS="$trials"
    TOTAL_BYTES=10737418240 # Per publisher (20 GiB aggregate at N=2).
    MSG_SIZE=4096
    EMBARCADERO_CXL_SIZE=274877906944
    BROKER_READY_TIMEOUT_SEC=300
)

if [[ "${RUN_RF2:-1}" == 1 ]]; then
    env "${common[@]}" \
        N_VALUES=2 TARGET_TRIALS="$trials" CAMPAIGN_ID="$campaign" \
        OUT_ROOT="$out_root" SKIP_LAZYLOG=0 SKIP_BASELINES=0 \
        SKIP_SCALOG_LAZYLOG=0 WAIT_FOR_IDLE="${WAIT_FOR_IDLE:-1}" \
        ONLY_CELLS=fig1_embar_o5_disk_n2,fig1_embar_o5_mem_n2,fig1_corfu_o2_disk_n2,fig1_corfu_o2_mem_n2,fig1_scalog_o1_disk_n2,fig1_scalog_o1_mem_n2,fig1_lazylog_o2_disk_n2 \
        bash PaperScripts/run_fig1_throughput_scaling.sh
fi

if [[ "${RUN_RF0:-1}" == 1 ]]; then
    if [[ -e "$out_root/rf0" && "${ALLOW_RF0_OVERWRITE:-0}" != 1 ]]; then
        echo "ERROR: $out_root/rf0 already exists; refusing to overwrite" >&2
        exit 1
    fi
    env "${common[@]}" \
        RUN_TAG="$rf0_tag" OUT_BASE="$out_root/rf0" WARMUP_TRIALS=0 \
        SKIP_BASELINES=0 SKIP_CLUSTER_SETUP="${SKIP_CLUSTER_SETUP:-0}" \
        ONLY_CELLS=e2_embar5_rf0_ack1_n2,e2_embar0_rf0_ack1_n2,e2_corfu_rf0_n2,e2_scalog_rf0_n2 \
        bash PaperScripts/run_overnight_eval.sh
fi

paper_arg=()
if [[ "${PUBLISH_TO_PAPER:-0}" == 1 ]]; then
    paper_arg=(--paper-pdf "$ROOT/Paper/Figures/throughput_scaling.pdf")
fi
python3 PaperScripts/plot_fig1_throughput_scaling.py \
    --rf2-csv "$out_root/results.csv" \
    --rf0-root "$out_root/rf0/multiclient/logs/$rf0_tag" \
    --commit "$commit" \
    --pdf "$out_root/throughput_regimes.pdf" \
    --png "$out_root/throughput_regimes.png" \
    --manifest "$out_root/throughput_regimes_manifest.json" \
    --selected-csv "$out_root/throughput_regimes_selected.csv" \
    "${paper_arg[@]}"

echo "$out_root"
