#!/bin/bash
# Dispatch supported profiles before any historical locks, SSH, or cleanup.
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib/experiment_dispatch.sh" || exit 1
if embarcadero_dispatch_supported "$@"; then shift; fi
# Remote-client throughput launcher.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
export THROUGHPUT_SCRIPT_MODE=remote
source "$SCRIPT_DIR/lib/run_throughput_impl.sh"
