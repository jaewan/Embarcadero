#!/usr/bin/env python3
"""Compatibility entry point for the canonical paper Fig. 1 generator."""

import runpy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if len(sys.argv) == 1:
    sys.argv.extend(
        [
            "--embar-csv",
            str(ROOT / "data/paper_eval/fig1/fig1_embar_official_3eaadffb/results.csv"),
            "--scalog-csv",
            str(ROOT / "data/paper_eval/fig1/fig1_scalog_official_3eaadffb/results.csv"),
            "--commit",
            "3eaadffb61eb3c4698aeb16a1cc845ed456ea035",
            "--pdf",
            str(ROOT / "data/paper_eval/fig1/throughput_scaling.pdf"),
            "--png",
            str(ROOT / "data/paper_eval/fig1/throughput_scaling.png"),
            "--manifest",
            str(ROOT / "data/paper_eval/fig1/fig1_fixed_commit_3eaadffb_manifest.json"),
            "--selected-csv",
            str(ROOT / "data/paper_eval/fig1/fig1_fixed_commit_3eaadffb_selected.csv"),
        ]
    )
runpy.run_path(
    str(ROOT / "PaperScripts" / "plot_fig1_throughput_scaling.py"),
    run_name="__main__",
)
