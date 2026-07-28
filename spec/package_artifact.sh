#!/usr/bin/env bash
# Build a self-contained, path-sanitized TLA+ artifact.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 OUTPUT_ZIP" >&2
  exit 2
fi

spec_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$spec_dir/.." && pwd)
output_zip=$(realpath -m "$1")
if [[ -e "$output_zip" ]]; then
  echo "refusing to overwrite existing path: $output_zip" >&2
  exit 2
fi

stage=$(mktemp -d /tmp/embarcadero-tla.XXXXXX)
cleanup() { rm -rf -- "$stage"; }
trap cleanup EXIT
artifact="$stage/Embarcadero-TLA"
mkdir -p "$artifact/results"

for path in "$spec_dir"/*.tla "$spec_dir"/*.cfg \
            "$spec_dir/README.md" "$spec_dir/run_all.sh"; do
  [[ -f "$path" ]] || {
    echo "missing TLA+ artifact input: $path" >&2
    exit 1
  }
  cp "$path" "$artifact/"
done
cp "$spec_dir/results/SUMMARY.md" "$artifact/results/"

# Reference TLC outputs contain the absolute checkout used for the original
# campaign. Preserve all model-checking evidence while normalizing only those
# non-semantic "Parsing file" lines.
for path in "$spec_dir"/results/*.txt; do
  name=$(basename "$path")
  sed -E 's#^(Parsing file) .*/([^/]+[.]tla)$#\1 \2#' \
    "$path" > "$artifact/results/$name"
done

chmod +x "$artifact/run_all.sh"
(
  cd "$artifact"
  sha256sum ./*.tla ./*.cfg README.md run_all.sh results/* \
    > SHA256SUMS
)

deny='(/home/|/Users/|moscxl|10[.]10[.]10[.]|file://|vscode://)'
if LC_ALL=C grep -aERn "$deny" "$artifact"; then
  echo "TLA+ artifact contains a local path, host, or private-network marker" >&2
  exit 1
fi
grep -Fq 'TLC2 Version 2.19' "$artifact/results/core.txt"
grep -Fq 'Model checking completed. No error has been found.' \
  "$artifact/results/core.txt"
grep -Fq 'Invariant Safety is violated' \
  "$artifact/results/stale_cv_bug_demo.txt"

mkdir -p "$(dirname "$output_zip")"
(
  cd "$stage"
  zip -q -r "$output_zip" Embarcadero-TLA
)
echo "$output_zip"
