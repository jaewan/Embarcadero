import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from tabulate import tabulate

# List of CSV files
file_names = [
    "latencies_Emb_512_ord0.csv",
    "latencies_Emb_512_ord1.csv",
    "latencies_Emb_512_ord3.csv",
    "latencies_scalog_512_ord1.csv",
    "latencies_Kafka_512.csv"
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

# Read CSV files, replace negative values, and store them in the dictionary
for file in file_names:
    # Read CSV and convert the 'Latency' column to numeric, forcing errors to NaN
    data = pd.read_csv(file, header=None, names=["Latency"], low_memory=False)
    data["Latency"] = pd.to_numeric(data["Latency"], errors="coerce")  # Convert to numeric, NaN for invalid entries
    data = data.dropna().reset_index(drop=True)  # Drop rows with NaN values and reset index

    # Replace negative values with the previous non-negative value
    data["Latency"] = replace_negative(data["Latency"])

    # Convert ms to microseconds
    data["Latency"] *= 1000

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
    # Sort latency values
    sorted_data = np.sort(data["Latency"].values)

    # Calculate CDF (y values)
    cdf = np.arange(1, len(sorted_data) + 1) / len(sorted_data)

    # Plot the CDF
    plt.plot(sorted_data, cdf, label=labels[file])

# Add title and labels with units in microseconds
plt.title("CDF of Latency (in Microseconds)", fontsize=22)
plt.xlabel("Latency (µs)", fontsize=18)
plt.ylabel("CDF", fontsize=18)
plt.grid(True)
plt.legend(fontsize=18)

# Save the plot as a PNG file
plt.savefig("latency_cdf_plot_microseconds.pdf")  #
