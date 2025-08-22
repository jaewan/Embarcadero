import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import argparse
import os
import matplotlib.ticker as mticker # Import the ticker module
# from matplotlib.ticker import FuncFormatter # No longer needed

# --- Configuration (Plot appearance settings) ---
FIGURE_WIDTH_INCHES = 6
FIGURE_HEIGHT_INCHES = 4
TITLE_FONTSIZE = 14
LABEL_FONTSIZE = 12
TICKS_FONTSIZE = 10
LEGEND_FONTSIZE = 9 # Slightly smaller legend font if needed for 6 entries
LEGEND_COLUMNS = 2 # Arrange legend in columns if needed
LINE_WIDTH = 1.5
GRID_ALPHA = 0.6
GRID_LINESTYLE = '--'

# --- System and Rate Definitions ---
# Define the systems and their corresponding base filenames
SYSTEMS = {
    "Corfu": "CORFU_latency.csv",
    "Embarcadero": "EMBARCADERO_latency.csv",
    "Scalog": "SCALOG_latency.csv"
}

# Define the rates, their directories, and the desired linestyles
RATES = {
    "Steady": {'dir': 'steady', 'linestyle': '-'}, # Solid line for steady
    "Bursty": {'dir': 'bursty', 'linestyle': '--'}  # Dashed line for bursty
}

# Define consistent colors for each system
SYSTEM_COLORS = {
    "Corfu": "tab:blue",
    "Embarcadero": "tab:orange",
    "Scalog": "tab:green"
}


# --- Plotting Function (Modified for merged steady/bursty) ---

def plot_merged_latency_cdfs(output_prefix):
    """
    Reads latency CDF data for multiple systems under steady and bursty rates
    from predefined directories and generates a single merged plot.

    Assumes data files are located like:
    ./steady/CORFU_latency.csv
    ./bursty/CORFU_latency.csv
    ./steady/EMBARCADERO_latency.csv
    etc.

    Args:
        output_prefix (str): Prefix for the output plot files (e.g., 'comparison_cdf').
                               Generates PREFIX.pdf and PREFIX.png.
    """

    # --- Create the Plot Figure and Axes ---
    plt.figure(figsize=(FIGURE_WIDTH_INCHES, FIGURE_HEIGHT_INCHES))

    # Variables to track overall latency range across all files
    min_overall_latency = float('inf')
    max_overall_latency = float('-inf')
    plotted_anything = False # Flag to track if at least one curve was plotted

    # --- Plot each system and rate ---
    for rate_name, rate_info in RATES.items():         # New outer loop
        for system_name, base_filename in SYSTEMS.items():

            csv_filename = os.path.join(rate_info['dir'], base_filename)
            legend_label = f"{system_name} ({rate_name})"
            linestyle = rate_info['linestyle']
            color = SYSTEM_COLORS.get(system_name, None) # Get color or None

            try:
                # Read the data using pandas
                data = pd.read_csv(csv_filename)
                print(f"Reading data for '{legend_label}' from {csv_filename}...")

                # Validate expected columns
                if 'Latency_us' not in data.columns or 'CumulativeProbability' not in data.columns:
                    print(f"Warning: Skipping {csv_filename}. Missing required columns ('Latency_us', 'CumulativeProbability').")
                    continue # Skip this file/rate combination

                latency_us = data['Latency_us']
                probability = data['CumulativeProbability']

                # --- Plot this specific CDF ---
                plt.plot(latency_us, probability,
                         linewidth=LINE_WIDTH,
                         label=legend_label,
                         color=color,          # Use system color
                         linestyle=linestyle)  # Use rate linestyle

                plotted_anything = True # Mark that we have plotted at least one line

                # Update overall min/max latency (considering only positive latency for log scale)
                current_min = latency_us[latency_us > 0].min() if (latency_us > 0).any() else float('inf')
                current_max = latency_us.max()
                min_overall_latency = min(min_overall_latency, current_min)
                max_overall_latency = max(max_overall_latency, current_max)

            except FileNotFoundError:
                print(f"Warning: Input CSV file not found at '{csv_filename}'. Skipping this entry.")
                # Continue processing other files/rates
            except Exception as e:
                print(f"An error occurred processing {csv_filename}: {e}. Skipping this entry.")
                # Continue processing other files/rates


    # --- Check if any data was actually plotted ---
    if not plotted_anything:
          print("Error: No valid data could be plotted from any files.")
          plt.close() # Close the empty figure
          return

    # --- Customize Appearance (after all lines are plotted) ---
    #plt.xscale('log')

    # Use r'$\mu s$' for the microsecond symbol
    plt.xlabel(r'Latency ($\mu s$)', fontsize=LABEL_FONTSIZE)

    plt.ylabel("Cumulative Probability (CDF)", fontsize=LABEL_FONTSIZE)
    # plt.title("Latency CDF Comparison: Steady vs. Bursty Rate", fontsize=TITLE_FONTSIZE) # Optional title
    plt.ylim(0, 1.05)

    # Set X limits based on the overall range found (ensure min is positive for log)
    safe_min_latency = max(min_overall_latency, 1e-1) # Avoid zero or negative for log limit
    #plt.xlim(left=safe_min_latency * 0.8, right=max_overall_latency * 1.2)
    #plt.xlim(left=1e5, right=max_overall_latency * 1.2)
    plt.xlim(left=0, right=max_overall_latency * 1.2)

    # --- Explicit Tick Control for Log Scale ---
    #ax = plt.gca() # Get the current axes

    # Set major ticks at powers of 10 (1, 10, 100, 1000...)
    #ax.xaxis.set_major_locator(mticker.LogLocator(base=10.0, subs=(1.0,)))

    # Use LogFormatterMathtext to display powers of 10 (e.g., 10^3, 10^6)
    #ax.xaxis.set_major_formatter(mticker.LogFormatterMathtext(base=10.0))

    # Minor ticks setup remains the same
    #ax.xaxis.set_minor_locator(mticker.LogLocator(base=10.0, subs=np.arange(2, 10) * .1))
    #ax.xaxis.set_minor_formatter(mticker.NullFormatter()) # No labels on minor ticks

    # --- End Explicit Tick Control ---

    plt.xticks(fontsize=TICKS_FONTSIZE)
    plt.yticks(fontsize=TICKS_FONTSIZE)

    # Grid setup remains the same
    plt.grid(True, which='major', linestyle=GRID_LINESTYLE, alpha=GRID_ALPHA)
    plt.grid(True, which='minor', linestyle=':', alpha=GRID_ALPHA * 0.5)

    plt.tight_layout(rect=[0, 0, 1, 0.97]) # Adjust layout slightly if title is used or legend is large

    # --- Add Legend ---
    # May need multiple columns if 6 entries make it too tall
    plt.legend(fontsize=LEGEND_FONTSIZE, loc='best', ncol=LEGEND_COLUMNS)

    # --- Save the Plot ---
    pdf_filename = output_prefix + ".pdf"
    png_filename = output_prefix + ".png"

    try:
        plt.savefig(pdf_filename, dpi=300, bbox_inches='tight')
        plt.savefig(png_filename, dpi=300, bbox_inches='tight')
        print(f"Merged plot saved successfully to {pdf_filename} and {png_filename}")
    except Exception as e:
         print(f"Error saving plot files: {e}")

    # --- Close the plot figure ---
    plt.close()


# --- Main execution block ---
if __name__ == "__main__":
    # Set up argument parser
    parser = argparse.ArgumentParser(
        description="Generate a single plot comparing latency CDFs for multiple systems under steady and bursty rates.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )

    # Define command-line arguments (only output prefix needed now)
    parser.add_argument("output_prefix", help="Prefix for the output plot files (e.g., 'rate_comparison_cdf').")

    # Parse arguments from the command line
    args = parser.parse_args()

    # Run the plotting function
    plot_merged_latency_cdfs(args.output_prefix)
