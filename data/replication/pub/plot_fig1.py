import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np

# Load the CSV file
df = pd.read_csv('disk_result.csv')

# Convert pubBandwidthMBps to GBps
df['pubBandwidthGBps'] = df['pubBandwidthMBps'] / 1024

# Group by sequencer and message_size, averaging the bandwidth
grouped = df.groupby(['sequencer', 'message_size'])['pubBandwidthGBps'].mean().reset_index()

# Set seaborn style for publication-quality plots
sns.set(style='whitegrid', context='paper', font_scale=1.5)

# Set up the plot
plt.figure(figsize=(8, 5))
palette = sns.color_palette("colorblind", n_colors=grouped['sequencer'].nunique())

# Plot lines for each sequencer
for idx, (sequencer, group) in enumerate(grouped.groupby('sequencer')):
    if sequencer == "SCALOG":
        sequencer = "Scalog         (Weak Total Order)"
    elif sequencer == "EMBARCADERO":
        sequencer = "Embarcadero (Strong Total Order)"
    elif sequencer == "CORFU":
        sequencer = "Corfu          (Strong Total Order)"
    group = group.sort_values('message_size')
    plt.plot(group['message_size'], group['pubBandwidthGBps'],
             marker='o', linewidth=2, markersize=6,
             label=sequencer, color=palette[idx])

# Set x-axis to log base 2
plt.xscale('log', base=2)

# Define x-ticks and labels in power-of-2 notation
x_vals = [128, 256, 512, 1024, 4096, 16384, 65536, 262144, 1048576]
x_labels = [r'$2^7$', r'$2^8$', r'$2^9$', r'$2^{10}$',
            r'$2^{12}$', r'$2^{14}$', r'$2^{16}$', r'$2^{18}$', r'$2^{20}$']
plt.xticks(x_vals, x_labels)

# Axis labels
plt.xlabel('Message Size (bytes)', labelpad=10)
plt.ylabel('Publish Bandwidth (GB/s)', labelpad=10)

# Grid style
plt.grid(True, which='both', linestyle='--', linewidth=0.5)

# Legend
plt.legend(loc='best', frameon=True)

# Tight layout for proper spacing
plt.tight_layout()

# Save high-quality figure
plt.savefig('/home/domin/pub_bandwidth_disk.png', dpi=300)
print("Plotted graph to pub_bandwidth_plot.pdf")
