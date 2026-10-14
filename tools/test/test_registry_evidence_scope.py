#!/usr/bin/env python3
"""Lightweight external-receipt checks; no models or private artifacts required."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ship_audit", ROOT / "tests/quality_gates/audit_ship_pipelines.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "receipt.json"
        self.cnn = {
            "external_receipt_only": True,
            "holdout_receipt": str(self.path),
            "holdout_receipt_schema": "preview_q8_threeway_runtime_fullframe",
            "holdout_expected_rows": 1,
            "holdout_expected_images": 1,
            "dashboard": "dashboard.html",
            "runtime_entrypoint": "renderer.py",
            "evidence_kind": "external_receipt",
            "execution_scope": "offline",
        }
        self.pipeline = {"cnn": "fixture", "ship_class": "PREVIEW", "$role": "ship-preview",
                         "release_selection": "ship_root", "execution_scope": "offline",
                         "evidence_kind": "external_receipt"}
        self.payload = {"summary": {"preview_q8_threeway_runtime_fullframe": {"count": 1, "pass_count": 1}},
                        "rows": [{"image_id": "image", "crop": "detail", "preview_pass": True,
                                  "lpips": 0.1, "ms_ssim": 0.97, "y_psnr": 32.0, "dE2000_mean": 2.0}]}
        self.write_receipt()

    def write_receipt(self):
        self.path.write_text(json.dumps(self.payload))
        self.cnn["holdout_receipt_sha256"] = hashlib.sha256(self.path.read_bytes()).hexdigest()

    def test_valid_and_hash_failures(self):
        self.assertEqual(audit.validate_external_receipt(self.cnn), [])
        self.path.write_text("{}")
        self.assertIn("sha256 mismatch", audit.validate_external_receipt(self.cnn)[0])
        self.path.unlink()
        self.assertIn("file missing", audit.validate_external_receipt(self.cnn)[0])

    def test_requires_pin(self):
        del self.cnn["holdout_receipt_sha256"]
        self.assertIn("pinned", audit.validate_external_receipt(self.cnn)[0])

    def test_bad_metrics_and_coverage(self):
        original = copy.deepcopy(self.payload)
        for metric, value in (("lpips", 0.5), ("ms_ssim", float("nan")),
                              ("y_psnr", 1), ("dE2000_mean", True)):
            self.payload = copy.deepcopy(original)
            self.payload["rows"][0][metric] = value
            self.write_receipt()
            self.assertTrue(audit.validate_external_receipt(self.cnn), metric)
        self.payload = original
        self.payload["rows"] = []
        self.write_receipt()
        self.assertTrue(audit.validate_external_receipt(self.cnn))

    def test_missing_metrics_and_unsupported_schema(self):
        del self.payload["rows"][0]["lpips"]
        self.write_receipt()
        self.assertTrue(audit.validate_external_receipt(self.cnn))
        self.cnn["holdout_receipt_schema"] = "unknown"
        self.assertIn("unsupported", audit.validate_external_receipt(self.cnn)[0])

    def test_declaration_is_not_validation(self):
        registry = {"cnns": {"fixture": self.cnn}, "pipelines": {"fixture": self.pipeline}}
        self.path.unlink()
        with patch.object(audit, "REG", registry), patch.object(audit, "git_tracked_file", return_value=True):
            ok, _, details = audit.external_receipt_checks(self.pipeline)
            self.assertTrue(ok)
            self.assertEqual(details["evidence_status"], "declaration_only")
            self.assertFalse(audit.external_receipt_checks(self.pipeline, True)[0])
            for flags, expected in ((["--strict"], 0), (["--strict-artifacts"], 1)):
                output = io.StringIO()
                with patch.object(sys, "argv", ["audit", *flags]), patch.object(audit, "latest_run", return_value=None), contextlib.redirect_stdout(output):
                    self.assertEqual(audit.main(), expected)
                self.assertNotIn("every ship pipeline has a production-valid", output.getvalue())

    def test_legacy_declaration(self):
        self.pipeline.pop("evidence_kind")
        self.pipeline["$doc"] = "External-receipt production path. Not live/camera-back preview."
        self.cnn["$doc"] = "External-receipt production path."
        with patch.object(audit, "REG", {"cnns": {"fixture": self.cnn}}), patch.object(audit, "git_tracked_file", return_value=True):
            self.assertTrue(audit.external_receipt_checks(self.pipeline)[0])


if __name__ == "__main__":
    unittest.main()
