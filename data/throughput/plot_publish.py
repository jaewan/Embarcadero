import pandas as pd
import matplotlib.pyplot as plt

# Function to read and process a CSV file
def process_file(file_path):
    # Read the CSV file, skipping bad lines
    df = pd.read_csv(file_path, on_bad_lines='skip')

    # Drop any rows with missing publish or msg_size values
    df = df.dropna(subset=['publish', 'msg_size'])

    # Group by msg_size and calculate the mean of publish values
    grouped = df.groupby('msg_size').mean()

    # Divide publish values by 1000 to convert to GB/sec
    grouped['publish'] = grouped['publish'] / 1000.0

    return grouped.index, grouped['publish']

# File names
files = ['Emb_order0_ack1.csv', 'Emb_order1_ack1.csv','Scalog_order1_ack1.csv', 'Emb_order2_ack1.csv', 'KafkaCXL_order0_ack1.csv', 'KafkaDisk_order0_ack1.csv']
labels = ['Emb_order 0', 'Emb_irder 1', 'Scalog (Order1)',  'Emb_order 2', 'Kafka-cxl', 'Kafka-disk']

# Plot the data for each file
plt.figure(figsize=(10, 7))

for i, file in enumerate(files):
    msg_size, avg_publish = process_file(file)
    plt.plot(msg_size, avg_publish, marker='o', linestyle='-', label=labels[i])

# Set the title and labels
plt.title('Average Publish Rate vs Message Size')
plt.xlabel('Message Size (Bytes)')
plt.ylabel('Average Publish Rate (GB/sec)')

# Set x-axis to logarithmic scale
plt.xscale('log')

# Add grid and legend
plt.grid(True, which="both", ls="--")
plt.legend()

# Save the plot
plt.savefig('publish_throughput.png', dpi=300)

# Show the plot
plt.show()
