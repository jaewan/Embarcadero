import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import argparse
import os # To help generate labels from filenames

# --- Configuration (Plot appearance settings) ---
FIGURE_WIDTH_INCHES = 6
FIGURE_HEIGHT_INCHES = 4
TITLE_FONTSIZE = 14
LABEL_FONTSIZE = 12
TICKS_FONTSIZE = 10
LEGEND_FONTSIZE = 10 # Font size for the legend
LINE_WIDTH = 1.5
GRID_ALPHA = 0.6
GRID_LINESTYLE = '--'

# --- Plotting Function (Modified for multiple CDFs) ---

def plot_multiple_latency_cdfs(csv_filenames, labels, output_prefix):
    """
    Reads latency CDF data from multiple CSV files and generates a single
    publication-quality plot comparing them.

    Args:
        csv_filenames (list): A list of paths to the input CSV files.
                              Each file expected columns: 'Latency_us', 'CumulativeProbability'
        labels (list): A list of labels for the legend, corresponding to each csv_filename.
        output_prefix (str): Prefix for the output plot files (e.g., 'comparison_cdf').
                               Generates PREFIX.pdf and PREFIX.png.
    """
    if len(csv_filenames) != len(labels):
        raise ValueError("Number of CSV files must match number of labels.")

    # --- Create the Plot Figure and Axes ---
    # Do this *once* before plotting the lines
    plt.figure(figsize=(FIGURE_WIDTH_INCHES, FIGURE_HEIGHT_INCHES))

    # Variables to track overall latency range across all files
    min_overall_latency = float('inf')
    max_overall_latency = float('-inf')
    all_data_loaded = True # Flag to track if all files loaded successfully

    # --- Plot each CDF ---
    # Matplotlib will automatically cycle through colors.
    # You can define specific colors/linestyles if needed:
    # colors = plt.cm.viridis(np.linspace(0, 1, len(csv_filenames))) # Example colormap
    # linestyles = ['-', '--', ':'] # Example linestyles

    for i, (csv_filename, label) in enumerate(zip(csv_filenames, labels)):
        try:
            # Read the data using pandas
            data = pd.read_csv(csv_filename)
            print(f"Reading data for '{label}' from {csv_filename}...")

            # Validate expected columns
            if 'Latency_us' not in data.columns or 'CumulativeProbability' not in data.columns:
                print(f"Warning: Skipping {csv_filename}. Missing required columns ('Latency_us', 'CumulativeProbability').")
                continue # Skip this file

            latency_us = data['Latency_us']
            probability = data['CumulativeProbability']

            # Plot this CDF data with its label
            # Add marker=... if you want markers, though usually not needed for CDFs
            plt.plot(latency_us, probability, linewidth=LINE_WIDTH, label=label) # Use label for legend
                     # color=colors[i], linestyle=linestyles[i % len(linestyles)]) # Optional manual style

            # Update overall min/max latency (considering only positive latency for log scale)
            current_min = latency_us[latency_us > 0].min() if (latency_us > 0).any() else float('inf')
            current_max = latency_us.max()
            min_overall_latency = min(min_overall_latency, current_min)
            max_overall_latency = max(max_overall_latency, current_max)

        except FileNotFoundError:
            print(f"Error: Input CSV file not found at '{csv_filename}'. Skipping this file.")
            all_data_loaded = False # Mark that at least one file failed
        except Exception as e:
            print(f"An error occurred processing {csv_filename}: {e}. Skipping this file.")
            all_data_loaded = False # Mark that at least one file failed

    # --- Check if any data was actually plotted ---
    if min_overall_latency == float('inf') or max_overall_latency == float('-inf'):
         print("Error: No valid data could be plotted from the provided files.")
         plt.close() # Close the empty figure
         return

    # --- Customize Appearance (after all lines are plotted) ---
    plt.xscale('log')
    plt.xlabel("Latency (microseconds)", fontsize=LABEL_FONTSIZE)
    plt.ylabel("Cumulative Probability (CDF)", fontsize=LABEL_FONTSIZE)
    plt.title("Comparison of End-to-End Latency CDFs", fontsize=TITLE_FONTSIZE) # Adjust title
    plt.ylim(0, 1.05)

    # Set X limits based on the overall range found
    plt.xlim(left=min_overall_latency * 0.8, right=max_overall_latency * 1.2)

    plt.xticks(fontsize=TICKS_FONTSIZE)
    plt.yticks(fontsize=TICKS_FONTSIZE)
    plt.grid(True, which='major', linestyle=GRID_LINESTYLE, alpha=GRID_ALPHA)
    plt.tight_layout()

    # --- Add Legend ---
    plt.legend(fontsize=LEGEND_FONTSIZE, loc='best') # 'loc' controls position

    # --- Save the Plot ---
    pdf_filename = output_prefix + ".pdf"

    try:
        plt.savefig(pdf_filename, dpi=300, bbox_inches='tight')
        print(f"Combined plot saved successfully to {pdf_filename}")
    except Exception as e:
         print(f"Error saving plot files: {e}")

    # --- Close the plot figure ---
    plt.close()

    # --- Display the Plot (Optional, commented out) ---
    # plt.show()


# --- Main execution block ---
if __name__ == "__main__":
    # Set up argument parser
    parser = argparse.ArgumentParser(
        description="Generate a single publication-quality plot comparing 3 latency CDFs.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )

    # Define command-line arguments (now 4 positional)
    parser.add_argument("input_csv1", help="Path to the first input CSV file.")
    parser.add_argument("input_csv2", help="Path to the second input CSV file.")
    parser.add_argument("input_csv3", help="Path to the third input CSV file.")
    parser.add_argument("output_prefix", help="Prefix for the output plot files (e.g., 'comparison_cdf').")

    # Parse arguments from the command line
    args = parser.parse_args()

    # Create list of input files
    input_files = [args.input_csv1, args.input_csv2, args.input_csv3]

    # Generate labels for the legend (using filename without path/extension)
    # You can customize this logic or pass labels explicitly if needed
    labels = [os.path.basename(f).rsplit('.', 1)[0] for f in input_files]
    print(f"Using labels for legend: {labels}")

    # Run the plotting function with the provided arguments
    plot_multiple_latency_cdfs(input_files, labels, args.output_prefix)
