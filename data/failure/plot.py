import pandas as pd
import matplotlib.pyplot as plt
from matplotlib import colormaps  # New colormap API

# Read the first CSV file
file_name = 'real_time_throughput.csv'
try:
    # Load the data into a pandas DataFrame
    data = pd.read_csv(file_name)
except FileNotFoundError:
    print(f"Error: The file '{file_name}' was not found.")
    exit()

# Verify the required columns exist
required_columns = ['0', '1', '2', '3', 'RealTimeThroughput']
if not all(col in data.columns for col in required_columns):
    print(f"Error: The file does not contain the required columns: {required_columns}")
    exit()

# Handle missing values (optional)
data = data.fillna(0)  # Replace NaN with 0

# Read the second CSV file
second_file_name = 'no_failure.csv'
try:
    # Load the second data file
    no_failure_data = pd.read_csv(second_file_name)
except FileNotFoundError:
    print(f"Error: The file '{second_file_name}' was not found.")
    exit()

# Verify the 'RealTimeThroughput' column exists in the second file
if 'RealTimeThroughput' not in no_failure_data.columns:
    print(f"Error: The file '{second_file_name}' does not contain the 'RealTimeThroughput' column.")
    exit()

# Handle missing values in the second dataset (optional)
no_failure_data = no_failure_data.fillna(0)  # Replace NaN with 0

# Extract throughput data
time_interval = 5  # Time interval in milliseconds
time = data.index * time_interval  # Generate the time values based on the row index
time_no_failure = no_failure_data.index * time_interval  # Generate time values for the second file

# Plot the throughput for each column
plt.style.use('seaborn-v0_8-whitegrid')  # Use a nice style
plt.figure(figsize=(12, 7), dpi=300)  # Set the figure size and resolution

# Define colormaps
broker_colormap = colormaps['tab10']  # Colormap for Brokers
aggregate_colormap = colormaps['cool']  # Colormap for Aggregate Throughput

# --- Plotting and collecting handles and labels for custom legend ---
broker_handles = []
broker_labels = []
for i, col in enumerate(required_columns[0:-1]):  # Iterate over the broker columns
    broker_line, = plt.plot(time, data[col], color=broker_colormap(i / (len(required_columns) - 2)), linewidth=1)
    broker_handles.append(broker_line)
    broker_labels.append(f'Broker {col}')

failure_aggregate_line, = plt.plot(time, (data['RealTimeThroughput']), linestyle='-', color='black', linewidth=2)
no_failure_aggregate_line, = plt.plot(time_no_failure, (no_failure_data['RealTimeThroughput']),
         linestyle='--', color=aggregate_colormap(0.4), linewidth=1.5, alpha=0.5)

# Define the desired order of handles and labels for the legend
handles = [failure_aggregate_line, no_failure_aggregate_line] + broker_handles
labels = ['Aggregate Throughput (Failures)', 'Aggregate Throughput (No Failures)'] + broker_labels

# Add labels, title, and legend
plt.xlabel('Time (ms)')
plt.ylabel('Real Time Throughput (GB/s)')
#plt.title('Real-Time Throughput Over Time')

# Add the legend with custom order
plt.legend(handles, labels, loc='upper right', fontsize=12)

plt.xticks(fontsize=12)
plt.yticks(fontsize=12)
plt.grid(True, which='both', linestyle='--', linewidth=0.7)

# Save the plot to a file (optional)
plt.savefig('failures.pdf', dpi=300, bbox_inches='tight')
