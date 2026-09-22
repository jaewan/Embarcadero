#!/usr/bin/env python3
"""Analyze a qualified, paired finite-transfer DRAM pilot (no dependencies).

Input: perf harness index.json, with paths relative to that index. Confidence
intervals are paired Student-t intervals on log throughput ratios. They assume
independent, approximately normal pair differences; six pairs cannot establish
those assumptions. Results are exploratory and never a sustained/CXL claim.
"""

import argparse
from collections import Counter
import json
import math
from pathlib import Path
import statistics


METRICS = ("ack_completion_mib_s", "audit_inclusive_e2e_mib_s")
MATCH_FIELDS = ("config_sha256", "common_client_sha256", "cpu_assignment", "payload_bytes",
                "comparison_fingerprint")
# Two-sided 95% Student-t critical values, df=1..30. Larger campaigns use
# df=30 conservatively; do not silently substitute a normal critical value.
T95 = (0, 12.706205, 4.302653, 3.182446, 2.776445, 2.570582,
       2.446912, 2.364624, 2.306004, 2.262157, 2.228139, 2.200985,
       2.178813, 2.160369, 2.144787, 2.131450, 2.119905, 2.109816,
       2.100922, 2.093024, 2.085963, 2.079614, 2.073873, 2.068658,
       2.063899, 2.059539, 2.055529, 2.051831, 2.048407, 2.045230,
       2.042272)


def paired_summary(pairs, margin=0.05):
    """pairs is [(baseline throughput, candidate throughput), ...]."""
    if not pairs:
        return {"pairs": 0, "classification": "inconclusive"}
    if any(not math.isfinite(x) or x <= 0 for pair in pairs for x in pair):
        raise ValueError("throughputs must be finite and positive")
    ratios = [candidate / baseline for baseline, candidate in pairs]
    logs = [math.log(ratio) for ratio in ratios]
    mean = statistics.mean(logs)
    result = {"pairs": len(pairs), "paired_candidate_over_baseline": ratios,
              "raw_pairs_mib_s": [{"baseline": baseline, "candidate": candidate}
                                   for baseline, candidate in pairs],
              "baseline_median_mib_s": statistics.median(p[0] for p in pairs),
              "candidate_median_mib_s": statistics.median(p[1] for p in pairs),
              "geometric_mean_ratio": math.exp(mean),
              "classification": "inconclusive"}
    if len(pairs) < 2:
        return result
    half_width = T95[min(len(pairs) - 1, 30)] * statistics.stdev(logs) / math.sqrt(len(logs))
    low, high = math.exp(mean - half_width), math.exp(mean + half_width)
    result["ratio_ci95"] = [low, high]
    if low > 1 + margin:
        label = "improvement_signal"
    elif high < 1 - margin:
        label = "regression_signal"
    elif low >= 1 - margin and high <= 1 + margin:
        label = "within_practical_band"
    elif low >= 1 - margin:
        label = "bounded_nonregression_signal"
    else:
        label = "inconclusive"
    result["classification"] = label
    return result


def analyze(index_path, expected_pairs=6, margin=0.05):
    index_path = Path(index_path)
    index = json.loads(index_path.read_text())
    if index.get("schema_version") != 1 or not isinstance(index.get("runs"), list):
        raise ValueError("expected schema_version=1 and runs list")
    protocol_revision = index.get("protocol_revision")
    if not isinstance(protocol_revision, str) or not protocol_revision.strip():
        raise ValueError("expected a nonempty protocol_revision")
    manifest_counts = Counter(str((index_path.parent / r["manifest"]).resolve())
                              for r in index["runs"] if isinstance(r.get("manifest"), str))
    output = {"schema_version": 1, "input": str(index_path.resolve()),
              "protocol_revision": protocol_revision,
              "expected_pairs_per_topology": expected_pairs,
              "practical_margin": margin,
              "method": "paired Student-t 95% interval on log throughput ratios",
              "limitations": [
                  "Exploratory small-sample intervals assume independent approximately normal paired log ratios.",
                  "Intervals are per metric/topology, without multiple-comparison correction.",
                  "ACK completion includes publish/Poll work; audited E2E additionally includes serial audit.",
                  "Common-client finite DRAM transfer only; no sustainable, whole-stack, or CXL claim.",
                  "Correctness, ISA, page policy and runtime gates are supplied by the harness, not proved by statistics."],
              "topologies": {}}
    for brokers in (1, 3):
        rows = [r for r in index["runs"] if r.get("brokers") == brokers]
        issues, measurements, qualifications = [], {}, set()
        reference = None
        expected_order = [("qualification", None, "baseline"), ("qualification", None, "candidate")]
        for pair in range(1, expected_pairs + 1):
            variants = ("baseline", "candidate") if pair % 2 else ("candidate", "baseline")
            expected_order.extend(("measurement", pair, variant) for variant in variants)
        if [(r.get("kind"), r.get("pair"), r.get("variant")) for r in rows] != expected_order:
            issues.append("run inventory/order differs from qualifications then consecutive planned AB/BA pairs")
        previous_end = None
        for execution_sequence, row in enumerate(rows, 1):
            identity = f"{row.get('kind')}/{row.get('pair')}/{row.get('variant')}"
            if row.get("variant") not in ("baseline", "candidate"):
                issues.append(f"{identity}: invalid variant")
                continue
            if row.get("status") != "passed":
                issues.append(f"{identity}: status={row.get('status')}")
                continue
            try:
                manifest_path = (index_path.parent / row["manifest"]).resolve()
                if manifest_counts[str(manifest_path)] != 1:
                    raise ValueError("duplicate manifest path")
                manifest = json.loads(manifest_path.read_text())
                if manifest.get("status") != "passed":
                    raise ValueError(f"manifest status={manifest.get('status')}")
                if manifest.get("protocol_revision") != protocol_revision:
                    raise ValueError("index/manifest protocol_revision mismatch")
                if manifest.get("failures") or manifest.get("failure_reasons"):
                    raise ValueError("manifest contains failure reasons")
                for label in ("brokers", "pair", "kind", "variant"):
                    if label not in manifest or manifest[label] != row.get(label):
                        raise ValueError(f"index/manifest {label} mismatch")
                if row.get("execution_sequence") != execution_sequence or manifest.get("execution_sequence") != execution_sequence:
                    raise ValueError("execution sequence does not match planned run order")
                started, ended = float(manifest["started_monotonic"]), float(manifest["ended_monotonic"])
                if (not math.isfinite(started) or not math.isfinite(ended) or started <= 0 or ended < started or
                        (previous_end is not None and started < previous_end)):
                    raise ValueError("invalid/overlapping run timeline")
                previous_end = ended
                expected_broker = index["identities"][row["variant"]]["broker_sha256"]
                if not expected_broker or manifest["artifact_identity"]["broker_sha256"] != expected_broker:
                    raise ValueError("broker binary differs from campaign identity")
                expected_client = index["identities"]["candidate"]["client_sha256"]
                if not expected_client or manifest["common_client_sha256"] != expected_client:
                    raise ValueError("common client differs from campaign identity")
                values = {field: manifest[field] for field in MATCH_FIELDS}
                if any(v is None or v == "" for v in values.values()):
                    raise ValueError("empty comparison identity")
                if reference is None:
                    reference = values
                elif values != reference:
                    raise ValueError("client/config/CPU/payload identity differs within topology")
                metrics = {metric: float(manifest["metrics"][metric]) for metric in METRICS}
                if any(not math.isfinite(v) or v <= 0 for v in metrics.values()):
                    raise ValueError("nonpositive/nonfinite metric")
                if metrics[METRICS[1]] > metrics[METRICS[0]] * 1.00001:
                    raise ValueError("audit-inclusive throughput exceeds ACK completion throughput")
                if row.get("kind") == "qualification":
                    if row.get("included") is not False or row.get("pair") is not None:
                        raise ValueError("qualification must be excluded and unpaired")
                    if row["variant"] in qualifications:
                        raise ValueError("duplicate qualification variant")
                    qualifications.add(row["variant"])
                elif row.get("kind") == "measurement":
                    if row.get("included") is not True:
                        raise ValueError("measurement was excluded; cannot select successful samples")
                    pair = row.get("pair")
                    if type(pair) is not int or not 1 <= pair <= expected_pairs:
                        raise ValueError("measurement pair outside planned range")
                    key = (pair, row["variant"])
                    if key in measurements:
                        raise ValueError("duplicate pair/variant")
                    measurements[key] = metrics
                else:
                    raise ValueError("unknown run kind")
            except (OSError, KeyError, ValueError, TypeError) as error:
                issues.append(f"{identity}: {error}")
        if qualifications != {"baseline", "candidate"}:
            issues.append("both excluded qualification runs must pass")
        complete = [pair for pair in range(1, expected_pairs + 1)
                    if (pair, "baseline") in measurements and (pair, "candidate") in measurements]
        if len(complete) != expected_pairs:
            issues.append(f"only {len(complete)}/{expected_pairs} complete measurement pairs")
        first_variants = []
        for pair in complete:
            first = next(r for r in rows if r.get("kind") == "measurement" and r.get("pair") == pair)
            first_variants.append(first["variant"])
        if abs(first_variants.count("baseline") - first_variants.count("candidate")) > 1:
            issues.append("AB/BA pair order is not balanced")
        summary = {"qualification": "passed" if not issues else "unqualified",
                   "issues": issues, "matched_identity": reference, "metrics": {}}
        for metric in METRICS:
            result = paired_summary([(measurements[pair, "baseline"][metric],
                                      measurements[pair, "candidate"][metric]) for pair in complete], margin)
            if issues:
                result["descriptive_classification_only"] = result["classification"]
                result["classification"] = "unqualified"
            summary["metrics"][metric] = result
        output["topologies"][str(brokers)] = summary
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("index", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--pairs", type=int, default=6)
    parser.add_argument("--margin", type=float, default=0.05)
    args = parser.parse_args()
    if args.pairs < 2 or not 0 < args.margin < 1:
        parser.error("require at least two pairs and 0 < margin < 1")
    result = analyze(args.index, args.pairs, args.margin)
    rendered = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
