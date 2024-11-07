import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

# Function to read and process a CSV file
def process_file(file_path):
    print(file_path)
    df = pd.read_csv(file_path, on_bad_lines='skip')
    df = df.dropna(subset=['subscribe', 'msg_size'])
    grouped = df.groupby('msg_size').mean()
    grouped['subscribe'] = grouped['subscribe'] / 1000.0
    return grouped.index, grouped['subscribe']

# File names and labels
files = ['Emb_order0_ack1.csv', 'Emb_order1_ack1.csv', 
         'Emb_order2_ack1.csv', 'KafkaCXL_order0_ack1.csv', 'KafkaDisk_order0_ack1.csv']
labels = ['Emb order 0', 'Emb order 1',
          'Emb order 2', 'Kafka-CXL', 'Kafka-Disk']

# Set the style
sns.set_style("whitegrid")
sns.set_palette("deep")

# Create the plot
fig, ax = plt.subplots(figsize=(12, 8))

for i, file in enumerate(files):
    msg_size, avg_publish = process_file(file)
    ax.plot(msg_size, avg_publish, marker='o', linestyle='-', linewidth=2, markersize=6, label=labels[i])

# Set the title and labels
#ax.set_title('Average Subscribe throughput', fontsize=20, fontweight='bold', pad=20)
ax.set_xlabel('Message Size (Bytes)', fontsize=24, fontweight='bold', labelpad=10)
ax.set_ylabel('Average Subscribe', fontsize=24, fontweight='bold', labelpad=10)

# Set x-axis to logarithmic scale
ax.set_xscale('log')

# Customize the legend
ax.legend(fontsize=18, frameon=True, fancybox=True, shadow=True, loc='best')

# Customize ticks
ax.tick_params(axis='both', which='major', labelsize=12)

# Tight layout
plt.tight_layout()

# Save the plot with higher DPI
plt.savefig('subscribe_throughput.png', dpi=300, bbox_inches='tight')

# Show the plot
plt.show()
