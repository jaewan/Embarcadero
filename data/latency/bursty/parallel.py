import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from tabulate import tabulate
from concurrent.futures import ProcessPoolExecutor

# List of CSV files
file_names = [
    "Emb_1024_Order0_latencies.csv",
    "Emb_1024_Order4_latencies.csv",
    "Scalog_1024_Order1_latencies.csv",
    "Corfu_1024_Order2_latencies.csv",
    "Kafka_1024_latencies.csv"
]

label_per_file = ["Emb", "Emb Strong", "Scalog Weak", "Corfu Strong", "Kafka"]
labels = {file: label for file, label in zip(file_names, label_per_file)}

# Function to process a single file
def process_file(file):
    # Read CSV and convert the 'Latency' column to numeric, forcing errors to NaN
    data = pd.read_csv(file, header=None, names=["Latency"])
    data["Latency"] = pd.to_numeric(data["Latency"], errors="coerce")  # Convert to numeric, NaN for invalid entries
    data = data.dropna()  # Drop rows with NaN values

    # Convert Kafka latencies (stored in ms) to microseconds
    if "Kafka" in file:
        data["Latency"] *= 1000  # Convert ms to µs for Kafka

    # Convert all latencies to microseconds (if not already)
    data["Latency"] /= 1000  # Convert ns to µs for non-Kafka files (if applicable)

    # Calculate mean, median, and 99.9th percentile
    mean_latency = data["Latency"].mean()
    median_latency = data["Latency"].median()
    percentile_999 = np.percentile(data["Latency"], 99.9)

    # Prepare the CDF
    sorted_data = np.sort(data["Latency"].values)
    cdf = np.arange(1, len(sorted_data) + 1) / len(sorted_data)

    return file, data, mean_latency, median_latency, percentile_999, sorted_data, cdf

# Use ProcessPoolExecutor to parallelize file processing
results = []
with ProcessPoolExecutor(max_workers=4) as executor:  # Adjust max_workers to a safe number
    results = list(executor.map(process_file, file_names))

# Prepare the statistics table
stats_table = []
latency_data = {}
sorted_cdfs = {}

for result in results:
    file, data, mean_latency, median_latency, percentile_999, sorted_data, cdf = result
    latency_data[file] = data
    sorted_cdfs[file] = (sorted_data, cdf)
    stats_table.append([file, mean_latency, median_latency, percentile_999])

# Print the statistics table
print("\nStatistics Table (Mean, Median, 99.9th Percentile Latency in microseconds):")
print(tabulate(stats_table, headers=["File", "Mean (µs)", "Median (µs)", "99.9th Percentile (µs)"], tablefmt="grid"))

# Plot CDF for each file
plt.figure(figsize=(10, 6))

for file, (sorted_data, cdf) in sorted_cdfs.items():
    # Plot the CDF
    plt.plot(sorted_data, cdf, label=labels[file])

# Add title and labels with units in microseconds
plt.title("CDF of Latency (in Microseconds)", fontsize=14)
plt.xlabel("Latency (µs)", fontsize=12)
plt.ylabel("CDF", fontsize=12)
plt.grid(True)
plt.legend()

# Set x-axis limits to start at 0 and end at 0.21e12 (210 billion microseconds)
plt.xlim(left=0, right=190000)

plt.tight_layout()
# Save the plot as a PNG file
plt.savefig("latency_cdf_plot_microseconds.pdf", dpi=300)  # Save the figure with high resolution
print("\nCDF plot saved as 'latency_cdf_plot_microseconds.pdf'")
