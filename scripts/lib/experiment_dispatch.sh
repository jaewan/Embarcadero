#!/bin/bash
# Source at the beginning of a compatibility launcher, before its old body.
# A zero return means --legacy was selected and the caller must shift once.
# Successful owned routes replace this shell; an empty argv preserves history.
embarcadero_dispatch_supported() {
    local _experiment_profile
    case "${1:-}" in
        --legacy) return 0 ;;
        "") return 1 ;;
        --dev-dram) _experiment_profile=dev ;;
        --workload-dram) _experiment_profile=workload ;;
        --fault-dram) _experiment_profile=fault ;;
        --perf-dram) _experiment_profile=perf ;;
        --legacy-startup) _experiment_profile=legacy-startup ;;
        --help|-h)
            cat <<'HELP'
Supported local routes (arguments are validated by the selected owned runner):
  --dev-dram [dev_cluster.py options]
  --workload-dram {latency|gap|publishers} [experiment_workload.py options]
  --fault-dram [run_production_faults.py options]
  --perf-dram [perf_compare.py options]
  --legacy-startup [check_legacy_startup.py options]
Append --help to any route for its options. Local routes use explicit DRAM
emulation and owned cleanup. Workload variations explicitly select existing
client semantics; aliases alone do not reproduce the historical experiment
named by this shell script, remote topology, or published measurement protocol.

Historical route:
  --legacy [historical arguments]
The original environment-only invocation is also retained. Historical routes
may use SSH, host tuning, and broad cleanup; inspect them on a dedicated host.
See docs/development-commands.md or tools/experiment.py --help.
HELP
            exit 0 ;;
        *)
            echo "Unknown launcher option; use --help, or --legacy for the historical workflow." >&2
            exit 2 ;;
    esac
    shift
    local _experiment_root
    _experiment_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)" || exit 1
    exec python3 "${_experiment_root}/tools/experiment.py" "${_experiment_profile}" "$@"
}
