#!/usr/bin/env python3
"""Regression tests for Mission 1 camera source probing."""
from __future__ import annotations

import importlib.util
import base64
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/mission1_camera_source_probe.py"
CHECK_TOOL = ROOT / "tools/check_mission1_camera_source_probe.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("mission1_camera_source_probe", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def parse(tool, *argv: str):
    return tool.parser().parse_args(list(argv))


def main() -> int:
    tool = load_tool()
    work_parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
    work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mission1_camera_source_", dir=work_parent) as td:
        root = Path(td)
        raw = root / "frame.raw"
        raw.write_bytes(b"\0" * (16 * 12 * 2))
        file_report = tool.build_report(
            parse(
                tool,
                "--raw",
                str(raw),
                "--raw-source-kind",
                "file_standin",
                "--source-width",
                "16",
                "--source-height",
                "12",
            )
        )
        assert file_report["verdict"]["source_ready"] is True
        assert file_report["blockers"] == []

        fifo = root / "sensor_dma_ring"
        os.mkfifo(fifo)
        camera_report = tool.build_report(
            parse(
                tool,
                "--raw",
                str(fifo),
                "--raw-source-kind",
                "sensor_dma_capture",
                "--source-width",
                "16",
                "--source-height",
                "12",
            )
        )
        assert camera_report["verdict"]["source_ready"] is True
        assert camera_report["blockers"] == []
        discovery = camera_report["probe"].get("discovery", {})
        assert any(row.get("path") == str(fifo) for row in discovery.get("device_like_raw_candidates", []))
        assert any(
            check["name"] == "camera raw source endpoint is device-like" and check["passed"] is True
            for check in camera_report["checks"]
        )
        assert any(
            check["name"] == "camera source discovery completed" and check["passed"] is True
            for check in camera_report["checks"]
        )

        ready_receipt = root / "camera_source_ready.json"
        ready_receipt.write_text(json.dumps(camera_report, indent=2) + "\n", encoding="utf-8")
        check_ready = subprocess.run(
            [sys.executable, str(CHECK_TOOL), str(ready_receipt)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert check_ready.returncode == 0, check_ready.stderr

        config_b64 = base64.b64encode(json.dumps({"raw": str(fifo)}).encode("utf-8")).decode("ascii")
        proc = subprocess.run(
            ["python3", "-"],
            input=f"CONFIG_B64 = {config_b64!r}\n{tool.REMOTE_SCRIPT}",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert proc.returncode == 0, proc.stderr
        remote_payload = json.loads(proc.stdout)
        assert remote_payload["path"]["path"] == str(fifo)
        assert any(
            row.get("path") == str(fifo)
            for row in remote_payload.get("discovery", {}).get("device_like_raw_candidates", [])
        )

        bad_camera_report = tool.build_report(
            parse(
                tool,
                "--raw",
                str(raw),
                "--raw-source-kind",
                "sensor_dma_capture",
                "--source-width",
                "16",
                "--source-height",
                "12",
            )
        )
        assert bad_camera_report["verdict"]["source_ready"] is False
        assert "camera raw source endpoint is not a device-like stream" in bad_camera_report["blockers"]
        bad_receipt = root / "camera_source_bad.json"
        bad_receipt.write_text(json.dumps(bad_camera_report, indent=2) + "\n", encoding="utf-8")
        check_bad = subprocess.run(
            [sys.executable, str(CHECK_TOOL), str(bad_receipt)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert check_bad.returncode == 1
        assert "source_ready is false" in check_bad.stderr

        missing_report = tool.build_report(
            parse(
                tool,
                "--raw",
                str(root / "missing_ring"),
                "--raw-source-kind",
                "sensor_dma_capture",
            )
        )
        assert missing_report["verdict"]["source_ready"] is False
        assert "camera raw source endpoint is missing on target" in missing_report["blockers"]

    print("test_mission1_camera_source_probe: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
