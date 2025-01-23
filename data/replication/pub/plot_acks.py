import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# Define a professional color palette
colors = ['#4C72B0', '#DD8452', '#55A868']  # Blue, Orange, Green

# Read the CSV file
file_path = 'result.csv'
df = pd.read_csv(file_path)

# Group the data by `replication_factor` and `ack_level` and calculate the average `pubBandwidthMBps`
grouped_data = df.groupby(['replication_factor', 'ack_level'])['pubBandwidthMBps'].mean().unstack()

# Create the grouped bar chart
fig, ax = plt.subplots(figsize=(10, 6))

# X positions for the replication factors
x = np.arange(len(grouped_data.index))  # [0, 1, 2] for replication factors 1, 2, 3

# Bar width
bar_width = 0.25

# Plot bars for each ack_level
ack_levels = grouped_data.columns  # [0, 1, 2]
for i, ack_level in enumerate(ack_levels):
    ax.bar(x + i * bar_width, grouped_data[ack_level], width=bar_width, label=f'ack_level {ack_level}', color=colors[i])

# Customize the plot
#ax.set_title('Average pubBandwidthMBps by Replication Factor and Ack Level', fontsize=14)
ax.set_xlabel('Replication Factor', fontsize=12)
ax.set_ylabel('Average pubBandwidthMBps', fontsize=12)
ax.set_xticks(x + bar_width / 2)  # Center x-ticks on groups
ax.set_xticklabels(grouped_data.index + 1)
ax.legend(title='Ack Levels', fontsize=10, title_fontsize=10)
ax.grid(axis='y', linestyle='--', alpha=0.7)


# Show the plot
plt.tight_layout()
fig.savefig('ack_replication.pdf', dpi=300, bbox_inches='tight')
plt.show()
