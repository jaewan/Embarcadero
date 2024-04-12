#!/bin/bash

set -ex

MY_CGROUP=eh-test2
CHILD_CGROUPS="broker0 broker1 broker2 broker3 pub0 pub1"

# Delete the child cgroups
for child_cgroup in $CHILD_CGROUPS; do 
	sudo rmdir /sys/fs/cgroup/$MY_CGROUP/$child_cgroup
done

# Delete the parent groups
sudo rmdir /sys/fs/cgroup/$MY_CGROUP


