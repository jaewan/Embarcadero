#!/usr/bin/env bash
# PaperScripts/run_order5_latency_package.sh — wrapper for publication latency package.
# Latency cells keep linger / RUNTIME_MODE=latency; do not force CLIENT_PUB_BATCH_KB=2048.
set -euo pipefail
PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$PAPER_DIR/.." && pwd)"
exec bash "$ROOT/scripts/publication/run_order5_latency_package.sh" "$@"
