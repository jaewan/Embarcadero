# Embarcadero Broker Scaling Experiment

This experiment tests Embarcadero's throughput scaling with increasing broker counts while maintaining stable 4Gbps per-broker bandwidth limits.

## 📊 Experiment Overview

- **Broker Counts**: 1, 2, 4, 8, 12, 16, 20
- **Per-Broker Bandwidth**: 4Gbps (proven stable threshold)
- **Total Message Size**: 10GB per test
- **Message Size**: 1KB (consistent across all tests)
- **Dynamic Resource Allocation**: Segment sizes and buffer sizes automatically adjusted based on broker count

## 🚀 Quick Start

### 1. Test Setup (Optional but Recommended)
```bash
# Test the setup with 4 brokers first
./scripts/test_scaling_setup.sh
```

### 2. Run Full Scaling Experiment
```bash
# This will test all broker counts (1,2,4,8,12,16,20)
./scripts/broker_scaling_experiment.sh
```

### 3. Generate Plots
```bash
# Generate throughput scaling plots
./scripts/plot_scaling_results.py

# Or specify custom results file
./scripts/plot_scaling_results.py data/broker_scaling_results.csv
```

## 📁 Generated Files

- **`data/broker_scaling_results.csv`**: Raw experiment results
- **`data/scaling_experiment_TIMESTAMP.log`**: Detailed execution log
- **`data/scaling_results_TIMESTAMP.png`**: Throughput scaling plots
- **`config/scaling_N_brokers.yaml`**: Dynamic configurations for each broker count

## 🔧 Dynamic Resource Allocation

The experiment automatically adjusts resources based on broker count:

### Segment Size Calculation
- **Total data**: 15GB (10GB messages + 50% header overhead)
- **Per-broker**: 15GB ÷ broker_count
- **Limits**: Minimum 2GB, Maximum 20GB per broker

### Buffer Size Allocation
- **1 broker**: 2048MB (2GB buffers)
- **2-4 brokers**: 1024MB (1GB buffers)
- **5-8 brokers**: 768MB (768MB buffers)
- **9-16 brokers**: 512MB (512MB buffers)
- **17+ brokers**: 256MB (256MB buffers)

## 📈 Expected Results

The experiment should demonstrate:
- **Linear scaling** with increasing broker count
- **Consistent per-broker performance** at 4Gbps limit
- **Total throughput increase** proportional to broker count
- **Stable performance** across all configurations

## 🔍 Results Analysis

### CSV Format
```
broker_count,total_bandwidth_gbps,throughput_gbps,throughput_msgs_per_sec,duration_seconds,segment_size_gb,buffer_size_mb,test_timestamp
```

### Key Metrics
- **Throughput (GB/s)**: Actual data transfer rate achieved
- **Messages/sec**: Message processing rate
- **Duration**: Time to complete 10GB transfer
- **Bandwidth Utilization**: throughput_gbps / total_bandwidth_gbps

## 🛠️ Troubleshooting

### Common Issues

1. **"bc calculator not found"**
   ```bash
   sudo apt-get install bc
   ```

2. **TC permission errors**
   ```bash
   # Ensure you can run sudo commands
   sudo -v
   ```

3. **Build directory not found**
   ```bash
   # Make sure you're in the Embarcadero root directory
   ls build/bin/embarlet  # Should exist
   ```

4. **Memory allocation errors**
   ```bash
   # Ensure hugepages are available
   echo 2048 | sudo tee /proc/sys/vm/nr_hugepages
   ```

### Failed Tests

If some broker counts fail:
- Check `data/scaling_experiment_TIMESTAMP.log` for detailed errors
- Failed tests are marked as "FAILED" in results CSV
- Experiment continues with remaining broker counts

## 📊 Plot Types Generated

1. **Throughput vs Broker Count**: Shows scaling efficiency
2. **Message Rate vs Broker Count**: Shows message processing scaling
3. **Bandwidth Utilization**: Actual vs allocated bandwidth
4. **Test Duration**: Time scaling with broker count

## 🎯 Performance Expectations

Based on our analysis:
- **Single broker**: ~2.5-3.5 GB/s throughput
- **Linear scaling**: Each additional broker should add ~2.5-3.5 GB/s
- **20 brokers**: Expected ~50-70 GB/s total throughput
- **Stable duration**: Test time should remain consistent (~30-60s)

## 🔬 Experiment Details

### Traffic Control Configuration
- **HTB qdisc** with optimized r2q=10 and quantum=10000
- **Per-broker classes** with 4Gbps rate limits
- **Port-based filtering** on broker data ports (1214-1233)
- **Heartbeat ports unlimited** for stable connections

### NUMA Optimization
- **CPU binding**: Node 1 (closest to CXL)
- **Memory binding**: Nodes 1,2 (includes CXL memory)
- **Hugepages enabled** for optimal performance

### Client Configuration
- **Single thread per broker** for consistent load
- **Single connection per broker** for simplified analysis
- **Dynamic buffer sizing** based on broker count
- **Optimized timeouts** for many-broker scenarios

## 📝 Notes

- Each test runs for maximum 5 minutes (300s timeout)
- Failed tests are logged but don't stop the experiment
- Results are appended to CSV for multiple runs
- All configurations are saved for reproducibility
- Detailed logs capture all broker and client output
