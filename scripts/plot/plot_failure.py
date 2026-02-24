#python3 plot_failure.py real_time_acked_throughput.csv my_failure_plot --events failure_events.csv
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib.patches import FancyArrowPatch
import numpy as np
import argparse
import sys

# --- Configuration ---
FIGURE_WIDTH_INCHES = 7
FIGURE_HEIGHT_INCHES = 4.2
LABEL_FONTSIZE = 12
TICKS_FONTSIZE = 10
LEGEND_FONTSIZE = 8.5
LINE_WIDTH = 1.6
AGGREGATE_WIDTH = 2.0
GRID_ALPHA = 0.25
DPI = 300
THROUGHPUT_THRESHOLD = 0.01
EARLY_FAILURE_FACTOR = 0.8
TAIL_CUTOFF_FACTOR = 0.70

SAFE_COLORS = [
    '#1f77b4',  # blue
    '#2ca02c',  # green
    '#ff7f0e',  # orange
    '#9467bd',  # purple
    '#8c564b',  # brown
    '#17becf',  # cyan
]
FAILED_COLOR = '#d62728'

# Inset magnifier configuration
INSET_PAD_SEC = 0.40       # seconds of context before/after the failure window
INSET_POSITION = [0.35, 0.33, 0.42, 0.58]  # [left, bottom, width, height] in axes fraction


def plot_real_time_throughput(csv_filename, output_prefix, event_filename=None):
    try:
        data = pd.read_csv(csv_filename)
        print(f"Read {len(data)} data points from {csv_filename}")
        if data.empty: raise ValueError("CSV empty")
        if 'Timestamp(ms)' not in data.columns: raise ValueError("Missing Timestamp(ms)")
        broker_cols = sorted(
            [c for c in data.columns if c.startswith('Broker_')],
            key=lambda n: int(n.replace('Broker_', '').replace('_GBps', ''))
        )
        if not broker_cols: raise ValueError("No broker columns")
        num_brokers = len(broker_cols)
        print(f"Detected {num_brokers} brokers: {broker_cols}")

        # Trim trailing zeros
        if 'Total_GBps' in data.columns:
            active = data[data['Total_GBps'] > THROUGHPUT_THRESHOLD].index
            if len(active) > 0:
                data = data.iloc[:active[-1] + 2]
                print(f"Trimmed to {len(data)} active points")

        # Trim tail-off using rolling average
        if 'Total_GBps' in data.columns and len(data) > 5:
            peak_total = data['Total_GBps'].max()
            cutoff = peak_total * TAIL_CUTOFF_FACTOR
            rolling = data['Total_GBps'].rolling(window=3, min_periods=1).mean()
            above = rolling[rolling >= cutoff].index
            if len(above) > 0:
                trim_end = min(above[-1] + 1, len(data))
                data = data.iloc[:trim_end]
                print(f"Trimmed tail-off to {len(data)} points (cutoff {cutoff:.1f} GB/s)")

        # Normalize time
        first_ts = data['Timestamp(ms)'].iloc[0]
        x_sec = (data['Timestamp(ms)'] - first_ts) / 1000.0

        # Read events
        event_data = None
        if event_filename:
            try:
                event_data = pd.read_csv(event_filename)
                if {'Timestamp(ms)', 'EventDescription'}.issubset(event_data.columns):
                    event_data['Timestamp(sec)'] = (event_data['Timestamp(ms)'] - first_ts) / 1000.0
                else:
                    event_data = None
            except Exception:
                event_data = None

        # Detect failed broker
        failed_idx = -1
        min_last_active = float('inf')
        max_last_active = 0.0
        candidate = -1
        for i, col in enumerate(broker_cols):
            active_pts = data[col][data[col] > THROUGHPUT_THRESHOLD]
            if not active_pts.empty:
                last = x_sec[active_pts.last_valid_index()]
                max_last_active = max(max_last_active, last)
                if last < min_last_active:
                    min_last_active = last
                    candidate = i
        if candidate >= 0 and max_last_active > 0 and min_last_active < max_last_active * EARLY_FAILURE_FACTOR:
            failed_idx = candidate
            print(f"Detected failure: Broker index {failed_idx}")

        # Event timestamps
        kill_time = detect_time = recover_time = None
        if event_data is not None:
            for _, ev in event_data.iterrows():
                desc = ev['EventDescription'].lower()
                ts = ev['Timestamp(sec)']
                if "broker kill" in desc and kill_time is None:
                    kill_time = ts
                if "send fail" in desc and detect_time is None:
                    detect_time = ts
                if "reconnect success" in desc:
                    recover_time = ts

        # --- Helper: draw all series on an axes ---
        def draw_series(target_ax, show_labels=True):
            safe_idx = 0
            for i, col in enumerate(broker_cols):
                label_num = col.replace('Broker_', '').replace('_GBps', '')
                is_failed = (i == failed_idx)
                if is_failed:
                    color = FAILED_COLOR
                    label = f'Broker {label_num} (failed)' if show_labels else '_nolegend_'
                    zorder, alpha = 3, 0.9
                else:
                    color = SAFE_COLORS[safe_idx % len(SAFE_COLORS)]
                    safe_idx += 1
                    label = f'Broker {label_num}' if show_labels else '_nolegend_'
                    zorder, alpha = 2, 0.8
                target_ax.plot(x_sec, data[col], linewidth=LINE_WIDTH, label=label,
                               color=color, alpha=alpha, zorder=zorder)

            if 'Total_GBps' in data.columns:
                label = 'Aggregate' if show_labels else '_nolegend_'
                target_ax.plot(x_sec, data['Total_GBps'], linewidth=AGGREGATE_WIDTH,
                               linestyle='--', color='black', label=label,
                               alpha=0.8, zorder=4)

        def draw_events(target_ax, show_labels=True, lw=1.0):
            if kill_time is not None and recover_time is not None:
                target_ax.axvspan(kill_time, recover_time, alpha=0.10, color='red', zorder=0)
            if kill_time is not None:
                label = 'Broker Failure' if show_labels else '_nolegend_'
                target_ax.axvline(kill_time, color='darkred', linestyle='--', linewidth=lw,
                                  alpha=0.8, label=label, zorder=5)
            if detect_time is not None:
                label = 'Rerouted' if show_labels else '_nolegend_'
                target_ax.axvline(detect_time, color='#2ca02c', linestyle='-.', linewidth=lw,
                                  alpha=0.7, label=label, zorder=5)

        # --- Main plot ---
        fig, ax = plt.subplots(figsize=(FIGURE_WIDTH_INCHES, FIGURE_HEIGHT_INCHES))
        draw_series(ax, show_labels=True)
        draw_events(ax, show_labels=True)

        # Axes
        ax.set_xlabel('Time (seconds)', fontsize=LABEL_FONTSIZE)
        ax.set_ylabel('Throughput (GB/s)', fontsize=LABEL_FONTSIZE)
        ax.tick_params(axis='both', labelsize=TICKS_FONTSIZE)
        ax.grid(True, which='major', linewidth=0.3, alpha=GRID_ALPHA)
        x_max = x_sec.max() * 1.02 if not x_sec.empty else 1
        ax.set_xlim(0, x_max)
        y_max = data['Total_GBps'].max() if 'Total_GBps' in data.columns else data[broker_cols].max().max()
        ax.set_ylim(0, y_max * 1.12)
        ax.yaxis.set_major_locator(ticker.MultipleLocator(2))
        ax.yaxis.set_minor_locator(ticker.MultipleLocator(1))

        # Legend
        handles, labels = ax.get_legend_handles_labels()
        by_label = dict(zip(labels, handles))
        ncol = 2 if len(by_label) > 5 else 1
        ax.legend(by_label.values(), by_label.keys(), fontsize=LEGEND_FONTSIZE,
                  loc='upper right', ncol=ncol, framealpha=0.9,
                  handlelength=1.5, columnspacing=1.0)

        # --- Magnifier inset ---
        if kill_time is not None and recover_time is not None:
            center_t = (kill_time + recover_time) / 2.0
            span = max(recover_time - kill_time, 0.05)
            zoom_x0 = center_t - INSET_PAD_SEC
            zoom_x1 = center_t + INSET_PAD_SEC + span

            # Gather y-values in the zoom window for auto y-limits
            mask = (x_sec >= zoom_x0) & (x_sec <= zoom_x1)
            if mask.any():
                all_vals = []
                for col in broker_cols:
                    all_vals.extend(data.loc[mask, col].values)
                if 'Total_GBps' in data.columns:
                    all_vals.extend(data.loc[mask, 'Total_GBps'].values)
                zoom_y0 = 0
                zoom_y1 = max(all_vals) * 1.15

                axins = ax.inset_axes(INSET_POSITION)
                draw_series(axins, show_labels=False)
                draw_events(axins, show_labels=False, lw=1.2)

                axins.set_xlim(zoom_x0, zoom_x1)
                axins.set_ylim(zoom_y0, zoom_y1)
                axins.tick_params(axis='both', labelsize=7)
                axins.grid(True, which='major', linewidth=0.2, alpha=0.3)
                axins.set_xlabel('')
                axins.set_ylabel('')

                # Format inset ticks as milliseconds relative to kill
                def fmt_ms(val, _):
                    ms = (val - kill_time) * 1000
                    if abs(ms) < 0.5:
                        return '0'
                    return f'{ms:+.0f}'
                axins.xaxis.set_major_formatter(ticker.FuncFormatter(fmt_ms))
                axins.xaxis.set_major_locator(ticker.MaxNLocator(5, integer=False))
                axins.set_xlabel('ms from failure', fontsize=7, labelpad=1)

                for spine in axins.spines.values():
                    spine.set_edgecolor('#555555')
                    spine.set_linewidth(1.2)

                # Connector lines from main plot to inset
                ax.indicate_inset_zoom(axins, edgecolor='#555555', linewidth=0.8, alpha=0.6)

                # Detection delay annotation inside inset
                delay_ms = (recover_time - kill_time) * 1000
                mid_t = (kill_time + recover_time) / 2.0
                axins.annotate(
                    f'{delay_ms:.0f} ms',
                    xy=(mid_t, zoom_y1 * 0.92),
                    fontsize=7, fontweight='bold', ha='center', va='top',
                    color='darkred',
                    bbox=dict(boxstyle='round,pad=0.2', fc='white', ec='darkred', alpha=0.85, lw=0.6)
                )

        fig.tight_layout(pad=0.5)

        for fmt in ['pdf', 'png']:
            fn = f"{output_prefix}.{fmt}"
            fig.savefig(fn, dpi=DPI, bbox_inches='tight')
            print(f"Saved {fn}")
        plt.close(fig)

    except FileNotFoundError:
        print(f"Error: File not found: {csv_filename}", file=sys.stderr)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback; traceback.print_exc()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Plot real-time throughput with failure events.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument("csv_file", help="Throughput CSV file")
    parser.add_argument("output_prefix", help="Output file prefix")
    parser.add_argument("-e", "--events", metavar="CSV", help="Event CSV file")
    args = parser.parse_args()
    plot_real_time_throughput(args.csv_file, args.output_prefix, args.events)
