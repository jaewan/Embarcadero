#!/usr/bin/env python3
"""Task 1 (T/tau vs T/R) aggregation.

Reads the per-broker-process trace CSVs written by src/embarlet/order5_tr_trace.h
(EMBAR_ORDER5_TR_TRACE=1) for each campaign cell tau<US>_trial<N>/, and reports the
measured distributions that resolve the paper's T/R contradiction:

  P (observation)  : scanner inspection period  (scan_pass events; x4096 inspections)
  tau (release)    : epoch seal interval
  per gap          : hold_release latency, seals_during_gap (= releases available in
                     the gap = T/tau), inspections_during_gap (= T/P)
  isolation        : each session's max inter-commit gap + commit throughput

Usage: python3 PaperScripts/analyze_tr_tau.py <campaign_dir> [out.csv]
"""
import sys, os, glob, csv
from collections import defaultdict, deque

def pct(a, p):
    a = sorted(a)
    if not a: return float('nan')
    return a[min(len(a)-1, int(p/100.0*len(a)))]

def med(a):
    a = sorted(a)
    if not a: return float('nan')
    n = len(a)
    return a[n//2] if n % 2 else (a[n//2-1]+a[n//2])/2.0

def load_cell(cell):
    rows = []
    for f in glob.glob(os.path.join(cell, "tr.pid*.t*")):
        try:
            rows += list(csv.DictReader(open(f)))
        except Exception:
            pass
    for r in rows:
        r['ns'] = int(r['steady_ns'])
    rows.sort(key=lambda r: r['ns'])
    return rows

def analyze_cell(cell):
    rows = load_cell(cell)
    if not rows: return None
    seals = [r['ns'] for r in rows if r['type'] == 'seal']
    seal_int = [(seals[i+1]-seals[i])/1000.0 for i in range(len(seals)-1)]  # us
    # scanner inspection period P (per-broker scan_pass sampled every 4096 inspections)
    passes_by_b = defaultdict(list)
    for r in rows:
        if r['type'] == 'scan_pass':
            passes_by_b[r['broker']].append(r['ns'])
    insp_us = []  # per-inspection period
    for b, ts in passes_by_b.items():
        ts.sort()
        for i in range(len(ts)-1):
            insp_us.append((ts[i+1]-ts[i])/1000.0/4096.0)
    # Robust mean scanner-inspection period from the surviving cumulative counter
    # (scan_pass_total, carried in every gap row) even when scan_pass rows were
    # buffered-then-SIGKILLed: mean_P = seal_span / (max_scan_pass_total * 4096).
    max_spt = max((int(r['scan_pass_total']) for r in rows
                   if r['type'] in ('gap_detect','gap_release','scan_pass')), default=0)
    seal_span_ns = (seals[-1]-seals[0]) if len(seals) >= 2 else 0
    mean_P_us = (seal_span_ns/1000.0)/(max_spt*4096.0) if (max_spt and seal_span_ns) else float('nan')
    # gaps: pair detect->release FIFO per session
    det = defaultdict(deque); holds=[]; seals_in=[]; insp_in=[]
    for r in rows:
        sk = r['session_or_epoch']
        if r['type'] == 'gap_detect':
            det[sk].append(r)
        elif r['type'] == 'gap_release' and det[sk]:
            d = det[sk].popleft()
            holds.append((r['ns']-d['ns'])/1000.0)                       # us
            seals_in.append(int(r['epoch_index'])-int(d['epoch_index'])) # seals during gap
            insp_in.append((int(r['scan_pass_total'])-int(d['scan_pass_total']))*4096)  # inspections
    # per-session commit timeline -> max inter-commit gap + throughput
    commits = defaultdict(list)
    for r in rows:
        if r['type'] == 'commit':
            commits[r['session_or_epoch']].append(r['ns'])
    sess_stats = {}
    for sk, ts in commits.items():
        ts.sort()
        if len(ts) < 2: continue
        gaps = [(ts[i+1]-ts[i])/1000.0 for i in range(len(ts)-1)]  # us
        span_s = (ts[-1]-ts[0])/1e9
        sess_stats[sk] = dict(commits=len(ts), max_intercommit_us=max(gaps),
                              p99_intercommit_us=pct(gaps,99), commit_rate_hz=len(ts)/span_s if span_s>0 else 0)
    return dict(
        seals=len(seals),
        tau_p50=med(seal_int), tau_p95=pct(seal_int,95), tau_p99=pct(seal_int,99),
        P_insp_us_p50=med(insp_us), P_insp_us_p95=pct(insp_us,95),
        P_insp_us_mean=mean_P_us,
        n_gaps=len(holds),
        hold_us_p50=med(holds), hold_us_p95=pct(holds,95), hold_us_p99=pct(holds,99),
        seals_in_p50=med(seals_in), seals_in_p95=pct(seals_in,95), seals_in_p99=pct(seals_in,99),
        insp_in_p50=med(insp_in), insp_in_p95=pct(insp_in,95),
        sessions=sess_stats,
    )

def main():
    camp = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(camp, "tr_tau_summary.csv")
    cells = sorted(glob.glob(os.path.join(camp, "tau*_trial*")))
    per_tau = defaultdict(list)
    rows_out = []
    for cell in cells:
        base = os.path.basename(cell)
        tau = int(base.split('_')[0].replace('tau',''))
        st = analyze_cell(cell)
        if not st:
            print(f"{base}: NO DATA"); continue
        per_tau[tau].append(st)
        print(f"\n== {base} ==  seals={st['seals']} tau_us(P50/P95/P99)={st['tau_p50']:.1f}/{st['tau_p95']:.1f}/{st['tau_p99']:.1f}"
              f"  P_insp_us(rows_P50={st['P_insp_us_p50']:.3f}, mean_from_counter={st['P_insp_us_mean']:.3f})")
        print(f"   gaps={st['n_gaps']} hold_us(P50/P95/P99)={st['hold_us_p50']:.0f}/{st['hold_us_p95']:.0f}/{st['hold_us_p99']:.0f}"
              f"  seals_in_gap(P50/P95/P99)={st['seals_in_p50']}/{st['seals_in_p95']}/{st['seals_in_p99']}"
              f"  insp_in_gap(P50)={st['insp_in_p50']:.0f}")
        for sk, ss in st['sessions'].items():
            print(f"   session {sk[:10]}: commits={ss['commits']} rate={ss['commit_rate_hz']:.0f}/s "
                  f"max_intercommit_us={ss['max_intercommit_us']:.0f} p99={ss['p99_intercommit_us']:.0f}")
    # aggregate across trials per tau
    with open(out, 'w') as f:
        w = csv.writer(f)
        w.writerow(["tau_us","trials","tau_meas_p50_med","P_insp_us_p50_med","n_gaps_total",
                    "hold_us_p50_med","seals_in_gap_p50_med","seals_in_gap_p50_min","seals_in_gap_p50_max",
                    "insp_in_gap_p50_med","T1p5ms_seals_pred"])
        print("\n==== PER-TAU AGGREGATE (median across trials; T/tau vs T/P) ====")
        for tau in sorted(per_tau):
            sts = per_tau[tau]
            taum = med([s['tau_p50'] for s in sts])
            pinsp = med([s['P_insp_us_p50'] for s in sts])
            ngap = sum(s['n_gaps'] for s in sts)
            holdm = med([s['hold_us_p50'] for s in sts])
            sgm = [s['seals_in_p50'] for s in sts]
            inspm = med([s['insp_in_p50'] for s in sts])
            pred_1p5 = 1500.0/taum if taum else float('nan')  # seals for a 1.5ms skew window
            w.writerow([tau, len(sts), f"{taum:.1f}", f"{pinsp:.3f}", ngap,
                        f"{holdm:.0f}", f"{med(sgm):.1f}", f"{min(sgm):.1f}", f"{max(sgm):.1f}",
                        f"{inspm:.0f}", f"{pred_1p5:.1f}"])
            print(f"  tau={tau}us: meas_tau={taum:.0f}us  P_insp={pinsp:.3f}us  gaps={ngap}  hold_p50={holdm:.0f}us  "
                  f"seals_in_gap_p50={med(sgm):.1f}(min {min(sgm)},max {max(sgm)})  insp_in_gap={inspm:.0f}  "
                  f"=> 1.5ms-skew releases=T/tau={pred_1p5:.1f} (vs ~{1500.0/pinsp if pinsp else 0:.0f} inspections)")
    print(f"\nWrote {out}")

if __name__ == "__main__":
    main()
