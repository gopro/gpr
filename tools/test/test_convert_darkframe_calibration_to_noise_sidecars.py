#!/usr/bin/env python3
"""Regression test for legacy darkframe calibration sidecar conversion."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONVERTER = ROOT / "tools/convert_darkframe_calibration_to_noise_sidecars.py"
CHECKER = ROOT / "tools/check_camera_noise_calibration.py"


def temp_root() -> Path:
    if os.environ.get("GPR_TMPDIR"):
        root = Path(os.environ["GPR_TMPDIR"])
    else:
        root = Path(tempfile.gettempdir())
    root.mkdir(parents=True, exist_ok=True)
    return root


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="gpr_legacy_darkcal_", dir=temp_root()) as tmp:
        base = Path(tmp)
        sources = []
        for idx in range(5):
            path = base / f"dark_{idx:02d}.raw"
            path.write_bytes(bytes([idx + 1]) * 32)
            sources.append(path)

        legacy = {
            "kind": "darkframe_calibration",
            "discovery_rows": [
                {
                    "path": str(path),
                    "make": "Fixture",
                    "model": "Large Bayer",
                    "iso": 1600,
                    "exposure_time": 0.01,
                    "dark_candidate": True,
                    "raw_shape": [12, 16],
                    "pattern": [[0, 1], [3, 2]],
                    "color_desc": "RGBG",
                    "black_by_site": {"R00": 64.0, "G01": 65.0, "G10": 66.0, "B11": 67.0},
                    "white": 16383.0,
                }
                for path in sources
            ],
            "calibration_groups": [
                {
                    "key": "Fixture Large Bayer ISO1600 exp0.01",
                    "frame_count": 5,
                    "available_candidate_count": 5,
                    "make": "Fixture",
                    "model": "Large Bayer",
                    "iso": 1600,
                    "exposure_time": 0.01,
                    "raw_shape": [12, 16],
                    "calibration_stride": 1,
                    "per_site": {
                        site: {
                            "frames": 5,
                            "mean_residual_counts": 1.0,
                            "spatial_fpn_rms_counts": 0.5,
                            "row_fpn_rms_counts": 0.2,
                            "col_fpn_rms_counts": 0.3,
                            "temporal_noise_rms_counts": sigma,
                            "temporal_noise_p95_counts": sigma * 2,
                        }
                        for site, sigma in {"R00": 1.1, "G01": 1.2, "G10": 1.3, "B11": 1.4}.items()
                    },
                    "artifacts": {},
                }
            ],
        }
        legacy_path = base / "darkframe_calibration.json"
        legacy_path.write_text(json.dumps(legacy), encoding="utf-8")

        out_dir = base / "out"
        subprocess.run([sys.executable, str(CONVERTER), "--legacy-json", str(legacy_path), "--out-dir", str(out_dir)], check=True)
        sidecars = sorted(out_dir.glob("*_noise_calibration.json"))
        assert len(sidecars) == 1, sidecars
        subprocess.run([sys.executable, str(CHECKER), str(sidecars[0])], check=True)

        payload = json.loads(sidecars[0].read_text(encoding="utf-8"))
        assert payload["camera"]["cfa_phase"] == "RGGB"
        assert payload["camera"]["width"] == 16
        assert payload["calibrations"][0]["sample_count"] == 5
        assert payload["calibrations"][0]["usable_for_training_targets"] is True
        assert payload["calibrations"][0]["source"]["sha256"]
        assert payload["calibrations"][0]["per_plane"]["b"]["sigma_black"] == 1.4

    print("test_convert_darkframe_calibration_to_noise_sidecars: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
