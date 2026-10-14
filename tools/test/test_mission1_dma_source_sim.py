#!/usr/bin/env python3
"""Regression test for the deterministic Mission 1 DMA source simulator."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/mission1_dma_source_sim.py"


def main() -> int:
    work_parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
    work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mission1_dma_source_sim_", dir=work_parent) as td:
        root = Path(td)
        out = root / "receipt.json"
        proc = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--output",
                str(out),
                "--work-dir",
                str(root / "fifo"),
                "--source-width",
                "32",
                "--source-height",
                "24",
                "--frames",
                "8",
                "--target-fps",
                "40",
                "--delay-pattern-ms",
                "0,1,0,2",
                "--consumer-delay-ms",
                "0.25",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if proc.returncode != 0:
            print(proc.stdout)
            print(proc.stderr, file=sys.stderr)
            return proc.returncode
        receipt = json.loads(out.read_text(encoding="utf-8"))
        assert receipt["schema"] == "gpr.mission1_dma_source_sim.v1"
        assert receipt["target"]["role"] == "stand-in"
        assert receipt["target"]["not_camera_evidence"] is True
        assert receipt["source"]["raw_source_kind"] == "sensor_dma_capture"
        assert receipt["source"]["frame_bytes"] == 32 * 24 * 2
        assert receipt["producer"]["process"] == "separate"
        assert receipt["consumer"]["process"] == "separate"
        assert receipt["producer"]["frames_written"] == 8
        assert receipt["consumer"]["complete_frames"] == 8
        assert receipt["verdict"]["source_ready"] is True
        assert receipt["verdict"]["deterministic_simulation"] is True
        assert receipt["verdict"]["production_evidence"] is False
        assert receipt["verdict"]["hashes_match"] is True
        assert receipt["consumer"]["frame_intervals_ms"]["n"] == 7
        assert Path(receipt["artifacts"]["producer_report"]).exists()
        assert Path(receipt["artifacts"]["consumer_report"]).exists()

    print("test_mission1_dma_source_sim: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
