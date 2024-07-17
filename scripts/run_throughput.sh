#!/bin/bash

pushd ../build/bin/

# ============= Run Embarlets in cgroup ============= 
# cgexec is not supported in cgroupv2. Thus we first run the program and attach it to the cgroup
#for i in {0,1,2,3}; do
for i in {0,1}; do
	PROGRAM=embarlet
	if [$i == 0];then
		./$PROGRAM --head &
	else
		./$PROGRAM --follower="localhost:12140"&
	fi

	echo "/sys/fs/cgroup/embarcadero_cgroup$i/cgroup.procs"
	
	PID=$!
	#PID=$(pidof $PROGRAM)
	echo $PID | sudo tee "/sys/fs/cgroup/embarcadero_cgroup$i/cgroup.procs"
done
