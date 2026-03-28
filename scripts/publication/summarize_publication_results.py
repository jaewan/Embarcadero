#!/usr/bin/env python3
import csv
import sys
from pathlib import Path


def combine(pattern: str, out_file: Path) -> int:
    rows = []
    header = None
    for path in sorted(Path(".").glob(pattern)):
      with path.open(newline="") as f:
        reader = csv.reader(f)
        try:
          current_header = next(reader)
        except StopIteration:
          continue
        if header is None:
          header = current_header
        elif header != current_header:
          raise SystemExit(f"header mismatch in {path}")
        rows.extend(reader)
    if header is None:
      return 1
    out_file.parent.mkdir(parents=True, exist_ok=True)
    with out_file.open("w", newline="") as f:
      writer = csv.writer(f)
      writer.writerow(header)
      writer.writerows(rows)
    return 0


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: summarize_publication_results.py <throughput|latency> <tag>", file=sys.stderr)
        return 2
    family, tag = sys.argv[1], sys.argv[2]
    if family not in {"throughput", "latency"}:
        print(f"unsupported family: {family}", file=sys.stderr)
        return 2
    root = Path("data/publication") / family / tag
    pattern = str(root / "*" / "run_*" / "summary.csv")
    out_file = root / f"{family}_summary.csv"
    rc = combine(pattern, out_file)
    if rc != 0:
        print(f"no summaries found under {root}", file=sys.stderr)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
