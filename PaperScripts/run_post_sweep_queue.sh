#!/usr/bin/env bash
# PaperScripts/run_post_sweep_queue.sh — thin wrapper around scripts/run_post_sweep_queue.sh
# so paper campaigns stay under one directory. Pass-through all env/args.
set -euo pipefail
PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$PAPER_DIR/.." && pwd)"
exec bash "$ROOT/scripts/run_post_sweep_queue.sh" "$@"
