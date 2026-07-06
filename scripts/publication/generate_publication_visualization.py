#!/usr/bin/env python3

import csv
import math
import os
from pathlib import Path
from statistics import mean


ROOT = Path("/home/domin/Embarcadero")
OUTPUT_DIR = ROOT / "data/publication/visualizations/20260330_publication_matrix_dashboard"

THROUGHPUT_SUMMARY = ROOT / "data/publication/throughput/20260329_moscxl_publication_rerun_fix25/throughput_summary.csv"
LATENCY_SUMMARY = ROOT / "data/publication/latency/20260328_moscxl_publication_rerun_fix24/latency_summary.csv"
SCALOG_TPUT_ROOT = ROOT / "data/publication/throughput/20260330_scalog_publication_rerun_fix39"
SCALOG_LAT_RF1 = ROOT / "data/publication/latency/20260330_scalog_publication_rerun_fix39/SCALOG_order1_rf1/run_20260329T193901Z/summary.csv"
SCALOG_LAT_RF2_FIX40 = ROOT / "data/publication/latency/20260330_scalog_latency_rf2_gate_fix40/SCALOG_order1_rf2/run_20260329T194956Z/summary.csv"
SCALOG_LAT_RF2_FIX42 = ROOT / "data/publication/latency/20260330_scalog_latency_rf2_ingest_flush_fix42/SCALOG_order1_rf2/run_20260329T200139Z/summary.csv"
SCALOG_MATRIX_LOG = ROOT / "data/publication/scalog_matrix/20260330_scalog_publication_rerun_fix39/matrix_run.log"


COLORS = {
    "EMBARCADERO": "#126b52",
    "CORFU": "#a6521a",
    "SCALOG": "#205a9c",
}


def read_csv(path: Path):
    with path.open(newline="") as fh:
        return list(csv.DictReader(fh))


def avg(values):
    return mean(float(v) for v in values)


def fmt_num(value, digits=1):
    return f"{value:.{digits}f}"


def status_badge(status: str):
    palettes = {
        "clean": ("#d7f5dd", "#166534"),
        "blocked": ("#fde2e2", "#991b1b"),
        "partial": ("#fff3c4", "#92400e"),
        "n/a": ("#e8edf3", "#475569"),
    }
    bg, fg = palettes[status]
    return f'<span class="badge" style="background:{bg};color:{fg}">{status.upper()}</span>'


def build_dataset():
    throughput_rows = read_csv(THROUGHPUT_SUMMARY)
    latency_rows = read_csv(LATENCY_SUMMARY)

    systems = {
        "EMBARCADERO": {
            "throughput": {1: {}, 2: {}},
            "latency": {},
            "order_label": "order0",
        },
        "CORFU": {
            "throughput": {1: {}, 2: {}},
            "latency": {},
            "order_label": "order2",
        },
        "SCALOG": {
            "throughput": {1: {}, 2: {}},
            "latency": {},
            "order_label": "order1",
        },
    }

    for system_key, csv_system in [("EMBARCADERO", "embarcadero"), ("CORFU", "corfu")]:
        for rf in (1, 2):
            for clients in (1, 2, 3):
                rows = [
                    r for r in throughput_rows
                    if r["system"] == csv_system
                    and int(r["replication_factor"]) == rf
                    and int(r["num_clients"]) == clients
                    and r["status"] == "ok"
                ]
                systems[system_key]["throughput"][rf][clients] = avg(r["throughput_gbps"] for r in rows)

            lat = [
                r for r in latency_rows
                if r["system"] == csv_system
                and int(r["replication_factor"]) == rf
                and r["status"] == "ok"
            ]
            systems[system_key]["latency"][rf] = {
                "p50": avg(r["p50_us"] for r in lat),
                "p95": avg(r["p95_us"] for r in lat),
                "p99": avg(r["p99_us"] for r in lat),
                "status": "clean",
            }

    for rf in (1, 2):
        for clients in (1, 2, 3):
            path = next((SCALOG_TPUT_ROOT / f"SCALOG_order1_rf{rf}_n{clients}").glob("run_*/summary.csv"))
            rows = [r for r in read_csv(path) if r["status"] == "ok"]
            systems["SCALOG"]["throughput"][rf][clients] = avg(r["throughput_gbps"] for r in rows)

    lat_rf1_rows = [r for r in read_csv(SCALOG_LAT_RF1) if r["status"] == "ok"]
    systems["SCALOG"]["latency"][1] = {
        "p50": avg(r["p50_us"] for r in lat_rf1_rows),
        "p95": avg(r["p95_us"] for r in lat_rf1_rows),
        "p99": avg(r["p99_us"] for r in lat_rf1_rows),
        "status": "clean",
    }

    systems["SCALOG"]["latency"][2] = {
        "status": "blocked",
        "evidence": [
            str(SCALOG_LAT_RF2_FIX40),
            str(SCALOG_LAT_RF2_FIX42),
        ],
    }

    return systems


def render_status_table(data):
    headers = [
        "System",
        "Tput RF1 n1",
        "Tput RF1 n2",
        "Tput RF1 n3",
        "Tput RF2 n1",
        "Tput RF2 n2",
        "Tput RF2 n3",
        "Lat RF1",
        "Lat RF2",
    ]
    rows = []
    for system in ("EMBARCADERO", "CORFU", "SCALOG"):
        t = data[system]["throughput"]
        l = data[system]["latency"]
        values = [
            f"{fmt_num(t[1][1])} Gbps",
            f"{fmt_num(t[1][2])} Gbps",
            f"{fmt_num(t[1][3])} Gbps",
            f"{fmt_num(t[2][1])} Gbps",
            f"{fmt_num(t[2][2])} Gbps",
            f"{fmt_num(t[2][3])} Gbps",
            status_badge(l[1]["status"]),
            status_badge(l[2]["status"]),
        ]
        row = f"<tr><th>{system}</th>" + "".join(f"<td>{value}</td>" for value in values) + "</tr>"
        rows.append(row)

    return (
        "<table class='matrix'><thead><tr>"
        + "".join(f"<th>{h}</th>" for h in headers)
        + "</tr></thead><tbody>"
        + "".join(rows)
        + "</tbody></table>"
    )


def render_throughput_chart(data):
    width = 980
    height = 330
    left = 70
    top = 28
    bottom = 48
    chart_h = height - top - bottom
    groups = [1, 2, 3]
    max_val = max(data[s]["throughput"][1][c] for s in data for c in groups) * 1.12
    band = 260
    bar_w = 44
    gap = 12
    group_start = left + 40
    elements = []

    for tick in range(0, int(max_val) + 1, 25):
        y = top + chart_h - (tick / max_val) * chart_h
        elements.append(f"<line x1='{left}' y1='{y:.1f}' x2='{width-25}' y2='{y:.1f}' class='grid'/>")
        elements.append(f"<text x='{left-10}' y='{y+4:.1f}' text-anchor='end' class='axis'>{tick}</text>")

    for group_idx, clients in enumerate(groups):
        x0 = group_start + group_idx * band
        for sys_idx, system in enumerate(("EMBARCADERO", "CORFU", "SCALOG")):
            val = data[system]["throughput"][1][clients]
            x = x0 + sys_idx * (bar_w + gap)
            h = (val / max_val) * chart_h
            y = top + chart_h - h
            elements.append(
                f"<rect x='{x:.1f}' y='{y:.1f}' width='{bar_w}' height='{h:.1f}' fill='{COLORS[system]}' rx='5'/>"
            )
            elements.append(f"<text x='{x + bar_w/2:.1f}' y='{y-8:.1f}' text-anchor='middle' class='value'>{fmt_num(val)}</text>")
        elements.append(f"<text x='{x0 + 1.5*(bar_w+gap)-gap/2:.1f}' y='{height-16}' text-anchor='middle' class='axis'>clients={clients}</text>")

    legend = []
    lx = width - 290
    for i, system in enumerate(("EMBARCADERO", "CORFU", "SCALOG")):
        x = lx + i * 92
        legend.append(f"<rect x='{x}' y='6' width='14' height='14' rx='3' fill='{COLORS[system]}'/>")
        legend.append(f"<text x='{x+20}' y='18' class='legend'>{system}</text>")

    return f"""
    <svg viewBox="0 0 {width} {height}" class="chart">
      <text x="{left}" y="18" class="title">RF=1 Throughput Comparison</text>
      <text x="{left}" y="{height-2}" class="axis-label">Average throughput (Gbps) across 3 trials</text>
      <line x1="{left}" y1="{top+chart_h}" x2="{width-25}" y2="{top+chart_h}" class="axis-line"/>
      <line x1="{left}" y1="{top}" x2="{left}" y2="{top+chart_h}" class="axis-line"/>
      {''.join(elements)}
      {''.join(legend)}
    </svg>
    """


def render_latency_chart(data):
    width = 980
    height = 390
    left = 110
    top = 30
    bottom = 48
    chart_h = height - top - bottom
    metrics = ["p50", "p95", "p99"]
    systems = ("EMBARCADERO", "CORFU", "SCALOG")
    values = [data[s]["latency"][1][m] for s in systems for m in metrics]
    min_log = math.log10(min(values))
    max_log = math.log10(max(values))
    rows = []
    for i, metric in enumerate(metrics):
        y = top + 45 + i * 94
        rows.append(f"<text x='{left-18}' y='{y+6}' text-anchor='end' class='axis'>{metric.upper()}</text>")
        rows.append(f"<line x1='{left}' y1='{y}' x2='{width-40}' y2='{y}' class='grid'/>")
        for j, system in enumerate(systems):
            v = data[system]["latency"][1][metric]
            frac = (math.log10(v) - min_log) / (max_log - min_log)
            bar_w = frac * (width - left - 90)
            bar_y = y - 28 + j * 18
            rows.append(
                f"<rect x='{left}' y='{bar_y}' width='{bar_w:.1f}' height='14' fill='{COLORS[system]}' rx='4' opacity='0.9'/>"
            )
            rows.append(
                f"<text x='{left + bar_w + 8:.1f}' y='{bar_y+11}' class='value'>{system} {int(round(v)):,} us</text>"
            )

    ticks = []
    tick_values = [1_000, 3_000, 10_000, 30_000, 100_000, 300_000, 1_000_000]
    for tick in tick_values:
        if tick < min(values) / 1.5 or tick > max(values) * 1.3:
            continue
        frac = (math.log10(tick) - min_log) / (max_log - min_log)
        x = left + frac * (width - left - 90)
        ticks.append(f"<line x1='{x:.1f}' y1='{top}' x2='{x:.1f}' y2='{height-bottom}' class='grid-vert'/>")
        ticks.append(f"<text x='{x:.1f}' y='{height-16}' text-anchor='middle' class='axis'>{tick:,}</text>")

    return f"""
    <svg viewBox="0 0 {width} {height}" class="chart">
      <text x="{left}" y="18" class="title">RF=1 Latency Comparison</text>
      <text x="{left}" y="{height-2}" class="axis-label">Average latency on log scale (microseconds)</text>
      {''.join(ticks)}
      {''.join(rows)}
    </svg>
    """


def render_html(data):
    matrix = render_status_table(data)
    tput_chart = render_throughput_chart(data)
    lat_chart = render_latency_chart(data)
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Publication Matrix Dashboard</title>
  <style>
    :root {{
      --bg: #f6f2e8;
      --panel: #fffdf8;
      --ink: #162027;
      --muted: #5f6b76;
      --line: #d8d0c0;
      --accent: #1f4e79;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      font-family: "IBM Plex Sans", "Avenir Next", sans-serif;
      color: var(--ink);
      background:
        radial-gradient(circle at top left, rgba(18,107,82,0.10), transparent 26%),
        radial-gradient(circle at top right, rgba(32,90,156,0.12), transparent 28%),
        linear-gradient(180deg, #f8f5ee 0%, var(--bg) 100%);
    }}
    .wrap {{
      max-width: 1320px;
      margin: 0 auto;
      padding: 36px 28px 40px;
    }}
    h1 {{
      font-family: "IBM Plex Serif", Georgia, serif;
      margin: 0 0 10px;
      font-size: 40px;
      line-height: 1.05;
    }}
    .subtitle {{
      max-width: 960px;
      color: var(--muted);
      font-size: 17px;
      line-height: 1.45;
      margin-bottom: 24px;
    }}
    .grid {{
      display: grid;
      grid-template-columns: 1.05fr 0.95fr;
      gap: 20px;
      align-items: start;
    }}
    .panel {{
      background: var(--panel);
      border: 1px solid rgba(216,208,192,0.9);
      border-radius: 22px;
      padding: 20px 22px;
      box-shadow: 0 12px 30px rgba(34, 48, 59, 0.06);
    }}
    h2 {{
      margin: 0 0 14px;
      font-size: 18px;
      letter-spacing: 0.02em;
      text-transform: uppercase;
    }}
    .matrix {{
      width: 100%;
      border-collapse: collapse;
      font-size: 14px;
    }}
    .matrix th, .matrix td {{
      padding: 11px 10px;
      border-bottom: 1px solid #ece6db;
      text-align: left;
      vertical-align: middle;
    }}
    .matrix thead th {{
      color: var(--muted);
      font-size: 12px;
      letter-spacing: 0.05em;
      text-transform: uppercase;
    }}
    .badge {{
      display: inline-block;
      padding: 4px 9px;
      border-radius: 999px;
      font-weight: 700;
      font-size: 12px;
      letter-spacing: 0.03em;
    }}
    .notes {{
      display: grid;
      gap: 12px;
    }}
    .callout {{
      padding: 14px 15px;
      border-radius: 16px;
      background: #f6f8fb;
      border: 1px solid #e0e7ef;
    }}
    .callout strong {{
      display: block;
      margin-bottom: 4px;
    }}
    .callout.blocked {{
      background: #fff1f1;
      border-color: #f4caca;
    }}
    .chart {{
      width: 100%;
      height: auto;
      display: block;
    }}
    .title {{
      font-size: 18px;
      font-weight: 700;
      fill: #162027;
    }}
    .legend, .axis-label {{
      font-size: 12px;
      fill: #5f6b76;
    }}
    .axis {{
      font-size: 12px;
      fill: #5f6b76;
    }}
    .value {{
      font-size: 11px;
      fill: #162027;
    }}
    .axis-line {{
      stroke: #71808e;
      stroke-width: 1.2;
    }}
    .grid {{
      stroke: #e3ddd2;
      stroke-width: 1;
    }}
    .grid-vert {{
      stroke: #ece6db;
      stroke-width: 1;
      stroke-dasharray: 4 6;
    }}
    .foot {{
      margin-top: 18px;
      color: var(--muted);
      font-size: 13px;
      line-height: 1.5;
    }}
    .source-list {{
      margin: 8px 0 0;
      padding-left: 18px;
      color: var(--muted);
      font-size: 13px;
    }}
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Publication Matrix Snapshot</h1>
    <div class="subtitle">
      Trusted publication-ready data for Embarcadero and Corfu is compared against the latest trustworthy Scalog reruns.
      The key point is that Scalog throughput is now clean through RF=2, while Scalog RF=2 latency remains blocked by a real system issue and should not be treated as publication-ready.
    </div>

    <div class="grid">
      <section class="panel">
        <h2>Status Matrix</h2>
        {matrix}
      </section>

      <section class="panel">
        <h2>Readout</h2>
        <div class="notes">
          <div class="callout">
            <strong>Throughput leader at RF=1</strong>
            Scalog leads aggregate throughput at 2 and 3 clients, while Embarcadero stays close at 1 client and dominates RF=1 latency.
          </div>
          <div class="callout">
            <strong>Clean publication status</strong>
            Embarcadero and Corfu are clean across the published RF=1 and RF=2 matrix slices used here. Scalog is clean for all throughput cells plus RF=1 latency.
          </div>
          <div class="callout blocked">
            <strong>Current blocker</strong>
            Scalog RF=2 latency is still blocked. The corrected gate in fix40 and the focused rerun in fix42 both preserved first-attempt failures, so this cell should be read as blocked rather than merely missing.
          </div>
        </div>
      </section>
    </div>

    <section class="panel" style="margin-top:20px;">
      {tput_chart}
    </section>

    <section class="panel" style="margin-top:20px;">
      {lat_chart}
    </section>

    <div class="foot">
      Mixed-root note: Embarcadero/Corfu values come from the trusted publication summaries in fix24/fix25. Scalog values come from the latest trustworthy Scalog publication reruns in fix39, with RF=2 latency status corrected using fix40/fix42 after the latency gate bug was fixed in <code>cd5dde3</code>.
      <ul class="source-list">
        <li>{THROUGHPUT_SUMMARY}</li>
        <li>{LATENCY_SUMMARY}</li>
        <li>{SCALOG_TPUT_ROOT}</li>
        <li>{SCALOG_LAT_RF1}</li>
        <li>{SCALOG_LAT_RF2_FIX40}</li>
        <li>{SCALOG_LAT_RF2_FIX42}</li>
        <li>{SCALOG_MATRIX_LOG}</li>
      </ul>
    </div>
  </div>
</body>
</html>
"""


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    data = build_dataset()
    html = render_html(data)
    (OUTPUT_DIR / "publication_matrix_dashboard.html").write_text(html)

    summary_lines = [
        "system,rf1_tput_n1_gbps,rf1_tput_n2_gbps,rf1_tput_n3_gbps,rf2_tput_n1_gbps,rf2_tput_n2_gbps,rf2_tput_n3_gbps,rf1_latency_status,rf2_latency_status",
    ]
    for system in ("EMBARCADERO", "CORFU", "SCALOG"):
        summary_lines.append(
            ",".join(
                [
                    system,
                    fmt_num(data[system]["throughput"][1][1], 3),
                    fmt_num(data[system]["throughput"][1][2], 3),
                    fmt_num(data[system]["throughput"][1][3], 3),
                    fmt_num(data[system]["throughput"][2][1], 3),
                    fmt_num(data[system]["throughput"][2][2], 3),
                    fmt_num(data[system]["throughput"][2][3], 3),
                    data[system]["latency"][1]["status"],
                    data[system]["latency"][2]["status"],
                ]
            )
        )
    (OUTPUT_DIR / "publication_matrix_snapshot.csv").write_text("\n".join(summary_lines) + "\n")


if __name__ == "__main__":
    main()
