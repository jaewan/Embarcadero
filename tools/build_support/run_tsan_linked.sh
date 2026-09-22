#!/usr/bin/env bash
# Instrument fetched dependencies and actual linked broker/client fixtures.
# These bounded fixtures do not start a 64 GiB broker cluster.
# The manager-geometry case extends the previous six-fixture suite; its result
# must be recorded separately until the expanded suite has actually run.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${1:?usage: run_tsan_linked.sh BUILD_DIRECTORY [additional CMake arguments]}
shift
jobs=${EMBARCADERO_BUILD_JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'EMBARCADERO_BUILD_JOBS must be positive' >&2; exit 2; }
# Fixed process timezone avoids glog/libc timezone reinitialization noise. Neither
# this nor ADDR_NO_RANDOMIZE changes host-wide settings or suppresses race reports.
export TZ=UTC TSAN_OPTIONS=halt_on_error=1
setarch "$(uname -m)" -R cmake --preset tsan -S "$repo" -B "$build" "$@"
setarch "$(uname -m)" -R cmake --build "$build" -j"$jobs" --target \
    topic_publication_test chain_disk_fault_test order5_publisher_rollover_test \
    configuration_safety_test network_safety_test test_epoch_shutdown
setarch "$(uname -m)" -R ctest --test-dir "$build" --output-on-failure \
    -R '^(unit_cxl_manager_geometry|unit_topic_publication|unit_chain_disk_fault|unit_order5_publisher_rollover|unit_configuration_safety|unit_network_safety|unit_epoch_shutdown)$'
