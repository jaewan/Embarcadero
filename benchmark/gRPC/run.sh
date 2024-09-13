#!/bin/bash

ulimit -n 16384

echo "Running Server, binding to $1"
go run server/main.go "$1" &
SERVER_PID=$!
echo "Server started with PID ${SERVER_PID}"

sleep 1

echo "Running Client connecting to $1"

echo "Running 1KB Benchmarks"
go run client/main.go "$1" 131072 100 10 1 1 1
sleep 1

# kill -9 "${SERVER_PID}"
# pkill main