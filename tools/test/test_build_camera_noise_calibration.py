#!/usr/bin/env python3
"""Regression test for the camera-noise calibration sidecar builder."""

from __future__ import annotations

import json
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / "tools/build_camera_noise_calibration.py"
CHECKER = ROOT / "tools/check_camera_noise_calibration.py"


def temp_root() -> Path:
    if os.environ.get("GPR_TMPDIR"):
        root = Path(os.environ["GPR_TMPDIR"])
    else:
        root = Path(tempfile.gettempdir())
    root.mkdir(parents=True, exist_ok=True)
    return root


def main() -> int:
    width = 16
    height = 12
    black = 64.0
    white = 16383.0
    rng = random.Random(1337)

    with tempfile.TemporaryDirectory(prefix="gpr_noise_cal_", dir=temp_root()) as tmp:
        tmp_path = Path(tmp)
        raw_paths = []
        for idx in range(6):
            values = []
            for row in range(height):
                for col in range(width):
                    value = black + rng.gauss(0.0, 1.5)
                    if row % 2 == 0 and col % 2 == 1:
                        value += rng.gauss(0.0, 0.3)
                    values.append(max(0, min(int(round(white)), int(round(value)))))
            path = tmp_path / f"dark_{idx:02d}.raw"
            path.write_bytes(b"".join(v.to_bytes(2, "little") for v in values))
            raw_paths.append(path)

        receipt = tmp_path / "noise_calibration.json"
        cmd = [
            sys.executable,
            str(BUILDER),
            "--width",
            str(width),
            "--height",
            str(height),
            "--bit-depth",
            "14",
            "--cfa-phase",
            "GRBG",
            "--iso",
            "1600",
            "--make",
            "Fixture",
            "--model",
            "Synthetic Dark",
            "--black-level",
            str(black),
            "--white-level",
            str(white),
            "--out",
            str(receipt),
        ]
        for path in raw_paths:
            cmd.extend(["--raw", str(path)])
        subprocess.run(cmd, check=True)
        subprocess.run([sys.executable, str(CHECKER), str(receipt)], check=True)

        payload = json.loads(receipt.read_text(encoding="utf-8"))
        assert payload["schema"] == "gpr.camera_noise_calibration.v1"
        assert payload["camera"]["cfa_phase"] == "GRBG"
        assert payload["calibrations"][0]["usable_for_training_targets"] is True
        assert payload["calibrations"][0]["noise_signal_audit"]["source_provenance_required"] is False
        planes = payload["calibrations"][0]["per_plane"]
        assert 0.5 < planes["r"]["sigma_black"] < 3.0
        assert planes["r"]["noise_profile_offset"] > 0.0

        for field in ("source_hash", "signal_audit", "sample_count", "schema"):
            invalid = json.loads(json.dumps(payload))
            calibration = invalid["calibrations"][0]
            if field == "source_hash":
                calibration["source"]["sha256"] = "invalid"
            elif field == "signal_audit":
                calibration["noise_signal_audit"]["separates_noise_from_signal"] = False
            elif field == "sample_count":
                calibration["sample_count"] = 1
            else:
                invalid["schema"] = "unsupported"
            invalid_path = tmp_path / f"invalid_{field}.json"
            invalid_path.write_text(json.dumps(invalid), encoding="utf-8")
            checked = subprocess.run(
                [sys.executable, str(CHECKER), str(invalid_path)],
                capture_output=True, text=True,
            )
            assert checked.returncode == 1, (field, checked.stdout, checked.stderr)

        strict_manifest = tmp_path / "strict_source_provenance.json"
        strict_manifest.write_text(
            json.dumps(
                {
                    "schema": "gpr.darkframe_raw_source_provenance.v1",
                    "frames": [
                        {
                            "raw_path": path.as_posix(),
                            "raw_sha256": payload_sha(path),
                            "original_path": f"/fixtures/original_{idx:02d}.DNG",
                            "original_sha256": f"{idx:064x}"[-64:],
                            "extract_receipt": f"/fixtures/extract_{idx:02d}.json",
                            "no_scene_signal": True,
                            "capture_setup": "lens cap on",
                        }
                        for idx, path in enumerate(raw_paths)
                    ],
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        strict_receipt = tmp_path / "noise_calibration_strict.json"
        strict_cmd = cmd.copy()
        strict_cmd[strict_cmd.index(str(receipt))] = str(strict_receipt)
        strict_cmd.extend(["--source-provenance-manifest", str(strict_manifest), "--require-source-provenance"])
        subprocess.run(strict_cmd, check=True)
        strict_payload = json.loads(strict_receipt.read_text(encoding="utf-8"))
        strict_cal = strict_payload["calibrations"][0]
        assert strict_payload["production_ready"] is True
        assert strict_cal["noise_signal_audit"]["source_provenance_required"] is True
        assert strict_cal["noise_signal_audit"]["source_provenance_ready"] is True
        assert strict_cal["noise_signal_audit"]["separates_noise_from_signal"] is True
        assert len(strict_cal["source"]["frames"]) == len(raw_paths)
        assert all(row["source_provenance_ready"] for row in strict_cal["source"]["frames"])

        bad_manifest = tmp_path / "bad_source_provenance.json"
        bad_data = json.loads(strict_manifest.read_text(encoding="utf-8"))
        bad_data["frames"][0]["no_scene_signal"] = False
        bad_manifest.write_text(json.dumps(bad_data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        bad_cmd = cmd.copy()
        bad_cmd[bad_cmd.index(str(receipt))] = str(tmp_path / "bad_noise_calibration.json")
        bad_cmd.extend(["--source-provenance-manifest", str(bad_manifest), "--require-source-provenance"])
        bad = subprocess.run(bad_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert bad.returncode == 2
        assert "source provenance is required" in bad.stderr

    print("test_build_camera_noise_calibration: PASS")
    return 0


def payload_sha(path: Path) -> str:
    import hashlib

    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


if __name__ == "__main__":
    sys.exit(main())
