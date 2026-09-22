#!/usr/bin/env bash
# Per-process address layout workaround for GCC TSan on high-ASLR kernels.
# No host sysctls are changed; a race report or unsupported personality fails.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
output=${1:?usage: run_tsan_components.sh NEW_OUTPUT_DIRECTORY}
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
for name in bounded_reservation session_admission single_topic_admission; do
    "${CXX:-g++}" -std=c++17 -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
        -pthread -I"$repo/src" "$repo/test/${name}_test.cc" -latomic -o "$output/$name"
    TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R "$output/$name" \
        > "$output/$name.log" 2>&1
done
