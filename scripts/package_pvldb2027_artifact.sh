#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 OUTPUT.zip" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=$(realpath -m "$1")
if [[ -e "$output" ]]; then
  echo "refusing to overwrite existing path: $output" >&2
  exit 2
fi
if [[ "$output" != *.zip ]]; then
  echo "output must end in .zip" >&2
  exit 2
fi

stage=$(mktemp -d /tmp/embarcadero-pvldb-artifact.XXXXXX)
cleanup() { rm -rf -- "$stage"; }
trap cleanup EXIT
bundle="$stage/Embarcadero-PVLDB2027-artifact"
mkdir -p "$bundle/source" "$bundle/paper-data"

source_commit=cd62a6bcbfd8618210641c5841dc4017845e84c8
if ! git -C "$repo_root" cat-file -e "$source_commit^{commit}"; then
  echo "evaluated source commit is unavailable: $source_commit" >&2
  exit 1
fi

git -C "$repo_root" archive "$source_commit" | tar -C "$bundle/source" -xf -
cp "$repo_root/ARTIFACT.md" "$bundle/README.md"
printf '%s\n' "$source_commit" > "$bundle/SOURCE_COMMIT"

copy_file() {
  local source=$1
  local destination=$2
  if [[ ! -f "$repo_root/$source" ]]; then
    echo "missing curated artifact input: $source" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$bundle/paper-data/$destination")"
  cp "$repo_root/$source" "$bundle/paper-data/$destination"
}

copy_tree() {
  local source=$1
  local destination=$2
  if [[ ! -d "$repo_root/$source" ]]; then
    echo "missing curated artifact directory: $source" >&2
    exit 1
  fi
  mkdir -p "$bundle/paper-data/$destination"
  cp -a "$repo_root/$source/." "$bundle/paper-data/$destination/"
}

copy_file data/paper_eval/fig1/fig1_regime_bars_c1bca74b_selected.csv \
  throughput/fig2_selected.csv
copy_file data/paper_eval/fig1/fig1_regime_bars_c1bca74b_manifest.json \
  throughput/fig2_manifest.json
copy_tree data/paper_eval/fig1/fig1_ordering_path_n4_cb6bb340 \
  throughput/ordering-path
copy_tree data/paper_eval/fig1/fig1_batch_sensitivity_v2_prefault \
  throughput/batch-sensitivity
copy_file data/paper_eval/fig1/fig1_path_decomp/results.csv \
  throughput/path-decomposition/results.csv
copy_file data/paper_eval/fig1/fig1_path_decomp/path_ablation_summary.csv \
  throughput/path-decomposition/summary.csv
copy_file data/paper_eval/fig1/fig1_path_decomp/path_ablation_manifest.json \
  throughput/path-decomposition/manifest.json

for item in \
  Curation:CURATION.md \
  Contract:campaign_contract.md \
  FigureManifest:figure_manifest.json \
  PrimaryManifest:primary_manifest.json \
  PrimarySummary:primary_summary.csv \
  Results:results.csv; do
  destination=${item%%:*}
  source=${item#*:}
  copy_file "data/paper_eval/fig2/fig2_append_latency_clean_ad8a064f/$source" \
    "latency/primary/$destination-${source}"
done
for item in campaign_contract.md mechanism_epoch_manifest.json \
  mechanism_epoch_summary.csv results.csv; do
  copy_file "data/paper_eval/fig2/fig2_mechanism_epoch_clean_fd1a36ce/$item" \
    "latency/mechanism/$item"
done
for baseline in scalog corfu; do
  if [[ "$baseline" == scalog ]]; then
    baseline_dir=fig2_scalog_official_3eaadffb
  else
    baseline_dir=fig2_corfu_official_3eaadffb
  fi
  for item in baseline_manifest.json campaign_contract.md results.csv; do
    copy_file "data/paper_eval/fig2/$baseline_dir/$item" \
      "latency/$baseline/$item"
  done
done

copy_tree data/paper_eval/fig3/fig3_failure_official_8351459f failure
copy_tree data/smr_fifo_paperscale smr-fifo
copy_tree data/paper_eval/cas/cas_skew_panelA_3trial cas-skew
copy_file data/latency/slow_replica/paper_stage_rows.csv \
  slow-replica/paper_stage_rows.csv
copy_file data/latency/slow_replica/paper_summary.json \
  slow-replica/paper_summary.json

(
  cd "$bundle"
  find . -type f ! -name SHA256SUMS -print0 |
    LC_ALL=C sort -z |
    xargs -0 sha256sum > SHA256SUMS
)
mkdir -p "$(dirname "$output")"
(
  cd "$stage"
  zip -q -r "$output" Embarcadero-PVLDB2027-artifact
)
echo "$output"
