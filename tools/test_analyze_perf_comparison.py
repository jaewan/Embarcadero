import json
from pathlib import Path
import tempfile
import unittest

from analyze_perf_comparison import analyze, paired_summary


class ComparisonTests(unittest.TestCase):
    def write_index(self, path, runs):
        path.write_text(json.dumps({"schema_version": 1, "protocol_revision": "paired-dram-v2-authoritative-order5-ack",
            "runs": runs, "identities": {
            variant: {"broker_sha256": f"broker-{variant}", "client_sha256": "client"}
            for variant in ("baseline", "candidate")}}))

    def test_direction_and_practical_band(self):
        for ratio, label in ((1.10, "improvement_signal"), (0.90, "regression_signal"),
                             (1.0, "within_practical_band")):
            summary = paired_summary([(100, 100 * ratio)] * 6)
            self.assertAlmostEqual(summary["geometric_mean_ratio"], ratio)
            self.assertEqual(summary["classification"], label)
        self.assertEqual(paired_summary([(100, 80), (100, 120)])[
            "classification"], "inconclusive")

    def make_campaign(self, root):
        runs = []
        for brokers in (1, 3):
            sequence = 0
            for pair in (None, 1, 2, 3, 4, 5, 6):
                variants = ["baseline", "candidate"]
                if pair and pair % 2 == 0:
                    variants.reverse()
                for variant in variants:
                    sequence += 1
                    name = f"{brokers}-{pair}-{variant}.json"
                    metrics = {"ack_completion_mib_s": 100, "audit_inclusive_e2e_mib_s": 50}
                    if variant == "candidate":
                        metrics = {key: value * 1.10 for key, value in metrics.items()}
                    (root / name).write_text(json.dumps({
                        "config_sha256": "config", "common_client_sha256": "client",
                        "cpu_assignment": {"brokers": brokers}, "payload_bytes": 2**31,
                        "comparison_fingerprint": f"matched-environment-{brokers}",
                        "status": "passed",
                        "protocol_revision": "paired-dram-v2-authoritative-order5-ack",
                        "brokers": brokers, "pair": pair, "variant": variant,
                        "kind": "qualification" if pair is None else "measurement",
                        "execution_sequence": sequence, "started_monotonic": sequence * 2,
                        "ended_monotonic": sequence * 2 + 1,
                        "artifact_identity": {"broker_sha256": f"broker-{variant}"},
                        "failures": [], "metrics": metrics}))
                    runs.append({"brokers": brokers, "pair": pair, "variant": variant,
                                 "kind": "qualification" if pair is None else "measurement",
                                 "execution_sequence": sequence,
                                 "included": pair is not None, "status": "passed", "manifest": name})
        path = root / "index.json"
        self.write_index(path, runs)
        return path, runs

    def test_balanced_valid_campaign(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, _ = self.make_campaign(Path(tmp))
            result = analyze(path)
            for summary in result["topologies"].values():
                self.assertEqual(summary["qualification"], "passed")
                self.assertEqual(summary["metrics"]["ack_completion_mib_s"]["classification"],
                                 "improvement_signal")

    def test_failed_trial_cannot_be_hidden_by_other_successes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, rows = self.make_campaign(Path(tmp))
            rows[3]["status"] = "failed"
            rows[3]["included"] = False
            self.write_index(path, rows)
            summary = analyze(path)["topologies"]["1"]
            self.assertEqual(summary["qualification"], "unqualified")
            self.assertEqual(summary["metrics"]["ack_completion_mib_s"]["classification"], "unqualified")

    def test_client_drift_invalidates_campaign(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, rows = self.make_campaign(Path(tmp))
            manifest_path = path.parent / rows[3]["manifest"]
            manifest = json.loads(manifest_path.read_text())
            manifest["common_client_sha256"] = "another-client"
            manifest_path.write_text(json.dumps(manifest))
            summary = analyze(path)["topologies"]["1"]
            self.assertEqual(summary["qualification"], "unqualified")
            self.assertTrue(any("identity" in issue for issue in summary["issues"]))

    def test_manifest_status_and_duplicate_evidence_fail_closed(self):
        for defect in ("failed-manifest", "reused-manifest", "duplicate-qualification"):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                path, rows = self.make_campaign(Path(tmp))
                if defect == "failed-manifest":
                    manifest_path = path.parent / rows[3]["manifest"]
                    manifest = json.loads(manifest_path.read_text())
                    manifest["status"] = "failed"
                    manifest_path.write_text(json.dumps(manifest))
                elif defect == "reused-manifest":
                    rows[3]["manifest"] = rows[2]["manifest"]
                else:
                    duplicate = dict(rows[0])
                    duplicate["manifest"] = "duplicate-qualification.json"
                    (path.parent / duplicate["manifest"]).write_text(
                        (path.parent / rows[0]["manifest"]).read_text())
                    rows.append(duplicate)
                self.write_index(path, rows)
                self.assertEqual(analyze(path)["topologies"]["1"]["qualification"], "unqualified")

    def test_mislabeled_artifact_binary_drift_and_overlap_rejected(self):
        for defect in ("label", "binary", "overlap", "reordered"):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                path, rows = self.make_campaign(Path(tmp))
                manifest_path = path.parent / rows[3]["manifest"]
                manifest = json.loads(manifest_path.read_text())
                if defect == "label":
                    manifest["variant"] = "baseline"
                elif defect == "binary":
                    manifest["artifact_identity"]["broker_sha256"] = "replaced-binary"
                elif defect == "overlap":
                    manifest["started_monotonic"] = 1
                else:
                    rows[3], rows[2] = rows[2], rows[3]
                manifest_path.write_text(json.dumps(manifest))
                self.write_index(path, rows)
                self.assertEqual(analyze(path)["topologies"]["1"]["qualification"], "unqualified")

    def test_protocol_revision_required_and_bound_across_topologies(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, rows = self.make_campaign(Path(tmp))
            # Even an internally consistent second topology cannot use another
            # protocol under the same campaign index.
            for row in rows:
                if row["brokers"] != 3:
                    continue
                manifest_path = path.parent / row["manifest"]
                manifest = json.loads(manifest_path.read_text())
                manifest["protocol_revision"] = "paired-dram-v1"
                manifest_path.write_text(json.dumps(manifest))
            result = analyze(path)
            self.assertEqual(result["topologies"]["1"]["qualification"], "passed")
            self.assertEqual(result["topologies"]["3"]["qualification"], "unqualified")
            self.assertTrue(any("protocol_revision mismatch" in issue
                                for issue in result["topologies"]["3"]["issues"]))
            original = json.loads(path.read_text())
            for revision in (None, "", "   "):
                invalid = dict(original)
                if revision is None:
                    invalid.pop("protocol_revision")
                else:
                    invalid["protocol_revision"] = revision
                path.write_text(json.dumps(invalid))
                with self.assertRaisesRegex(ValueError, "nonempty protocol_revision"):
                    analyze(path)


if __name__ == "__main__":
    unittest.main()
