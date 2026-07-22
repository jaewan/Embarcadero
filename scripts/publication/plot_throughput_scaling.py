#!/usr/bin/env python3
"""Compatibility entry point for the canonical paper Fig. 1 generator."""

import runpy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if len(sys.argv) == 1:
    sys.argv.extend(
        [
            "--rf2-csv",
            str(ROOT / "data/paper_eval/fig1/fig1_regime_bars_c1bca74b/results.csv"),
            "--rf0-root",
            str(ROOT / "data/paper_eval/fig1/fig1_regime_bars_c1bca74b/rf0/multiclient/logs/fig1_regime_rf0_c1bca74b"),
            "--commit",
            "c1bca74bdcc4aa4eea7ba52b1ae3b76292c82231",
            "--pdf",
            str(ROOT / "data/paper_eval/fig1/throughput_scaling.pdf"),
            "--paper-pdf",
            str(ROOT / "Paper/Figures/throughput_scaling.pdf"),
            "--png",
            str(ROOT / "data/paper_eval/fig1/throughput_scaling.png"),
            "--manifest",
            str(ROOT / "data/paper_eval/fig1/fig1_regime_bars_c1bca74b_manifest.json"),
            "--selected-csv",
            str(ROOT / "data/paper_eval/fig1/fig1_regime_bars_c1bca74b_selected.csv"),
        ]
    )
runpy.run_path(
    str(ROOT / "PaperScripts" / "plot_fig1_throughput_scaling.py"),
    run_name="__main__",
)
