import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from tabulate import tabulate

# List of CSV files
file_names = [
    "Emb_1024_Order0_latencies.csv",
    "Emb_1024_Order4_latencies.csv",
    "Scalog_1024_Order1_latencies.csv",
    "Corfu_1024_Order2_latencies.csv",
    "Kafka_1024_latencies.csv"
]

label_per_file = ["Emb","Emb Strong","Scalog Weak","Corfu Strong", "Kafka"]
labels = {}

# Dictionary to store data from CSV files
latency_data = {}

# Read CSV files and store them in the dictionary
for i, file in enumerate(file_names):
    # Read CSV and convert the 'Latency' column to numeric, forcing errors to NaN
    data = pd.read_csv(file, header=None, names=["Latency"])
    data["Latency"] = pd.to_numeric(data["Latency"], errors="coerce")  # Convert to numeric, NaN for invalid entries
    data = data.dropna()  # Drop rows with NaN values
    latency_data[file] = data
    labels[file] = label_per_file[i]

# Prepare data for the statistics table (mean, median, 99.9th percentile)
stats_table = []

for file, data in latency_data.items():
    # Convert non-Kafka latencies from ms to µs
    if "Kafka" not in file:
        data["Latency"] *= 1000
    # Assuming latencies are in nanoseconds, convert to microseconds
    data["Latency"] /= 1000  # Convert ms to microseconds

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
    # Sort latency values (now in microseconds)
    sorted_data = np.sort(data["Latency"].values)

    # Calculate CDF (y values)
    cdf = np.arange(1, len(sorted_data) + 1) / len(sorted_data)

    # Plot the CDF
    plt.plot(sorted_data, cdf, label=labels[file])

# Add title and labels with units in microseconds
plt.title("CDF of Latency (in Microseconds)", fontsize=14)
plt.xlabel("Latency (µs)", fontsize=12)
plt.ylabel("CDF", fontsize=12)
plt.grid(True)
plt.legend()

plt.xlim(left=0, right=0.21e12)
plt.tight_layout()
# Save the plot as a PNG file
plt.savefig("latency_cdf_plot_microseconds.pdf", dpi=300)  # Save the figure with high resolution
print("\nCDF plot saved as 'latency_cdf_plot_microseconds.pdf'")
