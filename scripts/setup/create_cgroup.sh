#! /bin/bash

function ClearCgroup()
{
	sudo rm -rf /sys/fs/cgroup/embarcadero_cgroup*
}

function CreateCgroup()
{
declare -a cpusets=("0-84" "85-169" "170-254" "255-339" "340-424" "425-509")

# ============= Throttle CPU core ============= 
# Enable cpuset controller
echo "+cpuset" | sudo tee /sys/fs/cgroup/cgroup.subtree_control

for i in {0,1,2,3,4,5}; do
	cgroup_path="/sys/fs/cgroup/embarcadero_cgroup$i"
	cpu_set="${cpusets[$i]}"

	# Create cgroup
	sudo mkdir "$cgroup_path"

	echo "$cpu_set" | sudo tee "$cgroup_path/cpuset.cpus"
	echo "124G" | sudo tee "$cgroup_path//memory.max"

	# ============= Throttle Disk Bandwidth ============= 
	# Enable cpuset controller
	# lsblk to find major:minor number, throttle bandwidth = 10MB/s (10485760 bytes/s)
	echo "253:2 rbps=10485760" | sudo tee "$cgroup_path/io.max"
	echo "253:2 wbps=10485760" | sudo tee "$cgroup_path/io.max"
done
}

#ClearCgroup
CreateCgroup
