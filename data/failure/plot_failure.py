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
# TITLE_FONTSIZE = 14 # Title removed
LABEL_FONTSIZE = 12 # Adjusted for typical paper needs
TICKS_FONTSIZE = 10
LEGEND_FONTSIZE = 10
LINE_WIDTH = 1.5
GRID_ALPHA = 0.6
GRID_LINESTYLE = ':'
DPI = 300

# --- Plotting Function ---
def plot_real_time_throughput(csv_filename, output_prefix, event_filename=None):
    """
    Reads real-time throughput data and optional event data from CSV files
    and generates a publication-quality plot, normalizing time to start at 0.
    Legend is top-right, title is removed, 'Total' label changed.

    Args:
        csv_filename (str): Path to the input throughput CSV file.
        output_prefix (str): Prefix for the output PDF and PNG files.
        event_filename (str, optional): Path to the failure/reconnect event CSV file.
    """
    try:
        # --- Read Main Throughput Data ---
        data = pd.read_csv(csv_filename)
        print(f"Successfully read {len(data)} data points from {csv_filename}")

        # --- Input Data Validation ---
        if data.empty: raise ValueError(f"Throughput CSV file '{csv_filename}' is empty.")
        if 'Timestamp(ms)' not in data.columns: raise ValueError("Throughput CSV 'Timestamp(ms)' column missing.")
        broker_cols = [col for col in data.columns if col.startswith('Broker_')]
        if not broker_cols:
             broker_cols = [col for col in data.columns if col.isdigit()] # Fallback
             if not broker_cols: raise ValueError("No broker throughput columns found.")
        num_brokers = len(broker_cols)
        try:
             broker_cols.sort(key=lambda name: int(name.replace('Broker_', '').replace('_GBps', '')))
        except ValueError:
             print("Warning: Could not sort broker columns numerically.", file=sys.stderr)
        print(f"Detected data for {num_brokers} brokers.")


        # --- Normalize Time Axis ---
        if not data.empty:
            first_timestamp_ms = data['Timestamp(ms)'].iloc[0]
            print(f"Normalizing time axis. First measurement timestamp: {first_timestamp_ms} ms")
            x_values_sec = (data['Timestamp(ms)'] - first_timestamp_ms) / 1000.0
        else:
            x_values_sec = pd.Series(dtype=float)


        # --- Read and Normalize Event Data (If provided) ---
        event_data = None
        event_lines_added = {}
        if event_filename:
            try:
                event_data = pd.read_csv(event_filename)
                if 'Timestamp(ms)' not in event_data.columns or 'EventDescription' not in event_data.columns:
                     print(f"Warning: Event file '{event_filename}' missing required columns ('Timestamp(ms)', 'EventDescription'). Skipping events.", file=sys.stderr)
                     event_data = None
                elif data.empty:
                     print(f"Warning: Throughput data is empty, cannot normalize event timestamps. Skipping events.", file=sys.stderr)
                     event_data = None
                else:
                     print(f"Read {len(event_data)} events from {event_filename}")
                     event_data['Timestamp(sec)'] = (event_data['Timestamp(ms)'] - first_timestamp_ms) / 1000.0
            except FileNotFoundError:
                print(f"Warning: Event file '{event_filename}' not found. Skipping event markers.", file=sys.stderr)
            except Exception as e:
                print(f"Warning: Error reading or processing event file '{event_filename}': {e}. Skipping event markers.", file=sys.stderr)
                event_data = None


        # --- Plotting Setup ---
        # plt.style.use('seaborn-v0_8-paper') # Optional
        plt.figure(figsize=(FIGURE_WIDTH_INCHES, FIGURE_HEIGHT_INCHES))

        # --- Plot Throughput Lines ---
        colors = plt.rcParams['axes.prop_cycle'].by_key()['color']
        for i, broker_col_name in enumerate(broker_cols):
             y_values = data[broker_col_name]
             label_num = ''.join(filter(str.isdigit, broker_col_name))
             label = f'Broker {label_num}' if label_num else broker_col_name
             plt.plot(x_values_sec, y_values, linewidth=LINE_WIDTH, label=label, color=colors[i % len(colors)], alpha=0.8)

        if 'Total_GBps' in data.columns:
            # Changed label from 'Total' to 'Aggregate'
            plt.plot(x_values_sec, data['Total_GBps'], linewidth=LINE_WIDTH*1.2, linestyle='--', color='k', label='Aggregate', alpha=0.9)


        # --- Add Event Markers (Vertical Lines) ---
        if event_data is not None:
            print("Adding event markers...")
            # ... (same logic as before for determining color/style) ...
            for index, event in event_data.iterrows():
                event_ts_sec = event['Timestamp(sec)']
                if event_ts_sec >= 0: # Only plot events within the normalized time range
                     description = event['EventDescription'].lower()
                     fail_color = 'red'; reconn_ok_color = 'green'; reconn_fail_color = 'orange'
                     line_color = fail_color; linestyle = ':'; event_type = "Failure Detected"
                     if "reconnect success" in description: line_color = reconn_ok_color; linestyle = '-.'; event_type = "Reconnect Success"
                     elif "reconnect fail" in description: line_color = reconn_fail_color; linestyle = ':'; event_type = "Reconnect Fail"
                     elif "fail" not in description: event_type = "Unknown Event"

                     line_label = None
                     if event_type not in event_lines_added: line_label = event_type; event_lines_added[event_type] = True

                     plt.axvline(x=event_ts_sec, color=line_color, linestyle=linestyle, linewidth=1.0, alpha=0.7, label=line_label)


        # --- Customize Plot Appearance ---
        plt.xlabel('Time (seconds)', fontsize=LABEL_FONTSIZE)
        plt.ylabel('Throughput (GB/s)', fontsize=LABEL_FONTSIZE)
        # plt.title('Real-Time Aggregate Throughput during Broker Failure Test', fontsize=TITLE_FONTSIZE) # Title removed
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
             # Changed loc from 'best' to 'upper right'
             plt.legend(by_label.values(), by_label.keys(), fontsize=LEGEND_FONTSIZE, loc='upper right', ncol=ncol)

        plt.tight_layout()

        # --- Save the Plot ---
        pdf_filename = output_prefix + ".pdf"
        try:
            plt.savefig(pdf_filename, dpi=DPI, bbox_inches='tight')
            print(f"Plot saved successfully to {pdf_filename}")
        except Exception as e:
            print(f"Error saving plot files: {e}", file=sys.stderr)

        plt.close()

    # --- Exception Handling --- (Same as before)
    except FileNotFoundError:
        print(f"Error: Input CSV file not found at '{csv_filename}'", file=sys.stderr)
    except KeyError as e:
        print(f"Error: Missing expected column in CSV file: {e}", file=sys.stderr)
        print("  Ensure the CSV has 'Timestamp(ms)' and columns like 'Broker_0_GBps', 'Total_GBps'.", file=sys.stderr)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
    except Exception as e:
        print(f"An unexpected error occurred during plotting: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()


# --- Main Execution --- (Same as before)
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate a publication-quality plot of real-time throughput, optionally marking failure events.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument("csv_file", help="Path to the input throughput CSV file.")
    parser.add_argument("output_prefix", help="Prefix for the output plot files.")
    parser.add_argument("-e", "--events", metavar="EVENT_CSV", default=None,
                        help="Optional path to the failure/reconnect event log CSV file.")

    args = parser.parse_args()

    plot_real_time_throughput(args.csv_file, args.output_prefix, args.events)
