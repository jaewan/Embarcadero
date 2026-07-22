#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 OUTPUT_DIRECTORY" >&2
  exit 2
fi

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
output_dir=$(realpath -m "$1")
if [[ -e "$output_dir" ]]; then
  echo "refusing to overwrite existing path: $output_dir" >&2
  exit 2
fi

stage=$(mktemp -d /tmp/anonymous-artifact.XXXXXX)
cleanup() { rm -rf -- "$stage"; }
trap cleanup EXIT
file_list="$stage/files.txt"

# Code artifact only. Raw result trees are intentionally separate: they are
# large, contain machine-local provenance, and require venue-specific hosting.
git -C "$project_root" ls-files | awk '
  /^(\.clang-format|\.editorconfig|\.gitattributes|\.gitignore|ARCHITECTURE.md|CMakeLists.txt|CONTRIBUTING.md|README.md|perf_run.sh)$/ { print; next }
  /^(benchmarks|config|PaperScripts|sessions|src|test)\// { print; next }
  /^scripts\// && !/^scripts\/network-emulation\/build\// &&
    !/^scripts\/publication\/build_anonymous_artifact\.sh$/ { print; next }
  /^spec\// && !/^spec\/results\// { print; next }
  /^docs\/(contracts|baselines|artifacts)\// { print; next }
  /^docs\/design\/protocol_spec\.md$/ { print; next }
' > "$file_list"

mkdir -p "$stage/artifact"
tar -C "$project_root" -cf - -T "$file_list" | tar -C "$stage/artifact" -xf -

# Replace machine-local defaults with explicit placeholders. The experiment
# scripts already expose these values as environment/configuration knobs.
while IFS= read -r -d '' file; do
  if grep -Iq . "$file"; then
    perl -pi -e 's#/home/domin/Embarcadero#/opt/embarcadero#g;
                 s#/home/domin#/home/anonymous#g;
                 s/moscxl/broker-host/g;
                 s/\bjaewan\b/anonymous/g;
                 s/\bdomin\b/anonymous/g;
                 s/10\.10\.10\.(\d+)/192.0.2.$1/g;
                 s/10\.10\.10\.x/192.0.2.x/g' "$file"
  fi
done < <(find "$stage/artifact" -type f -print0)

deny='(/home/domin|moscxl|10[.]10[.]10[.]|jaewan|file://|vscode://)'
if LC_ALL=C grep -aERn "$deny" "$stage/artifact"; then
  echo "artifact contains a local identity/path marker" >&2
  exit 1
fi

cat > "$stage/artifact/ANONYMOUS_ARTIFACT.md" <<'EOF'
# Embarcadero anonymous code artifact

This package contains the implementation, benchmarks, tests, paper-figure
scripts, and TLA+ sources from the review snapshot. It deliberately excludes
Git metadata, developer notes, paper drafts, build products, and the 35 GiB raw
result tree. Machine-local paths, hostnames, and private testbed addresses are
replaced with `/opt/embarcadero`, `broker-host`, and RFC 5737 addresses; set the
documented environment variables for the evaluation testbed.

The paper's compact result tables and figures are in the anonymous paper source
package. Full raw traces should be uploaded separately through the venue's
anonymous artifact service rather than embedded in the submission archive.
EOF

mkdir -p "$output_dir"
(
  cd "$stage/artifact"
  LC_ALL=C find . -type f -print | LC_ALL=C sort > FILES.txt
  zip -q -r "$output_dir/Embarcadero-anonymous-artifact.zip" .
)
sha256sum "$output_dir/Embarcadero-anonymous-artifact.zip" > "$output_dir/SHA256SUMS"
echo "$output_dir"
