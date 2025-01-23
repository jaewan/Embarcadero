import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from tabulate import tabulate
import os

# List of CSV files
file_names = [
    "latencies_Emb_1024_ord0.csv",
    "latencies_Emb_1024_ord1.csv",
    "latencies_Emb_1024_ord3.csv",
    "latencies_scalog_1024_ord1.csv",
    "latencies_Kafka_1024.csv"
]

label_per_file = ["Emb Ord 0","Emb Ord 1","Emb Ord 2","Scalog Ord 1","Kafka Ord 0"]
labels = {file: label for file, label in zip(file_names, label_per_file)}

# Dictionary to store data from CSV files
latency_data = {}

# Function to replace negative values with the previous non-negative value
def replace_negative(series):
    mask = series < 0
    idx = np.where(~mask)[0]
    np.maximum.accumulate(series.values[idx], out=series.values[idx])
    series[mask] = np.interp(np.flatnonzero(mask), idx, series.values[idx])
    return series

# Process CSV files
for file in file_names:
    sorted_file = f"sorted_{file}"
    
    if os.path.exists(sorted_file):
        # If sorted file exists, read it directly
        print(f"Using existing sorted file: {sorted_file}")
        data = pd.read_csv(sorted_file, header=None, names=["Latency"])
    else:
        # If sorted file doesn't exist, process the original file
        print(f"Processing original file: {file}")
        data = pd.read_csv(file, header=None, names=["Latency"], low_memory=False)
        data["Latency"] = pd.to_numeric(data["Latency"], errors="coerce")
        data = data.dropna().reset_index(drop=True)
        
        # Replace negative values with the previous non-negative value
        data["Latency"] = replace_negative(data["Latency"])
        
        # Convert ms to microseconds
        data["Latency"] *= 1000
        
        # Sort the data
        data = data.sort_values("Latency").reset_index(drop=True)
        
        # Save the sorted data
        data.to_csv(sorted_file, index=False, header=False)
        print(f"Saved sorted file: {sorted_file}")
    
    latency_data[file] = data

# Prepare data for the statistics table (mean, median, 99.9th percentile)
stats_table = []

for file, data in latency_data.items():
    # Calculate mean, median, and 99.9th percentile
    mean_latency = data["Latency"].mean()
    median_latency = data["Latency"].median()
    percentile_999 = np.percentile(data["Latency"], 99.9)

    # Append results to the table
    stats_table.append([file, mean_latency, median_latency, percentile_999])

# Print the statistics table
print("\nStatistics Table (Mean, Median, 99.9th Percentile Latency in microseconds):")
print(tabulate(stats_table, headers=["File", "Mean (µs)", "Median (µs)", "99.9th Percentile (µs)"], tablefmt="grid"))

# Plot CDF for each file
plt.figure(figsize=(10, 6))

for file, data in latency_data.items():
    # Calculate CDF (y values)
    cdf = np.arange(1, len(data) + 1) / len(data)

    # Plot the CDF
    plt.plot(data["Latency"], cdf, label=labels[file])

# Add title and labels with units in microseconds
plt.title("End to End latency CDF", fontsize=14)
plt.xlabel("Latency (µs)", fontsize=12)
plt.ylabel("CDF", fontsize=12)
plt.grid(True)
plt.legend()

# Save the plot as a PNG file
plt.savefig("latency_cdf_plot_microseconds.pdf")  # Save the figure
print("\nCDF plot saved as 'latency_cdf_plot_microseconds.pdf'")
