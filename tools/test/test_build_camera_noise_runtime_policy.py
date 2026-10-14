#!/usr/bin/env python3
"""Regression test for the camera-noise runtime policy builder."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/build_camera_noise_runtime_policy.py"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="gpr_noise_runtime_policy_") as td:
        out_dir = Path(td) / "policy"
        proc = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--synthetic",
                "--output-dir",
                str(out_dir),
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0:
            print(proc.stdout)
            print(proc.stderr, file=sys.stderr)
            return proc.returncode
        data = json.loads((out_dir / "camera_noise_runtime_policy.json").read_text(encoding="utf-8"))
        assert data["schema"] == "gpr.camera_noise_runtime_policy.v1"
        assert data["source_coverage_schema"] == "gpr.camera_noise_coverage_audit.v1"
        assert data["summary"]["nonzero_noise_addback_camera_keys"] == ["x2d", "z8"]
        assert data["summary"]["metadata_only_camera_keys"] == ["mission1", "iphone"]
        assert data["summary"]["production_noise_policy_complete"] is False
        policies = {row["camera_key"]: row for row in data["camera_policies"]}
        assert policies["x2d"]["allow_denoised_training_targets"] is True
        assert policies["x2d"]["allow_nonzero_noise_addback"] is True
        assert policies["mission1"]["allow_nonzero_noise_addback"] is False
        assert policies["mission1"]["runtime_fallback"]["mode"] == "metadata_conditioning_only"
        assert policies["iphone"]["iso_policy"] == "metadata_only"
        assert any("REF/source image residuals" in rule for rule in data["rules"])
        html = (out_dir / "index.html").read_text(encoding="utf-8")
        assert "Camera Noise Runtime Policy" in html
        assert "Metadata-only" in html
        assert proc.stdout.strip() == str(out_dir / "index.html")
    print("test_build_camera_noise_runtime_policy: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
