import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns  # For enhanced aesthetics and color palettes

def plot_bandwidth_vs_message_size_order(csv_file):
    """
    Plots two lines showing average bandwidth vs message size for order 0 and 1.

    Args:
        csv_file (str): The path to the CSV file.
    """
    # Read the CSV file into a pandas DataFrame
    df = pd.read_csv(csv_file)

    # Message sizes to plot
    message_sizes = [128, 512, 1024, 4096, 65536, 1048576]

    # Order levels to plot
    orders = [0, 1]

    # Define color palettes for each order level
    color_palette = sns.color_palette("husl", len(orders))

    # Create a figure and axes for the plot
    plt.figure(figsize=(12, 7))

    # Set seaborn style
    sns.set_style('whitegrid')

    # Loop through each order level and plot the data
    for i, order in enumerate(orders):
        avg_bandwidth = []
        for size in message_sizes:
            filtered_data = df[(df['message_size'] == size) & (df['order'] == order)]
            #avg = filtered_data['e2eBandwidthMBps'].mean()
            avg = filtered_data['pubBandwidthMBps'].mean()
            avg_bandwidth.append(avg)

        plt.plot(message_sizes, avg_bandwidth, marker='o',
                 label=f'Order Level={order}',
                 color=color_palette[i])

    # Set plot labels and title with enhanced formatting
    plt.xlabel('Message Size', fontsize=14)  # Increase font size for labels
    plt.ylabel('Average pubBandwidthMBps', fontsize=14)
    plt.title('Average Bandwidth vs Message Size for Different Order Levels', fontsize=16)
    plt.xscale('log', base=2)  # Use log scale for better visibility
    plt.legend(fontsize=12)  # Increase legend font size
    plt.grid(True, linestyle='--', alpha=0.7)  # Lighter grid lines for a cleaner look

    # Improve tick label appearance
    plt.tick_params(axis='both', which='major', labelsize=12)

    # Adjust layout to prevent labels from being cut off
    plt.tight_layout()

    # Save the figure as a high-resolution PNG file (optional)
    plt.savefig('order_bandwidth.pdf', dpi=300, bbox_inches='tight')

# Example usage
plot_bandwidth_vs_message_size_order('result.csv')
