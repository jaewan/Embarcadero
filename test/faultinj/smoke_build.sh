#!/usr/bin/env bash
# ============================================================================
# Fast standalone smoke build+run of the in-vitro fault-injection harness.
# Track 04 / W6.
#
# Compiles ONLY the faultinj layer (two .cc + the header seam) with a plain
# compiler invocation — no CMake, no gRPC/folly/mimalloc — so the edit→run loop
# is seconds. It validates: the controller, all three adversaries, the seam
# hooks in common/performance_utils.h, and the CXL::acquire_load read seam.
#
# This is NOT a substitute for the full CMake build on broker, which also:
#   * exercises the x86 intrinsic paths (_mm_clflushopt / _mm_clwb / _mm_prefetch)
#     that are compiled out on arm64, and
#   * confirms embarlet links with -DCXL_FAULT_INJECTION across all its TUs.
#
# Usage:  test/faultinj/smoke_build.sh [iterations]
#         CXX=g++ test/faultinj/smoke_build.sh 200000
# ============================================================================
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # <repo>/test/faultinj
repo="$(cd "$here/../.." && pwd)"                       # up two: test/faultinj -> repo
iters="${1:-50000}"
cxx="${CXX:-c++}"
tmp="$(mktemp -d)"
out="$tmp/faultinj_harness"

# -I test  => resolve "faultinj/cxl_fault_inject.h"; -I src => "common/performance_utils.h"
cflags=(-std=c++17 -O2 -Wall -DCXL_FAULT_INJECTION -I "$repo/src" -I "$repo/test")
libs=()

# --- glog: performance_utils.h includes <glog/logging.h> for a single inline
#     LOG(FATAL) that the harness never calls, so we need the glog HEADER path but
#     link NO glog symbol. Never add -lglog (that would fail on a headers-present/
#     lib-absent box for no reason). If headers are missing, use a tiny stub. ---
if pkg-config --exists libglog 2>/dev/null; then
  # shellcheck disable=SC2207
  cflags+=($(pkg-config --cflags libglog))   # include path only
elif echo '#include <glog/logging.h>' | "$cxx" -E -x c++ - >/dev/null 2>&1; then
  : # header is on the default search path; nothing to add, no library needed
else
  echo "[smoke] glog headers not found — using a local LOG() stub (the harness never logs via glog)"
  mkdir -p "$tmp/glogstub/glog"
  cat > "$tmp/glogstub/glog/logging.h" <<'H'
#pragma once
namespace smoke_glog { struct Sink { template <class T> Sink& operator<<(const T&) { return *this; } }; }
#define LOG(sev) ::smoke_glog::Sink()
H
  cflags+=(-I "$tmp/glogstub")
fi

# --- arch-specific flags ---
#   * x86: _mm_clflushopt / _mm_clwb / _mm_prefetch need target features, so pass
#     -march=native (the full CMake build does the same at CMakeLists.txt).
#   * arm64+clang: performance_utils.h's prefetch_cacheline uses a non-constant
#     __builtin_prefetch locality (valid on x86 via _mm_prefetch); neutralize that
#     one builtin for the local smoke only (unrelated to W6).
case "$(uname -m)" in
  x86_64|amd64)  cflags+=(-march=native) ;;
  arm64|aarch64) cflags+=(-D__builtin_prefetch\(...\)=\(\(void\)0\)) ;;
esac

echo "[smoke] $cxx ${cflags[*]} <2 src> ${libs[*]:-} -> $out"
"$cxx" "${cflags[@]}" \
  "$repo/test/faultinj/cxl_fault_inject.cc" \
  "$repo/test/faultinj/faultinj_harness.cc" \
  ${libs[@]+"${libs[@]}"} -o "$out"

echo "[smoke] running: faultinj_harness $iters"
"$out" "$iters"
echo "[smoke] OK (exit 0) — all adversaries behaved as expected"
