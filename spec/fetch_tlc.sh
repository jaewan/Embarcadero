#!/usr/bin/env bash
# Fetch the exact TLC release used for the checked-in reference outputs.
set -euo pipefail

spec_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
jar=${TLA2TOOLS_JAR:-$spec_dir/tla2tools.jar}
url=https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar
expected_sha256=936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88

if [[ -f "$jar" ]]; then
  actual_sha256=$(sha256sum "$jar" | awk '{print $1}')
  if [[ "$actual_sha256" == "$expected_sha256" ]]; then
    echo "$jar"
    exit 0
  fi
  echo "existing TLC JAR has unexpected SHA-256: $jar" >&2
  exit 1
fi

tmp_jar=$(mktemp /tmp/embarcadero-tla2tools.XXXXXX.jar)
cleanup() { rm -f -- "$tmp_jar"; }
trap cleanup EXIT
curl -fL --retry 3 -o "$tmp_jar" "$url"
actual_sha256=$(sha256sum "$tmp_jar" | awk '{print $1}')
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
  echo "TLC JAR SHA-256 mismatch: expected $expected_sha256, got $actual_sha256" >&2
  exit 1
fi
mv "$tmp_jar" "$jar"
trap - EXIT
echo "$jar"
