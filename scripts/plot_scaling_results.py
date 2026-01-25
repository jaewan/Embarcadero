#!/usr/bin/env python3

"""
Embarcadero Broker Scaling Results Plotter
Generates throughput vs broker count plots from scaling experiment results
"""

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import sys
import os
from datetime import datetime

def plot_scaling_results(csv_file):
    """Generate scaling plots from experiment results"""
    
    # Read results
    try:
        df = pd.read_csv(csv_file)
        print(f"📊 Loaded {len(df)} scaling test results from {csv_file}")
    except FileNotFoundError:
        print(f"❌ Results file not found: {csv_file}")
        print("Run the scaling experiment first: ./scripts/broker_scaling_experiment.sh")
        return False
    except Exception as e:
        print(f"❌ Error reading results: {e}")
        return False
    
    # Filter out failed tests
    df_success = df[df['throughput_gbps'] > 0].copy()
    df_failed = df[df['throughput_gbps'] == 0].copy()
    
    if len(df_success) == 0:
        print("❌ No successful test results found")
        return False
    
    print(f"✅ {len(df_success)} successful tests, {len(df_failed)} failed tests")
    
    # Set up the plotting style
    plt.style.use('seaborn-v0_8')
    sns.set_palette("husl")
    
    # Create figure with subplots
    fig, ((ax1, ax2), (ax3, ax4)) = plt.subplots(2, 2, figsize=(15, 12))
    fig.suptitle('Embarcadero Broker Scaling Performance', fontsize=16, fontweight='bold')
    
    # Plot 1: Throughput vs Broker Count
    ax1.plot(df_success['broker_count'], df_success['throughput_gbps'], 
             'o-', linewidth=2, markersize=8, label='Throughput')
    ax1.set_xlabel('Number of Brokers')
    ax1.set_ylabel('Throughput (GB/s)')
    ax1.set_title('Throughput Scaling')
    ax1.grid(True, alpha=0.3)
    ax1.legend()
    
    # Add failed points
    if len(df_failed) > 0:
        ax1.scatter(df_failed['broker_count'], [0] * len(df_failed), 
                   color='red', s=100, marker='x', label='Failed', zorder=5)
    
    # Plot 2: Messages per Second vs Broker Count
    ax2.plot(df_success['broker_count'], df_success['throughput_msgs_per_sec'], 
             'o-', linewidth=2, markersize=8, color='green', label='Messages/sec')
    ax2.set_xlabel('Number of Brokers')
    ax2.set_ylabel('Messages per Second')
    ax2.set_title('Message Rate Scaling')
    ax2.grid(True, alpha=0.3)
    ax2.legend()
    
    # Plot 3: Total Bandwidth vs Achieved Throughput
    ax3.plot(df_success['total_bandwidth_gbps'], df_success['throughput_gbps'], 
             'o-', linewidth=2, markersize=8, color='orange', label='Actual Throughput')
    # Add ideal line (throughput = bandwidth)
    max_bw = df_success['total_bandwidth_gbps'].max()
    ax3.plot([0, max_bw], [0, max_bw], '--', color='gray', alpha=0.7, label='Ideal (100% utilization)')
    ax3.set_xlabel('Total Bandwidth Allocation (Gbps)')
    ax3.set_ylabel('Achieved Throughput (GB/s)')
    ax3.set_title('Bandwidth Utilization')
    ax3.grid(True, alpha=0.3)
    ax3.legend()
    
    # Plot 4: Test Duration vs Broker Count
    ax4.plot(df_success['broker_count'], df_success['duration_seconds'], 
             'o-', linewidth=2, markersize=8, color='purple', label='Duration')
    ax4.set_xlabel('Number of Brokers')
    ax4.set_ylabel('Test Duration (seconds)')
    ax4.set_title('Test Duration Scaling')
    ax4.grid(True, alpha=0.3)
    ax4.legend()
    
    # Adjust layout
    plt.tight_layout()
    
    # Save plots
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    plot_file = f"data/scaling_results_{timestamp}.png"
    plt.savefig(plot_file, dpi=300, bbox_inches='tight')
    print(f"📈 Scaling plots saved to: {plot_file}")
    
    # Display summary statistics
    print("\n📊 SCALING PERFORMANCE SUMMARY:")
    print("=" * 50)
    print(f"Best throughput: {df_success['throughput_gbps'].max():.2f} GB/s with {df_success.loc[df_success['throughput_gbps'].idxmax(), 'broker_count']} brokers")
    print(f"Scaling efficiency: {(df_success['throughput_gbps'].iloc[-1] / df_success['throughput_gbps'].iloc[0]):.1f}x improvement")
    print(f"Average test duration: {df_success['duration_seconds'].mean():.1f}s")
    
    if len(df_failed) > 0:
        print(f"\n⚠️  Failed configurations: {df_failed['broker_count'].tolist()}")
    
    # Show plot
    plt.show()
    
    return True

def main():
    """Main function"""
    csv_file = "data/broker_scaling_results.csv"
    
    if len(sys.argv) > 1:
        csv_file = sys.argv[1]
    
    if not os.path.exists(csv_file):
        print(f"❌ Results file not found: {csv_file}")
        print("Run the scaling experiment first:")
        print("  ./scripts/broker_scaling_experiment.sh")
        return 1
    
    if plot_scaling_results(csv_file):
        print("✅ Scaling analysis complete!")
        return 0
    else:
        print("❌ Failed to generate scaling plots")
        return 1

if __name__ == "__main__":
    sys.exit(main())
