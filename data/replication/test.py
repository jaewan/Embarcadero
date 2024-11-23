import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns  # For enhanced aesthetics and color palettes

def hardcoded_plot_bandwidth_vs_replication_factor(filenames):
    """
    Plots a single graph with lines representing different files,
    showing average pubBandwidthMBps against replication factors
    for message_size = 1024, and prints the results as a table.

    Args:
        filenames (list of str): A list of paths to the CSV files.
    """
    # Set seaborn style
    sns.set_style('whitegrid')

    # Define the message size to filter on
    target_message_size = 1024

    # Initialize a figure for the plot
    plt.figure(figsize=(12, 7))

    # Create a list to hold results for the table
    results = []

    # Loop through each file
    for filename in filenames:
        # Read the CSV file into a pandas DataFrame
        df = pd.read_csv(filename)

        # Filter data for the target message size
        filtered_data = df[(df['message_size'] == target_message_size) & (df['ack_level'] == 0)]

        # Initialize a list to hold average bandwidth values
        replication_factors = range(4)  # Replication factors from 0 to 3
        avg_bandwidths = []

        # Loop through each replication factor
        for rep_factor in replication_factors:
            # Get the average bandwidth for the current replication factor
            avg_bandwidth = filtered_data[filtered_data['replication_factor'] == rep_factor]['pubBandwidthMBps'].mean()
            avg_bandwidths.append(avg_bandwidth)

            # Append the result for the table
            results.append({
                'Replication Factor': rep_factor,
                'Average Bandwidth (pubBandwidthMBps)': avg_bandwidth,
                'File': filename
            })

        # Plot the average bandwidth against replication factors
        plt.plot(replication_factors, avg_bandwidths, marker='o', label=f'File: {filename}')

    # Set plot labels and title with enhanced formatting
    plt.xlabel('Replication Factor', fontsize=14)
    plt.ylabel('Average pubBandwidthMBps', fontsize=14)
    plt.title('Average Bandwidth vs Replication Factor (Message Size = 1024)', fontsize=16)
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend(fontsize=12)
    plt.tight_layout()

    # Save the plot to a PDF file
    plt.savefig('bandwidth_vs_replication_factor.pdf', dpi=300, bbox_inches='tight')
    plt.show()  # Show the plot

    # Create a DataFrame from the results list
    results_df = pd.DataFrame(results)

    # Pivot the results DataFrame to create a 4x4 table
    pivot_table = results_df.pivot(index='Replication Factor', columns='File', values='Average Bandwidth (pubBandwidthMBps)')

    # Print the results table
    print("\nFinal Results:")
    print(pivot_table)

def plot_bandwidth_vs_replication_factor(filenames):
    """
    Plots a single graph with lines representing different files,
    showing average pubBandwidthMBps against replication factors
    for message_size = 1024, and prints the results as a table.

    Args:
        filenames (list of str): A list of paths to the CSV files.
    """
    # Set seaborn style
    sns.set_style('whitegrid')

    # Define the message size to filter on
    target_message_size = 1024

    # Initialize a figure for the plot
    plt.figure(figsize=(12, 7))

    # Create a list to hold results for the table
    results = []

    # Loop through each file
    for filename in filenames:
        # Read the CSV file into a pandas DataFrame
        df = pd.read_csv(filename)

        # Filter data for the target message size
        filtered_data = df[(df['message_size'] == target_message_size) & (df['ack_level'] == 0)]

        # Initialize lists to hold replication factors and average bandwidth values
        replication_factors = []
        avg_bandwidths = []

        # Loop through each replication factor (assuming it's in the DataFrame)
        for rep_factor in sorted(filtered_data['replication_factor'].unique()):
            # Get the average bandwidth for the current replication factor
            avg_bandwidth = filtered_data[filtered_data['replication_factor'] == rep_factor]['pubBandwidthMBps'].mean()
            replication_factors.append(rep_factor)
            avg_bandwidths.append(avg_bandwidth)

            # Append the result for the table
            results.append({
                'Replication Factor': rep_factor,
                'Average Bandwidth (pubBandwidthMBps)': avg_bandwidth,
                'File': filename
            })

        # Plot the average bandwidth against replication factors
        plt.plot(replication_factors, avg_bandwidths, marker='o', label=f'File: {filename}')

    # Set plot labels and title with enhanced formatting
    plt.xlabel('Replication Factor', fontsize=14)
    plt.ylabel('Average pubBandwidthMBps', fontsize=14)
    plt.title('Average Bandwidth vs Replication Factor (Message Size = 1024)', fontsize=16)
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend(fontsize=12)
    plt.tight_layout()

    # Save the plot to a PDF file
    plt.savefig('bandwidth_vs_replication_factor.pdf', dpi=300, bbox_inches='tight')
    plt.show()  # Show the plot

    # Create a DataFrame from the results list
    results_df = pd.DataFrame(results)

    # Print the results table
    print("\nFinal Results:")
    print(results_df)

# Example usage with a list of CSV files
#plot_bandwidth_vs_replication_factor(['pub/disk_result.csv', 'pub/result.csv', 'e2e/disk_result.csv', 'e2e/result.csv'])
hardcoded_plot_bandwidth_vs_replication_factor(['pub/disk_result.csv', 'pub/result.csv', 'e2e/disk_result.csv', 'e2e/result.csv'])
