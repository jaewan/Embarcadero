#!/bin/bash

# Array of message sizes
msg_sizes=(128 512 1024 4096 65536 1048576)

# Number of trials
trials=10

# Function to run Kafka benchmark and organize result files
run_benchmark() {
    local entity=$1
    local msg_size=$2
    local trial=$3
    
    # Start Kafka
    ../scripts/run_kafka.sh
    sleep 10

    # Create directory to store result files
    mkdir -p ./output/$msg_size

    # Run benchmark and save the result to a file
    while true; do
        sudo ./driver $entity $msg_size >> ./output/$msg_size/$trial.txt
        if [ $? -eq 0 ]; then
            break
        else
            echo "Benchmark failed for $entity with msg_size $msg_size, trial $trial. Retrying..."
            sleep 10
            ../scripts/stop_kafka.sh
            sleep 10
            ../scripts/run_kafka.sh
        fi
    done
    ./pclean.sh

    # Move latency files
    mkdir -p ./latency/$msg_size
    if [ "$entity" == "single" ]; then
        mv ./producer_latency.csv ./latency/$msg_size/producer$trial.csv
    elif [ "$entity" == "end2end" ]; then
        mv ./end2end_latency.csv ./latency/$msg_size/end2end$trial.csv
    fi

    # Stop Kafka
    sleep 10
    ../scripts/stop_kafka.sh
    sleep 30
}

# Run benchmark for all msg_sizes
for ((trial=1; trial<=trials; trial++)); do
    for msg_size in "${msg_sizes[@]}"; do
        run_benchmark "single" $msg_size $trial
        run_benchmark "end2end" $msg_size $trial
    done
done
