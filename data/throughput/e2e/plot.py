import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns  # For enhanced aesthetics and color palettes

def plot_bandwidth_vs_message_size_order(csv_file):
    """
    Plots average end-to-end bandwidth against message size for different order levels.

    Args:
        csv_file (str): The path to the CSV file containing the experimental results.
    """
    try:
        # Read the CSV file into a pandas DataFrame
        df = pd.read_csv(csv_file)
    except FileNotFoundError:
        print(f"Error: File '{csv_file}' not found.")
        return
    except pd.errors.EmptyDataError:
        print(f"Error: File '{csv_file}' is empty.")
        return
    except pd.errors.ParserError:
        print(f"Error: Could not parse '{csv_file}'. Check if it's a valid CSV file.")
        return

    # Message sizes and order levels to plot
    message_sizes = [128, 256, 512, 1024, 4096,16384, 65536, 262144]
    orders = [0, 4, 1]
    sequencers = ['EMBARCADERO', 'SCALOG', 'CORFU', 'KAFKACXL', 'KAFKADISK']
    legends = ['Embarcadero\u2002Basic Order', 'Embarcadero\u2002Strong Total Order', 'Scalog\u2002\u2002Weak Total Order', 'Corfu\u2002\u2002Strong Total Order', 'Kafka-CXL', 'Kafka-Disk']

    # Use a visually appealing color palette
    color_palette = sns.color_palette("husl", len(legends))

    # Create a figure and axes for the plot (golden ratio for aspect ratio)
    plt.figure(figsize=(11.326, 7))

    # Set seaborn style for better aesthetics
    sns.set_style('whitegrid')

    # Loop through each order level and plot the data
    for i, order in enumerate(orders):
        avg_bandwidth = []
        for size in message_sizes:
            # Filter data for the current message size, order, and sequencer
            filtered_data = df[(df['message_size'] == size) & (df['order'] == order) & (df['sequencer'] == sequencers[0])]

            # Calculate the average bandwidth
            avg = filtered_data['e2eBandwidthMBps'].mean() if not filtered_data.empty else 0
            avg_bandwidth.append(avg/1000)

        # Plot the average bandwidth for the current order level
        plt.plot(message_sizes, avg_bandwidth, marker='o',
                 label=legends[i],
                 color=color_palette[i])

    # Plot data for CORFU, SCALOG, KAFKA sequencer
    for i in range(3):
        avg_bandwidth = []
        for size in message_sizes:
            # Filter data for the current message size and CORFU sequencer
            filtered_data = df[(df['message_size'] == size) & (df['sequencer'] == sequencers[2 + i])]

            # Calculate the average bandwidth
            avg = filtered_data['e2eBandwidthMBps'].mean() if not filtered_data.empty else 0
            avg_bandwidth.append(avg/1000)

        # Plot the average bandwidth for CORFU
        plt.plot(message_sizes, avg_bandwidth, marker='o',
                 label=legends[3+i],
                 color=color_palette[3+i])

    # Set plot labels and title with enhanced formatting
    plt.xlabel('Message Size', fontsize=16)
    plt.ylabel('Bandwidth (GBps)', fontsize=16)
    #plt.title('Average Bandwidth vs Message Size for Different Order Levels', fontsize=16)

    # Use a log scale for the x-axis (base 2) for better visualization
    plt.xscale('log', base=2)
    plt.ylim(bottom=0)

    # Add a legend with increased font size
    plt.legend(fontsize=12, loc='best')

    # Add lighter grid lines for a cleaner look
    plt.grid(True, linestyle='--', alpha=0.7)

    # Increase the font size of tick labels
    plt.tick_params(axis='both', which='major', labelsize=12)

    # Adjust layout to prevent labels from overlapping
    plt.tight_layout()

    # Save the plot as a PDF file with high resolution
    plt.savefig('Throughput.pdf', dpi=300, bbox_inches='tight')
    plt.show()

# Example usage (replace 'result.csv' with your actual file path)
plot_bandwidth_vs_message_size_order('result.csv')
