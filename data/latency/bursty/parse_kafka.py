import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from tabulate import tabulate

# List of CSV files
file_names = [
    "Kafka_1024_latencies.csv"
]

label_per_file = ["Kafka"]
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
