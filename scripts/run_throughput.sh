#!/bin/bash
# Ensure we run from project root (works when invoked from any directory, e.g. measure_bandwidth_proper.sh)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Throughput test: all brokers are data ingestion (head and followers use same config).
# NOTE: This script is optimized for bandwidth benchmarking. For latency analysis, use run_latency.sh
# which requires recompilation with COLLECT_LATENCY_STATS=ON.
#
# PERFORMANCE VARIABILITY DIAGNOSIS:
# If you see 7-8GB/s on first run but 2GB/s on subsequent runs, this is likely due to:
# 1. Huge page exhaustion between trials (check huge page availability)
# 2. Memory fragmentation causing malloc fallback instead of mmap
# 3. Insufficient EMBAR_STABILIZE_SEC between trials
# Run with EMBAR_STABILIZE_SEC=5 to increase stabilization time.

# Navigate to build/bin directory
if [ ! -d "build/bin" ]; then
    echo "Error: Cannot find build/bin directory (expected $PROJECT_ROOT/build/bin)"
    exit 1
fi
cd build/bin

# Verify this is a performance-optimized build (latency stats disabled)
if [ "${QUIET:-0}" != "1" ]; then
    echo "Checking build configuration..."
    if nm ./throughput_test 2>/dev/null | grep -q "record_results_"; then
        echo "WARNING: Build appears to include latency collection code (COLLECT_LATENCY_STATS=ON)"
        echo "For optimal throughput performance, rebuild with: cmake .. -DCOLLECT_LATENCY_STATS=OFF"
    else
        echo "Build appears optimized for throughput (latency collection disabled)"
    fi
fi

# Use a fixed shared-memory object to avoid flooding /dev/shm/
# Clean up any existing file first to ensure clean state
if [ -z "${EMBARCADERO_CXL_SHM_NAME:-}" ]; then
  export EMBARCADERO_CXL_SHM_NAME="/CXL_SHARED_THROUGHPUT_${UID}"
  shm_unlink "$EMBARCADERO_CXL_SHM_NAME" 2>/dev/null || true
fi
echo "Using EMBARCADERO_CXL_SHM_NAME=${EMBARCADERO_CXL_SHM_NAME}"

# Config for all brokers (relative to build/bin)
HEAD_CONFIG_ARG="--config ../../config/embarcadero.yaml"
CONFIG_ARG="--config ../../config/embarcadero.yaml"

# Cleanup any stale processes/ports from previous runs.
# Hugepages (MAP_HUGETLB) are released when processes exit (or munmap in destructors).
# To avoid "hugepage shortage" on the next run, we wait briefly so the kernel can return
# pages to the pool (see EMBARCADERO_CLEANUP_SETTLE_SEC below).
cleanup() {
  echo "Cleaning up stale brokers and ports..."
  pkill -9 -f "./embarlet" >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  # Clean up any stale ready signal files
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  # Clean up PID files
  rm -f /tmp/embarlet_head_pid 2>/dev/null || true
  # Clean up stale CXL shm files (only ones we created with our UID)
  if [ -n "${EMBARCADERO_CXL_SHM_NAME:-}" ]; then
    if command -v shm_unlink >/dev/null 2>&1; then
      shm_unlink "$EMBARCADERO_CXL_SHM_NAME" 2>/dev/null || true
    else
      rm -f "/dev/shm/$EMBARCADERO_CXL_SHM_NAME" 2>/dev/null || true
    fi
  fi
  # Clean up any old shm files with our UID pattern
  for shm_file in /dev/shm/CXL_SHARED_FILE_$(id -u)_*; do
    if [ -e "$shm_file" ]; then
      rm -f "$shm_file" 2>/dev/null || true
    fi
  done
  # Give processes a moment to exit
  sleep 1
  # Optional: extra settle time so hugepages are back in the pool before the next run.
  # Set EMBARCADERO_CLEANUP_SETTLE_SEC=2 (or higher) if the next run often sees hugepage shortage.
  local settle="${EMBARCADERO_CLEANUP_SETTLE_SEC:-0}"
  if [ "$settle" -gt 0 ] 2>/dev/null; then
    echo "Settling ${settle}s for hugepage pool..."
    sleep "$settle"
  fi
}

cleanup

# PERF OPTIMIZED: Enable hugepages by default for 9GB/s+ performance
# Runtime hugepage allocation with 256MB buffers provides optimal performance
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
# For peak throughput repeatability, default to full startup warmup.
export EMBARCADERO_CXL_ZERO_MODE=${EMBARCADERO_CXL_ZERO_MODE:-full}

# NUMA: CPU on node 1; allow memory on nodes 1 and 2 so mbind(..., node_2) in cxl_manager works.
# Zero-core CXL is on node 2; script must use --membind=1,2 or first-touch after mbind can SIGBUS/OOM.
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
# Client/publisher pinned to node 0 for consistent separation from broker CPUs.
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-numactl --cpunodebind=0 --membind=0}"

# CRITICAL: Check huge page availability before each trial to diagnose variability
# Insufficient huge pages causes fallback to slow malloc, leading to 2GB/s vs 7GB/s variability
check_huge_pages() {
    local hps_kb=$(grep "Hugepagesize:" /proc/meminfo | awk '{print $2}')
    local free_pages=$(grep "HugePages_Free:" /proc/meminfo | awk '{print $2}')
    local free_mb=$((free_pages * hps_kb / 1024))

    echo "Huge pages available: ${free_pages} pages × ${hps_kb}KB = ${free_mb}MB free"

    # Warn if less than 16GB free (needed for optimal performance)
    if [ $free_mb -lt 16384 ]; then
        echo "WARNING: Only ${free_mb}MB huge pages free. Need ~16GB for optimal performance."
        echo "Consider: echo 8192 | sudo tee /proc/sys/vm/nr_hugepages  # for 16GB"
        return 1
    fi
    return 0
}

# Check system memory state to diagnose performance variability
check_memory_state() {
    echo "=== Memory State Check ==="
    grep -E "(HugePages_Total|HugePages_Free|Hugepagesize|MemFree|MemAvailable)" /proc/meminfo | while read line; do
        echo "  $line"
    done
    echo "=== Process Memory ==="
    ps -eo pid,comm,rss,vsz --no-headers | grep -E "(embarlet|throughput_test)" | head -10 || true

    # Check NUMA memory allocation
    echo "=== NUMA Memory Info ==="
    if command -v numactl >/dev/null 2>&1; then
        numactl --show || true
    fi
    if [ -d /sys/devices/system/node ]; then
        echo "NUMA node memory:"
        for node in /sys/devices/system/node/node*; do
            if [ -f "$node/meminfo" ]; then
                local node_id=$(basename "$node" | sed 's/node//')
                local free_kb=$(grep MemFree "$node/meminfo" | awk '{print $4}')
                echo "  Node $node_id: ${free_kb} KB free"
            fi
        done
    fi
    echo "========================"
}

NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-1}
HEAD_READY_TIMEOUT_SEC=${HEAD_READY_TIMEOUT_SEC:-90}
FOLLOWER_READY_TIMEOUT_SEC=${FOLLOWER_READY_TIMEOUT_SEC:-30}
# Use test type 1 (E2E) for validation - includes subscriber and DEBUG_check_order
test_cases=(${TEST_TYPE:-1})
# Use MESSAGE_SIZE environment variable or default to multiple sizes
if [ -n "$MESSAGE_SIZE" ]; then
    msg_sizes=($MESSAGE_SIZE)
else
    #msg_sizes=(128 256 512 1024 4096 16384 65536 262144 1048576)
    msg_sizes=(1024)  # 1KB message size for 8GB test
fi

# Total message size: 8GB = 8589934592 bytes
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-8589934592}


# Change these for Scalog and Corfu
# Order level 0 for unordered, 1 for ordered (not implemented yet), 4 for strong ordering, 5 for batch-level ordering
orders=(${ORDER:-5})
ack=${ACK:-1}
sequencer=${SEQUENCER:-EMBARCADERO}
QUIET=${QUIET:-0}

# Removed wait_for_signal function - using sleep-based timing instead

# Function to start a process and return the actual embarlet process PID via file
start_process() {
  local command=$1
  local pid_file="/tmp/embarlet_head_pid"
  eval "$command" &
  local shell_pid=$!
  sleep 2  # Wait for process to start

  # Find the actual embarlet process PID. 
  # If shell_pid is numactl, actual_pid is its child.
  local actual_pid=$shell_pid
  local child_pid=$(pgrep -P $shell_pid 2>/dev/null | head -1)
  if [ -n "$child_pid" ]; then
    actual_pid=$child_pid
  fi

  echo "Started process: shell_pid=$shell_pid, actual_pid=$actual_pid" >&2
  echo $actual_pid > "$pid_file"
}

# Array to store process IDs (must be declared before start_process uses it)
declare -a pids=()
overall_status=0

# Removed pipe creation - using sleep-based timing instead

# Helper function to wait for broker readiness signal
wait_for_broker_ready() {
  local expected_pid=$1
  local timeout=$2
  local elapsed=0
  local start_time=$(date +%s)

  echo "Waiting for broker to signal readiness (timeout: ${timeout}s, PID: $expected_pid)..."

  # Wait for broker ready file - check for the expected PID first, then fallback
  while [ $elapsed -lt $timeout ]; do
    # Primary check: exact PID match (most common case)
    local ready_file="/tmp/embarlet_${expected_pid}_ready"
    if [ -f "$ready_file" ]; then
      echo "Broker ready! Found ready file: $ready_file (PID: $expected_pid) after ${elapsed}s"

      # Log broker memory usage after startup
      if command -v ps >/dev/null 2>&1; then
        echo "Broker memory usage:"
        ps -p $expected_pid -o pid,comm,rss,vsz,pcpu,pmem --no-headers 2>/dev/null || true
      fi

      rm -f "$ready_file"
      return 0
    fi

    # Fallback: check for descendant processes (when expected_pid is numactl wrapper)
    # Only do expensive process tree search every 2 seconds to reduce overhead
    if [ $((elapsed % 2)) -eq 0 ] && [ $elapsed -ge 2 ]; then
      # Get all ready files created since we started waiting
      local ready_files=$(find /tmp -name "embarlet_*_ready" -newermt "@${start_time}" 2>/dev/null)
      for ready_file in $ready_files; do
        if [ -f "$ready_file" ]; then
          local file_pid=$(basename "$ready_file" | sed 's/embarlet_\([0-9]*\)_ready/\1/')
          # Verify the process is still running
          if kill -0 $file_pid 2>/dev/null; then
            # Check if file_pid is a descendant of expected_pid
            local current_pid=$file_pid
            local found=0
            for i in {1..3}; do  # Max 3 levels up (numactl -> embarlet)
              local ppid=$(ps -o ppid= -p $current_pid 2>/dev/null | tr -d ' ')
              if [ -z "$ppid" ] || [ "$ppid" = "1" ]; then
                break
              fi
              if [ "$ppid" = "$expected_pid" ]; then
                found=1
                break
              fi
              current_pid=$ppid
            done
            if [ "$found" = "1" ]; then
              echo "Broker ready! Found ready file: $ready_file (PID: $file_pid, descendant of $expected_pid) after ${elapsed}s"
              rm -f "$ready_file"
              return 0
            fi
          fi
        fi
      done
    fi

    # Check if process is still running (fail after 5s to allow initialization time)
    if ! kill -0 $expected_pid 2>/dev/null; then
      if [ $elapsed -ge 5 ]; then
        echo "ERROR: Broker process $expected_pid died before signaling readiness (after ${elapsed}s)"
        return 1
      fi
    fi

    sleep 0.1
    elapsed=$(($(date +%s) - start_time))
  done

  echo "ERROR: Broker failed to signal readiness in ${timeout}s"
  echo "Expected PID: $expected_pid"
  echo "Process status: $(ps -p $expected_pid -o pid,comm,state 2>/dev/null || echo 'not found')"
  echo "Ready files found: $(ls /tmp/embarlet_*_ready 2>/dev/null | wc -l)"
  return 1
}

# Wait for multiple brokers to signal readiness (for parallel follower startup)
# Usage: wait_for_all_brokers_ready timeout pid1 pid2 pid3
# Returns 0 when all have ready files, 1 on timeout or if any process dies
wait_for_all_brokers_ready() {
  local timeout=$1
  shift
  local pids=("$@")
  local start_time=$(date +%s)
  local elapsed=0
  local n=${#pids[@]}
  local ready_count=0

  echo "Waiting for $n broker(s) to signal readiness (timeout: ${timeout}s, PIDs: ${pids[*]})..."

  while [ $elapsed -lt $timeout ]; do
    ready_count=0
    local all_alive=1

    for expected_pid in "${pids[@]}"; do
      local ready_file="/tmp/embarlet_${expected_pid}_ready"
      if [ -f "$ready_file" ]; then
        ((ready_count++))
      else
        # Check if process died (only after 5s to allow initialization)
        if ! kill -0 $expected_pid 2>/dev/null && [ $elapsed -ge 5 ]; then
          echo "ERROR: Broker process $expected_pid died before signaling readiness"
          all_alive=0
        fi
      fi
    done

    if [ "$ready_count" = "$n" ]; then
      echo "All $n broker(s) ready after ${elapsed}s"
      for expected_pid in "${pids[@]}"; do
        rm -f "/tmp/embarlet_${expected_pid}_ready" 2>/dev/null || true
      done
      return 0
    fi

    if [ "$all_alive" = "0" ]; then
      return 1
    fi

    sleep 0.1
    elapsed=$(($(date +%s) - start_time))
  done

  echo "ERROR: Not all brokers signaled readiness in ${timeout}s (ready: $ready_count/$n)"
  return 1
}

# Check and optimize system configuration that affects performance
check_system_config() {
	if [ "${QUIET:-0}" = "1" ]; then
		return 0  # Skip system checks in quiet mode
	fi

	echo "=== System Performance Configuration ==="

	# Check and set transparent huge pages
	if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
		local thp=$(cat /sys/kernel/mm/transparent_hugepage/enabled)
		echo "Transparent Huge Pages: $thp"
		if [[ "$thp" != *"always"* ]] || [[ "$thp" == *"[madvise]"* ]]; then
			echo "Setting THP to 'always' for better performance..."
			if echo always > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null; then
				echo "THP set to 'always'"
			elif command -v sudo >/dev/null 2>&1; then
				echo "Trying with sudo..."
				if sudo sh -c 'echo always > /sys/kernel/mm/transparent_hugepage/enabled' 2>/dev/null; then
					echo "THP set to 'always' (with sudo)"
				else
					echo "WARNING: Could not set THP to 'always'. This may impact performance on memory-intensive workloads."
					echo "This is often expected if THP is controlled by kernel boot parameters."
					echo "Manual check: cat /sys/kernel/mm/transparent_hugepage/enabled"
					echo "If still '[madvise]', consider kernel boot parameter: transparent_hugepage=always"
				fi
			else
				echo "WARNING: Could not set THP to 'always'. This may impact performance on memory-intensive workloads."
				echo "Manual check: cat /sys/kernel/mm/transparent_hugepage/enabled"
			fi
		fi
	fi

	# Check and set CPU governor to performance
	if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
		local governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
		echo "CPU Governor: $governor"
		if [ "$governor" != "performance" ]; then
			echo "Setting CPU governor to 'performance'..."
			if echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null 2>&1; then
				echo "CPU governor set to 'performance'"
				# Verify it was actually set
				sleep 0.1
				local new_governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
				if [ "$new_governor" != "performance" ]; then
					echo "WARNING: Failed to set CPU governor! Still '$new_governor'"
					echo "This requires root privileges. Run: sudo cpupower frequency-set -g performance"
					echo "Or: sudo sh -c 'echo performance > /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'"
				fi
			else
				echo "WARNING: CPU governor is '$governor', not 'performance'! This causes 2-3x performance variability."
				echo "Run: sudo cpupower frequency-set -g performance"
				echo "Or: sudo sh -c 'echo performance > /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'"
			fi
		fi
	fi

	# Check CPU frequency
	if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq ]; then
		local freq_khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
		local freq_mhz=$((freq_khz / 1000))
		echo "CPU0 Frequency: ${freq_mhz} MHz"
		if [ $freq_mhz -lt 2000 ]; then
			echo "WARNING: CPU frequency is very low (${freq_mhz} MHz). Performance governor should help."
		fi
	fi

	# Check if we're running on isolated CPUs
	if [ -f /sys/devices/system/cpu/isolated ]; then
		local isolated=$(cat /sys/devices/system/cpu/isolated)
		echo "Isolated CPUs: $isolated"
		if [ -z "$isolated" ]; then
			echo "WARNING: No CPU isolation detected. Consider isolating CPUs for benchmarking."
		fi
	fi

	# Check for CPU frequency scaling driver
	if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver ]; then
		local driver=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver)
		echo "CPU Freq Driver: $driver"
	fi

	# Check kernel parameters that affect performance
	echo "Kernel Performance Settings:"
	if [ -f /proc/sys/vm/swappiness ]; then
		local swappiness=$(cat /proc/sys/vm/swappiness)
		echo "  vm.swappiness: $swappiness"
		if [ "$swappiness" -gt 10 ]; then
			echo "  Setting vm.swappiness to 10 to reduce swapping..."
			if echo 10 > /proc/sys/vm/swappiness 2>/dev/null; then
				echo "  vm.swappiness set to 10"
			elif command -v sudo >/dev/null 2>&1; then
				if sudo sh -c 'echo 10 > /proc/sys/vm/swappiness' 2>/dev/null; then
					echo "  vm.swappiness set to 10 (with sudo)"
				else
					echo "  WARNING: Could not reduce swappiness. High swappiness ($swappiness) may cause page swapping."
					echo "  Manual fix: sudo sh -c 'echo 10 > /proc/sys/vm/swappiness'"
				fi
			else
				echo "  WARNING: High swappiness ($swappiness) may cause page swapping. Run: sudo sh -c 'echo 10 > /proc/sys/vm/swappiness'"
			fi
		fi
	fi

	if [ -f /proc/sys/vm/dirty_ratio ]; then
		local dirty_ratio=$(cat /proc/sys/vm/dirty_ratio)
		echo "  vm.dirty_ratio: $dirty_ratio"
	fi

	# Check if running on CXL system
	if [ -n "${EMBARCADERO_CXL_SHM_NAME:-}" ]; then
		echo "CXL Memory Configuration:"
		echo "  CXL Shmem Name: $EMBARCADERO_CXL_SHM_NAME"
		# Check if CXL device exists
		if ls /dev/dax* >/dev/null 2>&1; then
			echo "  CXL DAX devices: $(ls /dev/dax* | wc -l) found"
		else
			echo "  Using shared memory (shm) - no DAX devices needed"
		fi
	fi

	echo "==========================================="
}

# Run experiments for each message size
for test_case in "${test_cases[@]}"; do
	for order in "${orders[@]}"; do
		for msg_size in "${msg_sizes[@]}"; do
		for ((trial=1; trial<=NUM_TRIALS; trial++)); do
				echo "Running trial $trial with message size $msg_size"

				# Check system state before each trial to diagnose variability
				check_memory_state
				if ! check_huge_pages; then
					echo "WARNING: Insufficient huge pages may cause performance variability"
				fi

				# Check and optimize system configuration that affects performance
				if ! check_system_config; then
					echo "CRITICAL: System configuration issues detected. Performance will be inconsistent."
					echo "Please fix the issues above and re-run the test."
					continue
				fi

			# Start the processes
			# [[FIX]]: Use start_process so we get actual embarlet PID → fast ready check via /tmp/embarlet_${pid}_ready
			pid_file="/tmp/embarlet_head_pid"
			start_process "$EMBARLET_NUMA_BIND ./embarlet $HEAD_CONFIG_ARG --head --$sequencer > broker_0_trial${trial}.log 2>&1"
			head_pid=$(cat "$pid_file" 2>/dev/null | tr -d '\n ' || echo "")
			rm -f "$pid_file"
				pids=()
				pids+=($head_pid)
				echo "Started head broker with PID $head_pid"

				# Wait for head broker to signal readiness (cold CXL setup can take 30–60s).
				if ! wait_for_broker_ready "$head_pid" "$HEAD_READY_TIMEOUT_SEC"; then
					echo "Head broker failed to initialize, aborting trial"
					overall_status=1
					# Kill all processes and skip to next trial
					for pid in "${pids[@]}"; do
						kill $pid 2>/dev/null || true
					done
					pids=()
					sleep 1
					cleanup
					continue
				fi
				
				# Start follower brokers in parallel (saves ~4–6s vs sequential)
				broker_shell_pids=()
				for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
				  $EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG > broker_${i}_trial${trial}.log 2>&1 &
				  broker_shell_pids+=($!)
				done
				sleep 0.2
				# Track spawned PIDs directly; avoids stale-process mismatches from global pgrep scans.
				follower_pids=()
				for broker_shell_pid in "${broker_shell_pids[@]}"; do
				  follower_pids+=($broker_shell_pid)
				  pids+=($broker_shell_pid)
				done
				echo "Started follower brokers with PIDs: ${follower_pids[*]}"
				if ! wait_for_all_brokers_ready "$FOLLOWER_READY_TIMEOUT_SEC" "${follower_pids[@]}"; then
				  echo "One or more followers failed to initialize, aborting trial"
				  overall_status=1
				  for pid in "${pids[@]}"; do kill $pid 2>/dev/null || true; done
				  pids=()
				  sleep 1
				  cleanup
				  continue 2
				fi
				echo "All brokers ready, cluster formed"

				# Run throughput test in foreground; stream output to terminal.
				# Client process is bound to node 0 via CLIENT_NUMA_BIND.
				# Total message size: 8GB (8589934592 bytes) for bandwidth measurement
				# Longer ACK timeout when ack=1 so test can complete (all-ingestion or backpressure can delay ACKs).
				# [[Phase B]] Order 5 has B0 tail-ACK convergence issues; use 180s for order=5, 120s otherwise.
				if [ "$ack" = "1" ]; then
					if [ "$order" = "5" ]; then
						export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-180}"
					else
						export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-120}"
					fi
				fi
				# Threads per broker: use 1 when NUM_BROKERS=1; otherwise 4 for 10+ GB/s (was 3; 4×4=16 threads saturates better).
				THREADS_PER_BROKER=${THREADS_PER_BROKER:-$([ "$NUM_BROKERS" = "1" ] && echo 1 || echo 4)}

				# [[PERF: Removed --record_results]] Latency collection adds overhead and is only needed for research analysis, not bandwidth benchmarks
				echo "=== Starting Throughput Test Trial $trial ==="
				test_start=$(date +%s.%N)
				stdbuf -oL -eL $CLIENT_NUMA_BIND ./throughput_test --config ../../config/client.yaml -n $THREADS_PER_BROKER -m $msg_size -s $TOTAL_MESSAGE_SIZE -t $test_case -o $order -a $ack --sequencer $sequencer -l 0 -r 0 2>&1 | tee "throughput_test_trial${trial}.log"
			test_exit_code=${PIPESTATUS[0]}
			test_end=$(date +%s.%N)
			test_duration=$(echo "$test_end - $test_start" | bc 2>/dev/null || echo "0")
			echo "=== Throughput Test Trial $trial Completed in ${test_duration}s ==="
			test_exit_code=${PIPESTATUS[0]}
			if [ $test_exit_code -ne 0 ]; then
				echo "ERROR: Throughput test failed with exit code $test_exit_code"
				overall_status=1
				# Still clean up brokers even if test failed
			fi
			# Test completed - graceful shutdown so brokers can drain (EpochSequencerThread, etc.)
			echo "Test completed, sending SIGTERM to broker processes for graceful shutdown..."
			for pid in "${pids[@]}"; do
				kill -TERM "$pid" 2>/dev/null || true
			done
			# Wait up to 10s for brokers to exit (drain can take ~3s, plus cleanup)
			for i in 1 2 3 4 5 6 7 8 9 10; do
				all_gone=true
				for pid in "${pids[@]}"; do
					kill -0 "$pid" 2>/dev/null && all_gone=false || true
				done
				$all_gone && break
				sleep 1
			done
			for pid in "${pids[@]}"; do
				if kill -0 "$pid" 2>/dev/null; then
					echo "Broker PID $pid did not exit in 10s, sending SIGKILL"
					kill -9 "$pid" 2>/dev/null || true
				else
					echo "Broker PID $pid exited gracefully"
				fi
			done

			echo "All processes have finished for trial $trial with message size $msg_size"

			# Clear pids array and save broker logs for inspection (especially on subscribe timeout)
			pids=()
			mkdir -p "../../data/throughput/logs"
			log_subdir="../../data/throughput/logs/trial_${trial}_$(date +%Y%m%d_%H%M%S)"
			mkdir -p "$log_subdir"
			mv broker_*_trial${trial}.log "$log_subdir"/ 2>/dev/null || true

			# Allow system to stabilize between trials (memory cleanup, huge page return to pool)
			echo "Waiting 3 seconds for system stabilization..."
			sleep 3
			cleanup

			# Additional delay to ensure huge pages are back in pool
			stabilize_sec="${EMBAR_STABILIZE_SEC:-2}"
			if [ "${stabilize_sec:-0}" -gt 0 ] 2>/dev/null; then
				echo "Stabilizing ${stabilize_sec}s for hugepage pool..."
				sleep "$stabilize_sec"
			fi
		done
		done
	done
done

echo "All experiments have finished."
exit $overall_status
