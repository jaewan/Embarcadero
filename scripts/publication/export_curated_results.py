#!/usr/bin/env python3
"""
Export curated publication-quality results to two plotting-ready CSVs.

Data source strategy:
  - publication_2026_final: reads per-cell run_*/summary.csv files (the top-level
    throughput_summary.csv is incomplete — it only contains EMBARCADERO rows)
  - LAZYLOG RF=1: 20260402_publication_baseline_rf1_final3 (clean run 212037Z)
  - LAZYLOG RF=2: 20260402_publication_matrix_appendable_rf2
  - CORFU latency: corfu_fullmatrix_20260401
  - EMBARCADERO latency: publication_2026_final latency per-cell summary files

Selection rules:
  - Only rows with status='ok'
  - For cells with multiple runs, take the LATEST run dir with OK rows
  - Annotate with topology_class and data_quality flags
  - No rows are hard-blocked (O5 RF2 N3 data integrity error claim was based on
    incorrect data from a subagent; actual values are distinct and valid)

Usage:
    cd /home/domin/Embarcadero
    python3 scripts/publication/export_curated_results.py

Outputs:
    data/publication/curated_throughput.csv
    data/publication/curated_latency.csv
"""

import csv
import os
import sys
from pathlib import Path
from collections import defaultdict

REPO_ROOT = Path(__file__).resolve().parents[2]
PUB_TP = REPO_ROOT / "data/publication/throughput"
PUB_LAT = REPO_ROOT / "data/publication/latency"
OUT_DIR = REPO_ROOT / "data/publication"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_csv(path):
    p = Path(path)
    if not p.exists():
        print(f"  WARNING: not found: {p}", file=sys.stderr)
        return []
    with open(p) as f:
        return list(csv.DictReader(f))

def overlap_val(row):
    try:
        return float(row.get("throughput_overlap_gbps", "") or "")
    except (ValueError, TypeError):
        return None

def classify_topology(row):
    """Flag whether the local moscxl machine is included as an Nth client."""
    layout = row.get("client_layout", "")
    try:
        n = int(row.get("num_clients", 1))
    except (ValueError, TypeError):
        n = 1
    if "moscxl-local" in layout and n >= 2:
        return "local_as_n2" if n == 2 else "local_as_n3"
    return "all_remote"

def data_quality_flag(ok_rows):
    """Classify cell quality based on trial count and variance."""
    n = len(ok_rows)
    if n == 0:
        return "missing"
    if n == 1:
        return "single_trial"
    vals = [v for v in (overlap_val(r) for r in ok_rows) if v is not None]
    if len(vals) < 2:
        return "single_trial"
    med = sorted(vals)[len(vals) // 2]
    if med > 0 and (max(vals) - min(vals)) / med > 0.40:
        return "high_variance"
    return "ok"

def latest_ok_run(cell_path):
    """
    Return the list of ok rows from the LATEST run_* dir under cell_path
    that contains at least one ok row.
    """
    cell_path = Path(cell_path)
    if not cell_path.exists():
        return []
    # Sort run dirs descending by name (ISO timestamp → lexicographic = chronological)
    run_dirs = sorted(
        [d for d in cell_path.iterdir() if d.is_dir() and d.name.startswith("run_")],
        reverse=True,
    )
    for run_dir in run_dirs:
        csv_path = run_dir / "summary.csv"
        if not csv_path.exists():
            continue
        rows = load_csv(csv_path)
        ok_rows = [r for r in rows if r.get("status") == "ok"]
        if ok_rows:
            return ok_rows
    return []

# ---------------------------------------------------------------------------
# Throughput curation
# ---------------------------------------------------------------------------

def curate_throughput():
    rows_out = []

    # ── publication_2026_final: EMBARCADERO, CORFU, SCALOG ──────────────────
    primary_tag = "publication_2026_final"
    primary_base = PUB_TP / primary_tag

    # Scan all cell subdirs (skip throughput_summary.csv which is incomplete)
    cell_dirs = sorted(
        [d for d in primary_base.iterdir() if d.is_dir()],
    )
    for cell_dir in cell_dirs:
        ok_rows = latest_ok_run(cell_dir)
        if not ok_rows:
            print(f"  WARN: no ok rows in {cell_dir.name}", file=sys.stderr)
            continue
        topo = classify_topology(ok_rows[0])
        qual = data_quality_flag(ok_rows)
        for r in ok_rows:
            rows_out.append({
                "source_tag": primary_tag,
                "commit": r.get("commit", "")[:8],
                "system": r.get("system", ""),
                "order": r.get("order", ""),
                "sequencer": r.get("sequencer", ""),
                "replication_factor": r.get("replication_factor", ""),
                "num_clients": r.get("num_clients", ""),
                "client_layout": r.get("client_layout", ""),
                "run_idx": r.get("run_idx", ""),
                "throughput_overlap_gbps": r.get("throughput_overlap_gbps", ""),
                "overlap_window_ms": r.get("overlap_window_ms", ""),
                "topology_class": topo,
                "data_quality": qual,
                "note": "",
            })

    # ── LAZYLOG RF=1: baseline_rf1_final3 ───────────────────────────────────
    ll_rf1_tag = "20260402_publication_baseline_rf1_final3"
    ll_rf1_rows = load_csv(PUB_TP / ll_rf1_tag / "throughput_summary.csv")
    # Only include ok rows from the clean run (run_20260401T212037Z), not the
    # earlier missing_logs run (run_20260401T211847Z__o_rf_n)
    for r in ll_rf1_rows:
        if r.get("status") != "ok":
            continue
        if r.get("system") != "lazylog":
            continue
        if "run_20260401T212037Z" not in r.get("artifact_dir", ""):
            continue
        topo = classify_topology(r)
        rows_out.append({
            "source_tag": ll_rf1_tag,
            "commit": r.get("commit", "")[:8],
            "system": "lazylog",
            "order": r.get("order", ""),
            "sequencer": r.get("sequencer", ""),
            "replication_factor": r.get("replication_factor", ""),
            "num_clients": r.get("num_clients", ""),
            "client_layout": r.get("client_layout", ""),
            "run_idx": r.get("run_idx", ""),
            "throughput_overlap_gbps": r.get("throughput_overlap_gbps", ""),
            "overlap_window_ms": r.get("overlap_window_ms", ""),
            "topology_class": topo,
            "data_quality": "ok",
            "note": "lazylog_separate_tag",
        })

    # ── LAZYLOG RF=2: appendable_rf2 ────────────────────────────────────────
    ll_rf2_tag = "20260402_publication_matrix_appendable_rf2"
    ll_rf2_rows = load_csv(PUB_TP / ll_rf2_tag / "throughput_summary.csv")
    for r in ll_rf2_rows:
        if r.get("status") != "ok":
            continue
        if r.get("system") != "lazylog":
            continue
        topo = classify_topology(r)
        rows_out.append({
            "source_tag": ll_rf2_tag,
            "commit": r.get("commit", "")[:8],
            "system": "lazylog",
            "order": r.get("order", ""),
            "sequencer": r.get("sequencer", ""),
            "replication_factor": r.get("replication_factor", ""),
            "num_clients": r.get("num_clients", ""),
            "client_layout": r.get("client_layout", ""),
            "run_idx": r.get("run_idx", ""),
            "throughput_overlap_gbps": r.get("throughput_overlap_gbps", ""),
            "overlap_window_ms": r.get("overlap_window_ms", ""),
            "topology_class": topo,
            "data_quality": "single_trial",
            "note": "lazylog_rf2_only_source;single_trial",
        })

    # Write output
    out_path = OUT_DIR / "curated_throughput.csv"
    fieldnames = [
        "source_tag", "commit", "system", "order", "sequencer",
        "replication_factor", "num_clients", "client_layout",
        "run_idx", "throughput_overlap_gbps", "overlap_window_ms",
        "topology_class", "data_quality", "note",
    ]
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows_out)
    print(f"Wrote {len(rows_out)} throughput rows → {out_path}")
    return rows_out


# ---------------------------------------------------------------------------
# Latency curation
# ---------------------------------------------------------------------------

def curate_latency():
    rows_out = []

    # ── EMBARCADERO ORDER=5 RF=1/RF=2 and ORDER=0 RF=1: publication_2026_final ─
    # Note: ORDER=0 RF=2 with c4 publisher FAILED in publication_2026_final.
    # Those conditions are sourced from embarcadero_latency_clean_20260401 below.
    lat_base = PUB_LAT / "publication_2026_final"
    emb_conditions = [
        ("embarcadero_order0_rf1", "0", "1", "EMBARCADERO", "c4"),
        # embarcadero_order0_rf2 is all-failed in pub_2026_final; skip it
        ("embarcadero_order5_rf1", "5", "1", "EMBARCADERO", "c4"),
        ("embarcadero_order5_rf2", "5", "2", "EMBARCADERO", "c4"),
    ]
    for cond_dir, order, rf, seq, pub in emb_conditions:
        cond_path = lat_base / cond_dir
        if not cond_path.exists():
            print(f"  WARNING: latency dir missing: {cond_path}", file=sys.stderr)
            continue
        for run_dir in sorted(cond_path.iterdir()):
            if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
                continue
            summary = run_dir / "summary.csv"
            if not summary.exists():
                continue
            with open(summary) as f:
                for row in csv.DictReader(f):
                    if row.get("status") != "ok":
                        continue
                    rows_out.append({
                        "source_tag": "publication_2026_final",
                        "commit": row.get("commit", "")[:8],
                        "system": row.get("system", "embarcadero"),
                        "order": row.get("order", order),
                        "sequencer": row.get("sequencer", seq),
                        "replication_factor": row.get("replication_factor", rf),
                        "publisher_host": row.get("publisher_host", pub),
                        "broker_host": row.get("broker_host", "moscxl"),
                        "run_idx": row.get("run_idx", ""),
                        "p50_us": row.get("p50_us", ""),
                        "p95_us": row.get("p95_us", ""),
                        "p99_us": row.get("p99_us", ""),
                        "max_us": row.get("max_us", ""),
                        "data_quality": "ok",
                        "note": "",
                    })

    # ── EMBARCADERO ORDER=0 RF=1 (c2 pub) and RF=2 (c2 pub): latency_clean ──
    # This tag has the post-fix ORDER=0 RF=2 data. Publisher is c2, not c4.
    # For cross-publisher comparisons, use these together (both c2).
    clean_base = PUB_LAT / "embarcadero_latency_clean_20260401"
    clean_conditions = [
        ("embarcadero_order0_rf1", "0", "1"),
        ("embarcadero_order0_rf2", "0", "2"),
    ]
    for cond_dir, order, rf in clean_conditions:
        cond_path = clean_base / cond_dir
        if not cond_path.exists():
            print(f"  WARNING: latency dir missing: {cond_path}", file=sys.stderr)
            continue
        # Use only the latest OK run
        ok_run_rows = []
        for run_dir in sorted(cond_path.iterdir(), reverse=True):
            if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
                continue
            summary = run_dir / "summary.csv"
            if not summary.exists():
                continue
            with open(summary) as f:
                rows_in_run = [r for r in csv.DictReader(f) if r.get("status") == "ok"]
            if rows_in_run:
                ok_run_rows = rows_in_run
                break
        for row in ok_run_rows:
            note = "c2_publisher_use_for_rf1_vs_rf2_comparison"
            if order == "0" and rf == "2":
                note += ";post_order0rf2_fix_20260401"
            rows_out.append({
                "source_tag": "embarcadero_latency_clean_20260401",
                "commit": row.get("commit", "")[:8],
                "system": row.get("system", "embarcadero"),
                "order": row.get("order", order),
                "sequencer": row.get("sequencer", "EMBARCADERO"),
                "replication_factor": row.get("replication_factor", rf),
                "publisher_host": row.get("publisher_host", "c2"),
                "broker_host": row.get("broker_host", "moscxl"),
                "run_idx": row.get("run_idx", ""),
                "p50_us": row.get("p50_us", ""),
                "p95_us": row.get("p95_us", ""),
                "p99_us": row.get("p99_us", ""),
                "max_us": row.get("max_us", ""),
                "data_quality": "ok",
                "note": note,
            })

    # ── CORFU: corfu_fullmatrix_20260401 (preferred; has 3 trials per RF) ───
    corfu_tag = "corfu_fullmatrix_20260401"
    corfu_rows = load_csv(PUB_LAT / corfu_tag / "latency_summary.csv")
    for row in corfu_rows:
        if row.get("status") != "ok":
            continue
        rf = row.get("replication_factor", "")
        run_idx = int(row.get("run_idx", 0))
        # RF=1 trial 2 (run_idx=2) shows P50=62ms vs trials 1,3 at ~7ms
        quality = "suspect_high_outlier" if (rf == "1" and run_idx == 2) else "ok"
        rows_out.append({
            "source_tag": corfu_tag,
            "commit": row.get("commit", "")[:8],
            "system": "corfu",
            "order": row.get("order", "2"),
            "sequencer": row.get("sequencer", "CORFU"),
            "replication_factor": rf,
            "publisher_host": row.get("publisher_host", "c2"),
            "broker_host": row.get("broker_host", "moscxl"),
            "run_idx": row.get("run_idx", ""),
            "p50_us": row.get("p50_us", ""),
            "p95_us": row.get("p95_us", ""),
            "p99_us": row.get("p99_us", ""),
            "max_us": row.get("max_us", ""),
            "data_quality": quality,
            "note": "rf1_trial2_outlier_8.6x" if quality == "suspect_high_outlier" else "",
        })

    out_path = OUT_DIR / "curated_latency.csv"
    fieldnames = [
        "source_tag", "commit", "system", "order", "sequencer",
        "replication_factor", "publisher_host", "broker_host",
        "run_idx", "p50_us", "p95_us", "p99_us", "max_us",
        "data_quality", "note",
    ]
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows_out)
    print(f"Wrote {len(rows_out)} latency rows → {out_path}")
    return rows_out


# ---------------------------------------------------------------------------
# Summary printout
# ---------------------------------------------------------------------------

def print_throughput_summary(rows):
    cell_vals = defaultdict(list)
    for r in rows:
        key = (r["system"], r["order"], r["replication_factor"], r["num_clients"],
               r["topology_class"], r["data_quality"])
        v = overlap_val(r)
        if v is not None:
            cell_vals[key].append(v)

    print("\n=== CURATED THROUGHPUT MEDIANS (overlap GB/s) ===")
    hdr = f"{'System':<22} {'RF':<4} {'N':<4} {'T':>2}  {'Median':>8}  {'Min':>8}  {'Max':>8}  {'Topo':<14}  {'Quality'}"
    print(hdr)
    print("-" * 100)

    ANOMALY = {
        # (system, order, rf, n): explanation
        ("scalog", "1", "1", "1"): "RF1 < RF2 (anomalous; see R-SCALOG)",
        ("corfu", "2", "1", "3"): "N3 < N2 overlap (see R-CORFU-N3)",
        ("corfu", "2", "2", "3"): "N3 < N2 overlap (see R-CORFU-N3)",
    }

    for key in sorted(cell_vals.keys(), key=lambda k: (k[0], k[1], int(k[2]), int(k[3]))):
        system, order, rf, n, topo, qual = key
        vals = sorted(cell_vals[key])
        median = vals[len(vals) // 2]
        label = f"{system}(o={order})"
        anomaly = ANOMALY.get((system, order, rf, n), "")
        flag = " ⚠" if anomaly else ""
        print(f"{label:<22} RF={rf:<3} N={n:<3} {len(vals):>2}  {median:>8.2f}  {min(vals):>8.2f}  {max(vals):>8.2f}  {topo:<14}  {qual}{flag}")
        if anomaly:
            print(f"  ↳ NOTE: {anomaly}")

def print_latency_summary(rows):
    print("\n=== CURATED LATENCY (µs) ===")
    print(f"{'System':<15} {'Order':<6} {'RF':<4} {'Run':<5} {'P50':>8}  {'P99':>10}  {'Quality'}")
    print("-" * 72)
    for r in rows:
        p50 = r.get("p50_us", "")
        p99 = r.get("p99_us", "")
        try:
            p50_f = f"{float(p50):>8,.0f}"
        except ValueError:
            p50_f = f"{'N/A':>8}"
        try:
            p99_f = f"{float(p99):>10,.0f}"
        except ValueError:
            p99_f = f"{'N/A':>10}"
        qual = r.get("data_quality", "")
        flag = " ⚠" if "suspect" in qual else ""
        print(f"{r.get('system',''):<15} {r.get('order',''):<6} {r.get('replication_factor',''):<4} "
              f"{r.get('run_idx',''):<5} {p50_f}  {p99_f}  {qual}{flag}")

if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    tp_rows = curate_throughput()
    lat_rows = curate_latency()
    print_throughput_summary(tp_rows)
    print_latency_summary(lat_rows)

    print("\n" + "="*60)
    print("KEY ISSUES REQUIRING ACTION BEFORE PUBLICATION")
    print("="*60)
    issues = [
        "BLOCKING : CORFU RF=1 N=3 latest run (99d2881) gives LOWER overlap than N=2.\n"
        "           Multiple commits in this cell (b8a9dd4→d770a19→d0597cb3→99d2881).\n"
        "           Re-run under a single fixed commit.",
        "BLOCKING : SCALOG RF=1 N=1 overlap (1.5 GB/s) < RF=2 N=1 (3.6 GB/s).\n"
        "           RF=2 should not be faster than RF=1. Check ACK/sequencer config.",
        "HIGH     : LazyLog RF=2 N=2,3 each have only 1 trial. Need ≥3 trials.",
        "HIGH     : CORFU RF=1 latency: trial 2 is 8.6× trials 1,3. Cause unknown.",
        "HIGH     : No latency data for Scalog or LazyLog exists anywhere.",
        "MEDIUM   : EMBARCADERO O0/O5 RF=2 N=2 has 40-60% inter-trial range.",
        "MEDIUM   : CORFU RF=2 N=1: trial 1 (3.80) is 40% below median (6.25).",
        "INFO     : publication_2026_final top-level throughput_summary.csv is\n"
        "           incomplete (CORFU/SCALOG missing). Use per-cell files only.",
        "INFO     : Mixed commits within CORFU RF=1 N=3 cell across run attempts.",
    ]
    for i in issues:
        print(f"  • {i}")
