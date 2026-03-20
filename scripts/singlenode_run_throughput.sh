#!/bin/bash
# Single-node throughput launcher.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
export THROUGHPUT_SCRIPT_MODE=single
source "$SCRIPT_DIR/lib/run_throughput_impl.sh"
