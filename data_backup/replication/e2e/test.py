import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns  # For enhanced aesthetics and color palettes

def plot_bandwidth_vs_message_size(filename):
    """
    Plots a single graph with 8 lines representing different combinations of
    replication factors and ack levels, with distinct color groups for each ack level,
    and prints a table of average results.

    Args:
        filename (str): The path to the CSV file without .csv.
    """
    # Read the CSV file into a pandas DataFrame
    df = pd.read_csv(filename)

    # Message sizes to plot
    message_sizes = [128, 512, 1024, 4096, 65536, 1048576]

    # Replication factors and ack levels to plot
    replication_factors = [0, 1, 2, 3]
    ack_levels = [0, 2]

    # Initialize a list to store the average results for the table
    results = []

    # Define color palettes for each ack level
    color_palette_ack0 = sns.color_palette("Blues", len(replication_factors))
    color_palette_ack2 = sns.color_palette("Reds", len(replication_factors))

    # Create a figure and axes for the plot
    plt.figure(figsize=(12, 7))

    # Set seaborn style
    sns.set_style('whitegrid')

    # Loop through each replication factor and ack level and plot the data
    for ack_level in ack_levels:
        if ack_level == 0:
            color_palette = color_palette_ack0
        else:
            color_palette = color_palette_ack2

        for i, rep_factor in enumerate(replication_factors):
            avg_bandwidth = []
            for size in message_sizes:
                # Filter data based on conditions
                filtered_data = df[(df['message_size'] == size) &
                                   (df['replication_factor'] == rep_factor) &
                                   (df['ack_level'] == ack_level)]
                # Compute average bandwidth
                avg = filtered_data['pubBandwidthMBps'].mean()
                avg_bandwidth.append(avg)
                
                # Store the results for the table
                results.append({
                    'Message Size': size,
                    'Replication Factor': rep_factor,
                    'Ack Level': ack_level,
                    'Average Bandwidth (pubBandwidthMBps)': avg
                })

            # Plot the average bandwidth
            plt.plot(message_sizes, avg_bandwidth, marker='o',
                     label=f'Rep Factor={rep_factor}, Ack Level={ack_level}',
                     color=color_palette[i])

    # Set plot labels and title with enhanced formatting
    plt.xlabel('Message Size', fontsize=14)  # Increase font size for labels
    plt.ylabel('Average pubBandwidthMBps', fontsize=14)
    plt.title('Average Bandwidth vs Message Size for Different Configurations', fontsize=16)
    plt.xscale('log', base=2)  # Use log scale for better visibility
    plt.legend(fontsize=12)  # Increase legend font size
    plt.grid(True, linestyle='--', alpha=0.7)  # Lighter grid lines for a cleaner look

    plt.tick_params(axis='both', which='major', labelsize=12)
    plt.tight_layout()

    plt.savefig(filename+'.pdf', dpi=300, bbox_inches='tight')

    # Create a DataFrame from the results list
    results_df = pd.DataFrame(results)

    # Print the results table
    print("\nFinal Results:")
    print(results_df.pivot_table(index=['Message Size', 'Replication Factor'],
                                  columns='Ack Level',
                                  values='Average Bandwidth (pubBandwidthMBps)',
                                  aggfunc='mean'))

# Example usage
plot_bandwidth_vs_message_size('disk_result.csv')
plot_bandwidth_vs_message_size('result.csv')
