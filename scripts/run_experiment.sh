#!/bin/bash
# Canonical shell alias; the Python dispatcher and selected runner own behavior.
set -euo pipefail
_experiment_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "${_experiment_root}/tools/experiment.py" "$@"
