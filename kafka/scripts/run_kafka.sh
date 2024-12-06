#!/bin/bash

UUID=$(uuidgen)
for i in {0..3}; do
	sudo ip netns exec embarcadero_netns$i /home/domin/Junbong/Embarcadero/kafka/kafka_2.13-3.7.1/bin/kafka-storage.sh format -t $UUID -c /home/domin/Junbong/Embarcadero/kafka/kafka_2.13-3.7.1/config/kraft/server.$i.properties
	sudo ip netns exec embarcadero_netns$i /home/domin/Junbong/Embarcadero/kafka/kafka_2.13-3.7.1/bin/kafka-server-start.sh /home/domin/Junbong/Embarcadero/kafka/kafka_2.13-3.7.1/config/kraft/server.$i.properties &
	sudo ip netns pids embarcadero_netns$i | sudo tee "/sys/fs/cgroup/embarcadero_cgroup$i/cgroup.procs"
done