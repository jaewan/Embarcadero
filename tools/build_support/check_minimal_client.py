#!/usr/bin/env python3
"""Verify actual generated client graph excludes comparison baseline protocols."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
args = parser.parse_args()
build = args.build.resolve()
entries = json.loads((build / 'compile_commands.json').read_text())
for entry in entries:
    name = Path(entry['file']).name
    if name.endswith(('.pb.cc', '.grpc.pb.cc')) and any(x in name for x in ('corfu', 'scalog', 'lazylog')):
        raise SystemExit('Baseline generated source registered: ' + name)
for name in ('corfu', 'scalog', 'lazylog'):
    if list((build / 'src').glob(name + '*.pb.h')):
        raise SystemExit('Baseline generated header exists: ' + name)
binary = build / 'bin/throughput_test'
if not binary.is_file():
    raise SystemExit('Client binary missing')
symbols = subprocess.check_output(['nm', '-C', str(binary)], text=True)
if any(x in symbols for x in ('corfutokenproxy::', 'scalogreplication::', 'lazylogmetadata::')):
    raise SystemExit('Baseline RPC symbols linked into minimal client')
print('Minimal client built: no baseline generated sources, headers, or RPC symbols')
