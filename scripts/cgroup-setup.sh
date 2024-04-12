#!/bin/bash

set -ex

MY_CGROUP=eh-test2
CHILD_CGROUPS="broker0 broker1 broker2 broker3 pub0 pub1"
# On this machine, there are 512 cores. 
# 512 / 6 processes is 85 + 1/3. We will just limit each process to at most 85 cores
# This limit will take effect even if there is idle cores:
# cpu.cfs_quota_us = 100_000
# # 85 * cpu.cfs_quota_us
# cpu.cfs_period_us = 8_500_000 

# Based mostly on this document:
# https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/8/html/managing_monitoring_and_updating_the_kernel/using-cgroups-v2-to-control-distribution-of-cpu-time-for-applications_managing-monitoring-and-updating-the-kernel

# We assume we already have cgroups2 mounted correctly

# Enable CPU-related controllers
echo "+cpu" | sudo tee -a /sys/fs/cgroup/cgroup.subtree_control

# Great the cgroup
sudo mkdir /sys/fs/cgroup/$MY_CGROUP/

# Enable cpu controllers in our new cgroup
echo "+cpu" | sudo tee -a /sys/fs/cgroup/$MY_CGROUP/cgroup.subtree_control

# Create the child cgroups
for child_cgroup in $CHILD_CGROUPS; do 
	sudo mkdir /sys/fs/cgroup/$MY_CGROUP/$child_cgroup

	# enable cpu controller
	echo "+cpu" | sudo tee -a /sys/fs/cgroup/$MY_CGROUP/$child_cgroup/cgroup.subtree_control
	# Set max of 85 CPUs which is approximately 1/6th of the machine
	echo '8500000 100000' | sudo tee -a /sys/fs/cgroup/$MY_CGROUP/$child_cgroup/cpu.max
done


