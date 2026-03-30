# Sourced by test/e2e/*.sh — sets NUMA_BIND when unset.
# Use membind 1,2 only if NUMA node 2 exists (avoids libnuma errors on 2-node hosts).
if [[ -z "${NUMA_BIND:-}" ]]; then
	_e2e_mb="1"
	if command -v numactl >/dev/null 2>&1 && numactl -H 2>/dev/null | grep -qE '^node 2 cpus:'; then
		_e2e_mb="1,2"
	fi
	NUMA_BIND="numactl --cpunodebind=1 --membind=${_e2e_mb}"
	unset _e2e_mb
fi
