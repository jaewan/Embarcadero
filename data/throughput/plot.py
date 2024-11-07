import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

# Function to read and process a CSV file
def process_file(file_path):
    df = pd.read_csv(file_path, on_bad_lines='skip')
    df = df.dropna(subset=['E2E', 'msg_size'])
    grouped = df.groupby('msg_size').mean()
    grouped['publish'] = grouped['publish'] / 1000.0
    return grouped.index, grouped['publish']

# File names and labels
files = ['Emb_order0_ack1.csv', 'Emb_order1_ack1.csv', 'Scalog_order1_ack1.csv', 
         'Emb_order2_ack1.csv', 'KafkaCXL_order0_ack1.csv', 'KafkaDisk_order0_ack1.csv']
labels = ['Emb order 0', 'Emb order 1', 'Scalog (Order 1)', 
          'Emb order 2', 'Kafka-CXL', 'Kafka-Disk']

# Set the style
sns.set_style("whitegrid")
sns.set_palette("deep")

# Create the plot
fig, ax = plt.subplots(figsize=(14, 10))  # Increased figure size

for i, file in enumerate(files):
    msg_size, avg_publish = process_file(file)
    ax.plot(msg_size, avg_publish, marker='o', linestyle='-', linewidth=3, markersize=8, label=labels[i])

# Set the title and labels
ax.set_title('Average Publish Rate vs Message Size', fontsize=30, fontweight='bold', pad=20)
ax.set_xlabel('Message Size (Bytes)', fontsize=26, fontweight='bold', labelpad=10)
ax.set_ylabel('Average Publish Rate (GB/sec)', fontsize=26, fontweight='bold', labelpad=10)

# Set x-axis to logarithmic scale
ax.set_xscale('log')

# Customize the legend
ax.legend(fontsize=24, frameon=True, fancybox=True, shadow=True, loc='best')

# Customize ticks
ax.tick_params(axis='both', which='major', labelsize=16)

# Adjust layout
plt.tight_layout()

# Save the plot with higher DPI
plt.savefig('publish_throughput.png', dpi=300, bbox_inches='tight')

