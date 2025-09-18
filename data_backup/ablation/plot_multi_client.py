import pandas as pd
import matplotlib.pyplot as plt

# Load the CSV data into a pandas DataFrame
data = pd.read_csv('multi_client.csv')

# Group by the ('num_connections', 'num_clients') pair and calculate the mean throughput
df_avg = data.groupby(['num_connections', 'num_clients'])['throughput'].mean().reset_index()

# Convert throughput from MB/s to GB/s
df_avg['throughput'] /= 1000

# Create a new column for the x-axis labels as strings (e.g., '(1, 1)', '(2, 1)')
df_avg['x_labels'] = df_avg.apply(lambda row: f"({row['num_connections']}, {row['num_clients']})", axis=1)

# Set up the figure and axis objects
fig, ax = plt.subplots(figsize=(10, 6))

# Plot with markers and a smooth dashed line
ax.plot(df_avg['x_labels'], df_avg['throughput'], marker='o', linestyle='-', color='navy', linewidth=2)

# Set plot labels and title with professional font sizes and weights
ax.set_xlabel('(Total Number of Connections, Number of Clients)', fontsize=16, fontweight='bold')
ax.set_ylabel('Average Throughput (GB/s)', fontsize=16, fontweight='bold')
#ax.set_title('Throughput vs (Number of Connections, Number of Clients)', fontsize=16, fontweight='bold')

# Customize the x-axis and y-axis ticks for better readability
ax.tick_params(axis='both', which='major', labelsize=12)

# Rotate x-axis labels for clear reading
plt.xticks(rotation=45, ha='right')

# Add gridlines for the y-axis only (for clarity)
ax.grid(True, which='major', axis='y', linestyle='--', linewidth=0.7, alpha=0.7)

# Set tight layout for better spacing
plt.tight_layout()

# Save the plot as a high-resolution PNG (300 DPI)
plt.savefig('IncreasingConnection.png', dpi=300, bbox_inches='tight')
