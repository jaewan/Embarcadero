#!/usr/bin/env python3
import argparse
import csv
from collections import defaultdict
import statistics as stats

def load_summary(path):
    rows = []
    with open(path, newline='') as f:
        r = csv.reader(f)
        for row in r:
            if not row:
                continue
            # Try to parse key,value pairs; if last value has no key, treat as 'flush'
            if row[0] == 'brokers' and len(row) > 1 and row[1].isdigit():
                # Handle rows that start with 'brokers,<num>,clients_per_broker,...,flush'
                d = {}
                i = 0
                while i + 1 < len(row):
                    k = row[i].strip()
                    v = row[i+1].strip()
                    d[k] = v
                    i += 2
                rows.append(d)
            elif row[0] == 'brokers' and len(row) > 1 and not row[1].isdigit():
                # Header row like: brokers,clients_per_broker,...,flush -> skip
                continue
            elif len(row) % 2 == 0:
                # Even count: assume strict key,value pairs
                d = {}
                ok = True
                for i in range(0, len(row), 2):
                    k = row[i].strip()
                    v = row[i+1].strip()
                    if not k:
                        ok = False
                        break
                    d[k] = v
                if ok:
                    rows.append(d)
            else:
                # Odd count: last token is 'flush' value
                d = {}
                ok = True
                for i in range(0, len(row)-1, 2):
                    k = row[i].strip()
                    v = row[i+1].strip()
                    if not k:
                        ok = False
                        break
                    d[k] = v
                if ok:
                    d['flush'] = row[-1].strip()
                    rows.append(d)
    return rows

def group_by(rows, key):
    g = defaultdict(list)
    for row in rows:
        g[row[key]].append(row)
    return g

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--summary_csv', required=True)
    ap.add_argument('--out_txt', required=True)
    ap.add_argument('--out_csv', required=False)
    ap.add_argument('--out_png', required=False)
    ap.add_argument('--out_pdf', required=False)
    ap.add_argument('--out_svg', required=False)
    ap.add_argument('--title', required=False, default='Ordering Throughput vs Brokers')
    ap.add_argument('--dpi', type=int, required=False, default=300)
    # Optional latency outputs
    ap.add_argument('--lat_png', required=False)
    ap.add_argument('--lat_pdf', required=False)
    ap.add_argument('--lat_svg', required=False)
    ap.add_argument('--lat_csv', required=False)
    # Optional contention outputs
    ap.add_argument('--cont_png', required=False)
    ap.add_argument('--cont_pdf', required=False)
    ap.add_argument('--cont_svg', required=False)
    ap.add_argument('--cont_csv', required=False)
    args = ap.parse_args()

    rows = load_summary(args.summary_csv)
    # Normalize numeric fields
    for r in rows:
        for k in ['brokers','clients_per_broker','message_size','batch_size','gap_ratio','dup_ratio','target_msgs_per_s','throughput_avg','total_batches','total_ordered','total_skipped','total_dups','atomic_fetch_add','claimed_msgs','total_lock_ns','total_assign_ns','flush','p50_ns','p90_ns','p99_ns']:
            if k in r:
                try:
                    if k in ['gap_ratio','dup_ratio','target_msgs_per_s','throughput_avg']:
                        r[k] = float(r[k])
                    else:
                        r[k] = int(float(r[k]))
                except Exception:
                    pass

    # Some rows may lack 'flush' due to formatting; treat missing as 0
    for r in rows:
        if 'flush' not in r:
            r['flush'] = '0'
    # Filter to valid numeric broker rows
    rows = [r for r in rows if 'brokers' in r and str(r['brokers']).isdigit() and 'throughput_avg' in r]
    by_flush = group_by(rows, 'flush')
    lines = []
    for flush_value, srows in sorted(by_flush.items(), key=lambda kv: int(kv[0])):
        srows_sorted = sorted(srows, key=lambda r: int(r['brokers']))
        # If multiple repeats exist per (brokers,flush), aggregate by median to smooth step artifacts
        agg = {}
        for r in srows_sorted:
            b = int(r['brokers'])
            agg.setdefault(b, []).append(float(r['throughput_avg']))
        xs = sorted(agg.keys())
        ys = [float(stats.median(agg[b])) for b in xs]
        lines.append((int(flush_value), xs, ys))

    # Write simple text plot data for external plotting and a short analysis
    with open(args.out_txt, 'w') as out:
        out.write('Throughput vs Brokers (msgs/s)\n')
        for flush, xs, ys in lines:
            out.write(f'flush={flush}:\n')
            for x, y in zip(xs, ys):
                out.write(f'  brokers={x}, throughput_avg={y:.0f}\n')

        # Derived metrics at max brokers
        max_brokers = max(r['brokers'] for r in rows)
        at_max = [r for r in rows if r['brokers'] == max_brokers]
        out.write(f'\nAt brokers={max_brokers}:\n')
        for r in sorted(at_max, key=lambda r: r['flush']):
            ops_per_s = 0.0
            # Assume duration_s ~ 10s in sweep; infer from counts if available
            duration_s = 10.0
            try:
                ops_per_s = float(r['atomic_fetch_add']) / duration_s
            except Exception:
                pass
            out.write(f"flush={r['flush']}, throughput_avg={r['throughput_avg']:.0f}, atomic_fetch_add/s={ops_per_s:.0f}, total_skipped={r['total_skipped']}, total_dups={r['total_dups']}\n")

        # Simple trend summary
        for flush, xs, ys in lines:
            if len(xs) >= 2:
                slope = (ys[-1] - ys[0]) / max(1, xs[-1] - xs[0])
            else:
                slope = 0.0
            out.write(f'flush={flush} slope msgs/s per broker ~ {slope:.1f}\n')

    # Optional CSV for easy plotting elsewhere
    if args.out_csv:
        # unify brokers set
        broker_set = sorted({x for _, xs, _ in lines for x in xs})
        flush_map = {flush: dict(zip(xs, ys)) for flush, xs, ys in lines}
        with open(args.out_csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['brokers', 'throughput_flush0', 'throughput_flush1'])
            for b in broker_set:
                y0 = flush_map.get(0, {}).get(b, '')
                y1 = flush_map.get(1, {}).get(b, '')
                w.writerow([b, y0, y1])

    # Optional PNG plot
    if args.out_png or args.out_pdf or args.out_svg:
        try:
            import matplotlib
            matplotlib.use('Agg')
            import matplotlib.pyplot as plt
            plt.figure(figsize=(6.5,3.8))
            for _flush, xs, ys in lines:
                # Plot lines without a legend label (hide flush in legend)
                plt.plot(xs, ys, marker='o')
            plt.xlabel('Brokers')
            plt.ylabel('Throughput (msgs/s)')
            plt.title(args.title)
            plt.grid(True, linestyle='--', alpha=0.3)
            # Intentionally hide legend to avoid showing flush series labels
            plt.tight_layout()
            if args.out_png:
                plt.savefig(args.out_png, dpi=args.dpi)
            if args.out_pdf:
                plt.savefig(args.out_pdf, dpi=args.dpi)
            if args.out_svg:
                plt.savefig(args.out_svg)
        except Exception:
            pass

    # Latency percentiles vs brokers (aggregate across repeats/flush by median)
    if args.lat_png or args.lat_pdf or args.lat_svg or args.lat_csv:
        # Build per-broker lists
        lat = {}
        for r in rows:
            b = int(r['brokers'])
            p50 = r.get('p50_ns')
            p90 = r.get('p90_ns')
            p99 = r.get('p99_ns')
            if p50 is None or p90 is None or p99 is None:
                continue
            lat.setdefault(b, {'p50': [], 'p90': [], 'p99': []})
            lat[b]['p50'].append(int(p50))
            lat[b]['p90'].append(int(p90))
            lat[b]['p99'].append(int(p99))
        xs = sorted(lat.keys())
        ys50 = [int(stats.median(lat[b]['p50'])) for b in xs]
        ys90 = [int(stats.median(lat[b]['p90'])) for b in xs]
        ys99 = [int(stats.median(lat[b]['p99'])) for b in xs]

        if args.lat_csv:
            with open(args.lat_csv, 'w', newline='') as f:
                w = csv.writer(f)
                w.writerow(['brokers','p50_ns','p90_ns','p99_ns'])
                for i, b in enumerate(xs):
                    w.writerow([b, ys50[i], ys90[i], ys99[i]])

        if args.lat_png or args.lat_pdf or args.lat_svg:
            try:
                import matplotlib
                matplotlib.use('Agg')
                import matplotlib.pyplot as plt
                plt.figure(figsize=(6.5,3.8))
                plt.plot(xs, ys50, marker='o', label='P50')
                plt.plot(xs, ys90, marker='o', label='P90')
                plt.plot(xs, ys99, marker='o', label='P99')
                plt.xlabel('Brokers')
                plt.ylabel('Ordering latency (ns per batch)')
                plt.title('Ordering latency percentiles vs brokers')
                plt.grid(True, linestyle='--', alpha=0.3)
                plt.legend(frameon=False)
                plt.tight_layout()
                if args.lat_png:
                    plt.savefig(args.lat_png, dpi=args.dpi)
                if args.lat_pdf:
                    plt.savefig(args.lat_pdf, dpi=args.dpi)
                if args.lat_svg:
                    plt.savefig(args.lat_svg)
            except Exception:
                pass

    # Contention breakdown vs brokers
    if args.cont_png or args.cont_pdf or args.cont_svg or args.cont_csv:
        # Compute lock/assign per batch and atomics per batch
        agg = {}
        for r in rows:
            b = int(r['brokers'])
            tb = max(1, int(r.get('total_batches', 0)))
            lock_ns = int(r.get('total_lock_ns', 0))
            assign_ns = int(r.get('total_assign_ns', 0))
            atoms = int(r.get('atomic_fetch_add', 0))
            lock_per_batch = lock_ns / tb
            assign_per_batch = assign_ns / tb
            atoms_per_batch = atoms / tb
            agg.setdefault(b, {'lock': [], 'assign': [], 'atoms': []})
            agg[b]['lock'].append(lock_per_batch)
            agg[b]['assign'].append(assign_per_batch)
            agg[b]['atoms'].append(atoms_per_batch)
        xs = sorted(agg.keys())
        lock_med = [stats.median(agg[b]['lock']) for b in xs]
        assign_med = [stats.median(agg[b]['assign']) for b in xs]
        atoms_med = [stats.median(agg[b]['atoms']) for b in xs]
        total_time = [max(1e-9, lock_med[i] + assign_med[i]) for i in range(len(xs))]
        lock_pct = [100.0 * lock_med[i] / total_time[i] for i in range(len(xs))]
        assign_pct = [100.0 * assign_med[i] / total_time[i] for i in range(len(xs))]

        if args.cont_csv:
            with open(args.cont_csv, 'w', newline='') as f:
                w = csv.writer(f)
                w.writerow(['brokers','lock_ns_per_batch','assign_ns_per_batch','lock_pct','assign_pct','atomics_per_batch'])
                for i, b in enumerate(xs):
                    w.writerow([b, int(lock_med[i]), int(assign_med[i]), f"{lock_pct[i]:.2f}", f"{assign_pct[i]:.2f}", f"{atoms_med[i]:.3f}"])

        if args.cont_png or args.cont_pdf or args.cont_svg:
            try:
                import matplotlib
                matplotlib.use('Agg')
                import matplotlib.pyplot as plt
                fig, ax1 = plt.subplots(figsize=(6.5,3.8))
                # Stacked percentages
                ax1.stackplot(xs, lock_pct, assign_pct, labels=['Lock','Assign'], colors=['#4c78a8','#f58518'], alpha=0.8)
                ax1.set_xlabel('Brokers')
                ax1.set_ylabel('Percent time per batch (%)')
                ax1.set_ylim(0, 100)
                ax1.grid(True, linestyle='--', alpha=0.3)
                # Overlay atomics per batch on right axis
                ax2 = ax1.twinx()
                ax2.plot(xs, atoms_med, color='#54a24b', marker='o', label='Atomics/batch')
                ax2.set_ylabel('Atomics per batch')
                # Compose legend
                lines_labels = [(l, l.get_label()) for l in ax2.lines]
                # For stackplot, create proxy artists
                import matplotlib.patches as mpatches
                proxy_lock = mpatches.Patch(color='#4c78a8', label='Lock')
                proxy_assign = mpatches.Patch(color='#f58518', label='Assign')
                proxies = [proxy_lock, proxy_assign] + [l for l,_ in lines_labels]
                labels = ['Lock','Assign'] + [lbl for _, lbl in lines_labels]
                ax1.legend(proxies, labels, frameon=False, loc='upper right')
                fig.tight_layout()
                if args.cont_png:
                    fig.savefig(args.cont_png, dpi=args.dpi)
                if args.cont_pdf:
                    fig.savefig(args.cont_pdf, dpi=args.dpi)
                if args.cont_svg:
                    fig.savefig(args.cont_svg)
            except Exception:
                pass

if __name__ == '__main__':
    main()


