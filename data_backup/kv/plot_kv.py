import pandas as pd
import matplotlib.pyplot as plt
import matplotlib as mpl
import numpy as np
import seaborn as sns

# Set up publication-quality plot settings
plt.rcParams.update({
    'font.family': 'serif',
    'font.serif': ['Times New Roman'],
    'font.size': 10,
    'axes.labelsize': 12,
    'axes.titlesize': 12,
    'xtick.labelsize': 10,
    'ytick.labelsize': 10,
    'legend.fontsize': 10,
    'figure.figsize': (8, 5),
    'figure.dpi': 300,
    'savefig.dpi': 600,
    'savefig.bbox': 'tight',
    'savefig.pad_inches': 0.05
})

# Enable seaborn for better aesthetics
sns.set_style('whitegrid')
sns.set_context('paper')

# Read the CSV files
emb_data = pd.read_csv('emb_multi_put_results.csv')
scalog_data = pd.read_csv('scalog_multi_put_results.csv')

# Group by batch_size and calculate mean throughput if there are multiple measurements
emb_grouped = emb_data.groupby('batch_size')['throughput_ops_per_sec'].mean().reset_index()
scalog_grouped = scalog_data.groupby('batch_size')['throughput_ops_per_sec'].mean().reset_index()

# Create figure and axis
fig, ax = plt.subplots()

# Set the width of the bars
bar_width = 0.35

# Get unique batch sizes and create positions for the bars
batch_sizes = sorted(set(list(emb_grouped['batch_size']) + list(scalog_grouped['batch_size'])))
indices = np.arange(len(batch_sizes))

# Create the bars
emb_bars = ax.bar(indices - bar_width/2,
                 [emb_grouped[emb_grouped['batch_size']==bs]['throughput_ops_per_sec'].values[0]
                  if bs in emb_grouped['batch_size'].values else 0
                  for bs in batch_sizes],
                 bar_width, label='EMB', color='#1f77b4', edgecolor='black', linewidth=0.5)

scalog_bars = ax.bar(indices + bar_width/2,
                    [scalog_grouped[scalog_grouped['batch_size']==bs]['throughput_ops_per_sec'].values[0]
                     if bs in scalog_grouped['batch_size'].values else 0
                     for bs in batch_sizes],
                    bar_width, label='ScaLOG', color='#ff7f0e', edgecolor='black', linewidth=0.5)

# Add labels, title, and legend
ax.set_xlabel('Batch Size')
ax.set_ylabel('Throughput (ops/sec)')
ax.set_title('Throughput Comparison: EMB vs. ScaLOG')
ax.set_xticks(indices)
ax.set_xticklabels(batch_sizes)
ax.legend()

# Add grid lines for y-axis only (horizontal)
ax.yaxis.grid(True, linestyle='--', alpha=0.7)
ax.set_axisbelow(True)  # Put grid lines behind the bars

# Add value labels on top of each bar
def add_value_labels(bars):
    for bar in bars:
        height = bar.get_height()
        if height > 0:  # Only label bars with non-zero values
            ax.annotate(f'{int(height):,}',
                        xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3),  # 3 points vertical offset
                        textcoords="offset points",
                        ha='center', va='bottom', fontsize=8)

add_value_labels(emb_bars)
add_value_labels(scalog_bars)

# Adjust layout and save
plt.tight_layout()
plt.savefig('kv.pdf')  # Save as PDF for publication

# Show the plot
plt.show()
