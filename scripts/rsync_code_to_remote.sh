#!/usr/bin/env bash
# Sync Embarcadero *source* to a remote host, then configure and build the client
# binary there. Do not rsync build/ across machines (different glibc); build on the remote.
#
# Usage:
#   bash scripts/rsync_code_to_remote.sh [SSH_HOST]
#   REMOTE_JOBS=16 bash scripts/rsync_code_to_remote.sh c4
#
# Excludes large or machine-local trees:
#   build/       — always rebuild on remote
#   data/        — experiment outputs
#   .Replication/ — local replication experiment store (can be huge)
#   .cursor/     — editor
#   .git/        — optional; omit from RSYNC_EXCLUDES if you want history on remote
set -euo pipefail

REMOTE_HOST="${1:-c4}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Destination on remote (rsync expands ~ on the *local* side for the remote spec).
REMOTE_SPEC="${REMOTE_EMBARCADERO_SPEC:-${REMOTE_HOST}:~/Embarcadero/}"
REMOTE_JOBS="${REMOTE_JOBS:-$(nproc 2>/dev/null || echo 8)}"

# Default excludes (large or machine-local). Export RSYNC_EXTRA_EXCLUDES="pat1 pat2"
# for more; each token becomes one --exclude.
RSYNC_EXCLUDES=(
  'build/'
  'data/'
  '.Replication/'
  '.cursor/'
  '.git/'
)
excludes=()
for x in "${RSYNC_EXCLUDES[@]}"; do
  excludes+=(--exclude "$x")
done
if [[ -n "${RSYNC_EXTRA_EXCLUDES:-}" ]]; then
  for x in $RSYNC_EXTRA_EXCLUDES; do
    excludes+=(--exclude "$x")
  done
fi

echo "==> rsync $PROJECT_ROOT/ -> $REMOTE_SPEC"
rsync -av --delete "${excludes[@]}" "$PROJECT_ROOT/" "$REMOTE_SPEC"

# Nodes without GitHub access cannot run FetchContent git clone. Seed grpc from this machine's
# build tree (must already exist under build/_deps/grpc-src). Example:
#   RSYNC_GRPC_SRC=1 bash scripts/rsync_code_to_remote.sh c4
if [[ "${RSYNC_GRPC_SRC:-0}" == "1" ]]; then
  if [[ ! -f "$PROJECT_ROOT/build/_deps/grpc-src/CMakeLists.txt" ]]; then
    echo "ERROR: RSYNC_GRPC_SRC=1 but $PROJECT_ROOT/build/_deps/grpc-src is missing; build once on this host." >&2
    exit 1
  fi
  echo "==> rsync grpc-src -> ${REMOTE_HOST}:~/Embarcadero/build/_deps/grpc-src/"
  rsync -av "$PROJECT_ROOT/build/_deps/grpc-src/" "${REMOTE_HOST}:~/Embarcadero/build/_deps/grpc-src/"
fi

echo "==> remote cmake + throughput_test (${REMOTE_JOBS} jobs)"
# Do not rm -rf build by default: wipes FetchContent trees and may force a GitHub clone.
# CLEAN_REMOTE_BUILD=1 only when the remote can reach github.com or you will re-seed grpc-src.
ssh -o BatchMode=yes "$REMOTE_HOST" bash -s -- "$REMOTE_JOBS" "${CLEAN_REMOTE_BUILD:-0}" <<'REMOTE_EOF'
set -euo pipefail
JOBS="$1"
CLEAN="$2"
cd ~/Embarcadero
if [[ "$CLEAN" == "1" ]]; then
  rm -rf build
fi
GRPC_SRC="$HOME/Embarcadero/build/_deps/grpc-src"
CMAKE_EXTRA=(-DCOLLECT_LATENCY_STATS=ON)
if [[ -f "$GRPC_SRC/CMakeLists.txt" ]]; then
  CMAKE_EXTRA+=(-DFETCHCONTENT_SOURCE_DIR_GRPC="$GRPC_SRC")
fi
cmake -S . -B build "${CMAKE_EXTRA[@]}"
cmake --build build -j"$JOBS" --target throughput_test
echo "Remote binary: $HOME/Embarcadero/build/bin/throughput_test"
ldd "$HOME/Embarcadero/build/bin/throughput_test" | grep -E 'not found' && exit 1 || true
REMOTE_EOF

echo "Done. Run from broker host, e.g.:"
echo "  SCENARIO=remote REMOTE_CLIENT_HOST=$REMOTE_HOST BROKER_LISTEN_ADDR=<routable-ip> bash scripts/run_latency.sh"
