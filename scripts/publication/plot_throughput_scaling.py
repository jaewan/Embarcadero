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
            "--rf0-n4-root",
            str(ROOT / "data/paper_eval/fig1/fig1_regime_n4_07e5f3c6/multiclient/logs/fig1_regime_n4_07e5f3c6"),
            "--commit",
            "c1bca74bdcc4aa4eea7ba52b1ae3b76292c82231",
            "--n4-commit",
            "07e5f3c6f94ce0da917f15272c5410d00dae1089",
            "--n4-ordering-csv",
            str(ROOT / "data/paper_eval/fig1/fig1_ordering_path_n4_cb6bb340/results.csv"),
            "--n4-ordering-manifest",
            str(ROOT / "data/paper_eval/fig1/fig1_ordering_path_n4_cb6bb340/campaign_manifest.json"),
            "--n4-ordering-commit",
            "cb6bb340baa5c3ef531b30d068618bb85535996b",
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
