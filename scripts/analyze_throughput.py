#!/usr/bin/env python3
"""
Analyze throughput bottlenecks from Embarcadero logs.

Usage:
    python3 scripts/analyze_throughput.py <log_file>

Extracts timing information from logs and computes:
- Time spent in each pipeline stage
- Bottleneck identification
- Per-component latency breakdown
"""

import sys
import re
from collections import defaultdict
from datetime import datetime

def parse_timestamp(line):
    """Extract timestamp from glog format: I20260128 09:59:02.123456"""
    match = re.match(r'[IWEF](\d{8})\s+(\d{2}):(\d{2}):(\d{2})\.(\d{6})', line)
    if match:
        date, h, m, s, us = match.groups()
        # Return microseconds since epoch (simplified - just use time within day)
        return int(h) * 3600_000_000 + int(m) * 60_000_000 + int(s) * 1_000_000 + int(us)
    return None

def analyze_log(filename):
    """Analyze log file for throughput bottlenecks."""

    # Track timing events
    publish_times = []
    send_times = []
    receive_times = []
    ordering_times = []
    ack_times = []

    # Track batch processing
    batches_published = 0
    batches_sent = 0
    batches_received = 0
    batches_ordered = 0
    acks_received = 0

    # Track component activity
    last_publish_ts = None
    last_send_ts = None
    last_receive_ts = None
    last_ordering_ts = None
    last_ack_ts = None

    print(f"Analyzing {filename}...")

    with open(filename, 'r') as f:
        for line in f:
            ts = parse_timestamp(line)
            if ts is None:
                continue

            # Track publish activity
            if 'Publish()' in line or 'client_order_' in line:
                if last_publish_ts:
                    publish_times.append(ts - last_publish_ts)
                last_publish_ts = ts
                batches_published += 1

            # Track send activity
            if 'PublishThread' in line and 'sent' in line.lower():
                if last_send_ts:
                    send_times.append(ts - last_send_ts)
                last_send_ts = ts
                batches_sent += 1

            # Track receive activity
            if 'ReqReceiveThread' in line or 'HandlePublishRequest' in line:
                if last_receive_ts:
                    receive_times.append(ts - last_receive_ts)
                last_receive_ts = ts
                batches_received += 1

            # Track ordering activity
            if 'BrokerScannerWorker5' in line or 'AssignOrder5' in line:
                if last_ordering_ts:
                    ordering_times.append(ts - last_ordering_ts)
                last_ordering_ts = ts
                batches_ordered += 1

            # Track ACK activity
            if 'ack_received_' in line or 'ACK' in line:
                if last_ack_ts:
                    ack_times.append(ts - last_ack_ts)
                last_ack_ts = ts
                acks_received += 1

    # Compute statistics
    def stats(times, name):
        if not times:
            print(f"\n{name}: No data")
            return

        avg = sum(times) / len(times)
        sorted_times = sorted(times)
        p50 = sorted_times[len(sorted_times) // 2]
        p95 = sorted_times[int(len(sorted_times) * 0.95)]
        p99 = sorted_times[int(len(sorted_times) * 0.99)]

        print(f"\n{name}:")
        print(f"  Count: {len(times)}")
        print(f"  Avg inter-event time: {avg/1000:.2f} ms")
        print(f"  P50: {p50/1000:.2f} ms")
        print(f"  P95: {p95/1000:.2f} ms")
        print(f"  P99: {p99/1000:.2f} ms")
        print(f"  Throughput: {1_000_000/avg:.2f} events/sec")

    print("\n" + "="*60)
    print("THROUGHPUT ANALYSIS")
    print("="*60)

    stats(publish_times, "Publish")
    stats(send_times, "Network Send")
    stats(receive_times, "Receive")
    stats(ordering_times, "Ordering/Sequencing")
    stats(ack_times, "ACK")

    print("\n" + "="*60)
    print("BATCH COUNTS")
    print("="*60)
    print(f"Batches published: {batches_published}")
    print(f"Batches sent: {batches_sent}")
    print(f"Batches received: {batches_received}")
    print(f"Batches ordered: {batches_ordered}")
    print(f"ACKs received: {acks_received}")

    # Identify bottleneck
    print("\n" + "="*60)
    print("BOTTLENECK ANALYSIS")
    print("="*60)

    if publish_times and send_times:
        if sum(publish_times)/len(publish_times) > sum(send_times)/len(send_times):
            print("⚠ BOTTLENECK: Publisher (slow batching)")
        elif batches_sent < batches_received * 0.9:
            print("⚠ BOTTLENECK: Network Send (congestion)")
        elif batches_ordered < batches_received * 0.9:
            print("⚠ BOTTLENECK: Sequencer (slow ordering)")
        elif acks_received < batches_ordered * 0.9:
            print("⚠ BOTTLENECK: ACK path (slow acknowledgments)")
        else:
            print("✓ Pipeline balanced - check configuration")

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <log_file>")
        sys.exit(1)

    analyze_log(sys.argv[1])
