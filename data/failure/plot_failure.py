#python3 plot_failure.py real_time_acked_throughput.csv my_failure_plot --events failure_events.csv
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import argparse
import os
import sys

# --- Configuration ---
FIGURE_WIDTH_INCHES = 8
FIGURE_HEIGHT_INCHES = 5
LABEL_FONTSIZE = 12
TICKS_FONTSIZE = 10
LEGEND_FONTSIZE = 10
LINE_WIDTH = 1.5
GRID_ALPHA = 0.6
GRID_LINESTYLE = ':'
DPI = 300
THROUGHPUT_THRESHOLD = 0.01
EARLY_FAILURE_FACTOR = 0.8

# --- Define Color Palettes ---
# List of visually distinct colors EXCLUDING standard red shades
# Using Tableau10 names/hex codes as a base, skipping 'tab:red' (#d62728)
SAFE_COLORS = [
    '#1f77b4',  # tab:blue
    '#2ca02c',  # tab:green
    '#ff7f0e',  # tab:orange
    '#9467bd',  # tab:purple
    '#8c564b',  # tab:brown
    '#e377c2',  # tab:pink
    '#7f7f7f',  # tab:gray
    '#bcbd22',  # tab:olive
    '#17becf'   # tab:cyan
    # Add more distinct non-red colors here if you have > 9 brokers
]
# Define the specific color for the FAILED broker line
FAILED_COLOR = '#d62728' # Use the standard 'tab:red' explicitly for failure
# Or use a brighter red if preferred: FAILED_COLOR = '#FF0000'


# --- Plotting Function ---
def plot_real_time_throughput(csv_filename, output_prefix, event_filename=None):
    """
    Reads real-time throughput data and optional event data from CSV files
    and generates a publication-quality plot, normalizing time to start at 0,
    highlighting the first broker that fails early in a specific red color.
    Other brokers use colors from a safe, non-red palette.

    Args:
        csv_filename (str): Path to the input throughput CSV file.
        output_prefix (str): Prefix for the output PDF and PNG files.
        event_filename (str, optional): Path to the failure/reconnect event CSV file.
    """
    try:
        # --- Read Main Throughput Data ---
        data = pd.read_csv(csv_filename)
        print(f"Successfully read {len(data)} data points from {csv_filename}")
        # ... (Input Data Validation) ...
        if data.empty: raise ValueError(f"Throughput CSV file '{csv_filename}' is empty.")
        if 'Timestamp(ms)' not in data.columns: raise ValueError("Throughput CSV 'Timestamp(ms)' column missing.")
        broker_cols = [col for col in data.columns if col.startswith('Broker_')]
        if not broker_cols:
             broker_cols = [col for col in data.columns if col.isdigit()]
             if not broker_cols: raise ValueError("No broker throughput columns found.")
        num_brokers = len(broker_cols)
        try:
             broker_cols.sort(key=lambda name: int(name.replace('Broker_', '').replace('_GBps', '')))
        except ValueError:
             print("Warning: Could not sort broker columns numerically.", file=sys.stderr)
        print(f"Detected data for {num_brokers} brokers: {broker_cols}")

        # --- Normalize Time Axis ---
        x_values_sec = pd.Series(dtype=float)
        first_timestamp_ms = 0
        if not data.empty:
            first_timestamp_ms = data['Timestamp(ms)'].iloc[0]
            x_values_sec = (data['Timestamp(ms)'] - first_timestamp_ms) / 1000.0

        # --- Read and Normalize Event Data ---
        event_data = None
        event_lines_added = {}
        if event_filename:
            # ... (try/except block to read and normalize event_data) ...
            try:
                event_data = pd.read_csv(event_filename)
                if 'Timestamp(ms)' in event_data.columns and 'EventDescription' in event_data.columns and not data.empty:
                    event_data['Timestamp(sec)'] = (event_data['Timestamp(ms)'] - first_timestamp_ms) / 1000.0
                else: event_data = None
            except Exception: event_data = None # Simplified error handling example

        # --- Pre-analysis to Detect Early Failure ---
        failed_broker_index = -1; min_last_active_time_sec = float('inf'); max_last_active_time_sec = 0.0; potential_failed_broker_index = -1
        if not x_values_sec.empty:
            max_time_sec = x_values_sec.max()
            for i, broker_col_name in enumerate(broker_cols):
                y_values = data[broker_col_name]
                active_points = y_values[y_values > THROUGHPUT_THRESHOLD]
                if not active_points.empty:
                     last_active_index = active_points.last_valid_index()
                     if last_active_index is not None and last_active_index in x_values_sec.index:
                          last_active_time = x_values_sec[last_active_index]
                          max_last_active_time_sec = max(max_last_active_time_sec, last_active_time)
                          if last_active_time < min_last_active_time_sec: min_last_active_time_sec = last_active_time; potential_failed_broker_index = i
            if potential_failed_broker_index != -1 and max_last_active_time_sec > 0 and \
               min_last_active_time_sec < (max_last_active_time_sec * EARLY_FAILURE_FACTOR):
                 failed_broker_index = potential_failed_broker_index
                 print(f"*** Detected early failure for Broker index {failed_broker_index} ***")
            else:
                 print("--- No significant early broker failure detected. ---")
        print(f"DEBUG: Final failed_broker_index = {failed_broker_index}")


        # --- Plotting Setup ---
        plt.figure(figsize=(FIGURE_WIDTH_INCHES, FIGURE_HEIGHT_INCHES))
        plt.style.use('seaborn-v0_8-paper') # Optional

        # --- Plot Throughput Lines ---
        for i, broker_col_name in enumerate(broker_cols):
             y_values = data[broker_col_name]
             label_num = ''.join(filter(str.isdigit, broker_col_name))
             label = f'Broker {label_num}' if label_num else broker_col_name

             # --- Determine plot color ---
             is_failed = (i == failed_broker_index)
             if is_failed:
                 line_color = FAILED_COLOR # Use the specific red for failed broker
                 label += ' (Failed)'
                 line_zorder = 3
                 line_alpha = 0.9
             else:
                 # Use colors from our SAFE_COLORS list, cycling through
                 color_index = i % len(SAFE_COLORS)
                 # Adjust index if we skipped the failed broker's potential default color index (optional, makes colors more stable)
                 if failed_broker_index != -1 and i > failed_broker_index:
                     color_index = (i -1) % len(SAFE_COLORS) # Simple shift after failed index
                 line_color = SAFE_COLORS[color_index]
                 line_zorder = 2
                 line_alpha = 0.8

             print(f"DEBUG: Plotting Broker index {i} ({label}), Assigned Color={line_color}, IsFailed={is_failed}")
             plt.plot(x_values_sec, y_values, linewidth=LINE_WIDTH, label=label, color=line_color, alpha=line_alpha, zorder=line_zorder)

        # Plot Aggregate Line
        if 'Total_GBps' in data.columns:
            plt.plot(x_values_sec, data['Total_GBps'], linewidth=LINE_WIDTH*1.2, linestyle='--', color='k', label='Aggregate', alpha=0.9, zorder=4)


        # --- Add Event Markers ---
        if event_data is not None:
             print("Adding event markers...")
             for index, event in event_data.iterrows():
                event_ts_sec = event['Timestamp(sec)']
                if event_ts_sec >= 0:
                     description = event['EventDescription'].lower()
                     fail_color = 'red'; reconn_ok_color = 'green'; reconn_fail_color = 'orange'
                     line_color = fail_color; linestyle = ':'; event_type = "Failure Detected"
                     if "reconnect success" in description: line_color = reconn_ok_color; linestyle = '-.'; event_type = "Reconnect Success"
                     elif "reconnect fail" in description: line_color = reconn_fail_color; linestyle = ':'; event_type = "Reconnect Fail"
                     elif "fail" not in description: event_type = "Unknown Event"
                     line_label = None
                     if event_type not in event_lines_added: line_label = event_type; event_lines_added[event_type] = True
                     plt.axvline(x=event_ts_sec, color=line_color, linestyle=linestyle, linewidth=1.0, alpha=0.7, label=line_label, zorder=1)


        # --- Customize Plot Appearance ---
        plt.xlabel('Time (seconds)', fontsize=LABEL_FONTSIZE)
        plt.ylabel('Throughput (GB/s)', fontsize=LABEL_FONTSIZE)
        plt.xticks(fontsize=TICKS_FONTSIZE)
        plt.yticks(fontsize=TICKS_FONTSIZE)
        plt.grid(True, which='major', linestyle=GRID_LINESTYLE, linewidth=0.5, alpha=GRID_ALPHA)
        plt.xlim(left=0)
        plt.ylim(bottom=0)

        # --- Add Legend ---
        handles, labels = plt.gca().get_legend_handles_labels()
        if handles:
             by_label = dict(zip(labels, handles))
             ncol = 1
             if len(by_label) > 6: ncol = 2
             if len(by_label) > 12: ncol = 3
             plt.legend(by_label.values(), by_label.keys(), fontsize=LEGEND_FONTSIZE, loc='upper right', ncol=ncol)


        plt.tight_layout()

        # --- Save the Plot ---
        pdf_filename = output_prefix + ".png"
        try:
            plt.savefig(pdf_filename, dpi=DPI, bbox_inches='tight')
            print(f"Plot saved successfully to {pdf_filename}")
        except Exception as e:
            print(f"Error saving plot files: {e}", file=sys.stderr)
        plt.close()


    # --- Exception Handling --- (Same as before)
    except FileNotFoundError: print(f"Error: Input CSV file not found at '{csv_filename}'", file=sys.stderr)
    except KeyError as e: print(f"Error: Missing column: {e}", file=sys.stderr)
    except ValueError as e: print(f"Error: {e}", file=sys.stderr)
    except Exception as e: print(f"An unexpected error: {e}", file=sys.stderr); import traceback; traceback.print_exc()


# --- Main Execution --- (Same as before)
if __name__ == "__main__":
    # ... (ArgumentParser setup including optional --events) ...
    parser = argparse.ArgumentParser(
        description="Generate a publication-quality plot of real-time throughput, optionally marking failure events and highlighting the failed broker.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument("csv_file", help="Path to the input throughput CSV file.")
    parser.add_argument("output_prefix", help="Prefix for the output plot files.")
    parser.add_argument("-e", "--events", metavar="EVENT_CSV", default=None,
                        help="Optional path to the failure/reconnect event log CSV file.")

    args = parser.parse_args()

    plot_real_time_throughput(args.csv_file, args.output_prefix, args.events)
