#!/usr/bin/env python3
"""Regression tests for Mission 1 camera target preflight."""
from __future__ import annotations

import importlib.util
import os
import stat
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/mission1_camera_target_preflight.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("mission1_camera_target_preflight", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def make_executable(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def has_executable_candidate(rows: list[dict], suffix: str) -> bool:
    return any(row.get("executable") is True and str(row.get("path", "")).endswith(suffix) for row in rows)


def args(
    tool,
    root: Path,
    *,
    camera_flags: bool,
    layout: str = "build",
    camera_labels: bool = True,
    standin_label: bool = False,
    raw_source_kind: str = "file_standin",
    camera_endpoint_exists: bool = True,
):
    repo = root / "repo"
    repo.mkdir(parents=True)
    (repo / ".git").mkdir()
    if raw_source_kind == "file_standin":
        raw = root / "GP017602.raw"
        raw.write_bytes(b"\0" * (16 * 12 * 2))
    else:
        raw = root / "sensor_dma_ring"
        if camera_endpoint_exists:
            os.mkfifo(raw)
    if layout == "build-closure":
        make_executable(repo / "build-closure/source/app/bench_fused/bench_fused")
        make_executable(repo / "build-closure/bin/labs_encoder_bench_cli")
        make_executable(repo / "build-closure/bin/fused_decode_cli")
        make_executable(repo / "build-closure/bin/gvid_preview_rgb_cli")
    else:
        make_executable(repo / "build/bin/bench_fused")
        make_executable(repo / "build/bin/labs_encoder_bench_cli")
        make_executable(repo / "build/bin/fused_decode_cli")
        make_executable(repo / "build/bin/gvid_preview_rgb_cli")
    argv = [
        "--repo-root",
        str(repo),
        "--raw",
        str(raw),
        "--output-dir",
        str(root / "out"),
        "--scratch-dir",
        str(root / "tmp"),
        "--source-width",
        "16",
        "--source-height",
        "12",
        "--raw-source-kind",
        raw_source_kind,
    ]
    if camera_labels:
        argv += [
            "--frame-source",
            "sensor DMA ring buffer" if not standin_label else "file-backed stand-in source",
            "--write-path",
            "Mission 1 camera storage writer",
            "--storage-medium",
            "Mission 1 SD card",
            "--display-surface",
            "Mission 1 rear display",
            "--presentation-path",
            "Mission 1 display presenter",
        ]
    argv += (
        [
            "--camera-frame-source-ready",
            "--camera-storage-path-ready",
            "--camera-display-path-ready",
        ]
        if camera_flags
        else []
    )
    return tool.parser().parse_args(argv)


def main() -> int:
    tool = load_tool()
    work_parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
    work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mission1_target_preflight_", dir=work_parent) as td:
        blocked = tool.build_report(args(tool, Path(td) / "blocked", camera_flags=False))
        assert blocked["verdict"]["target_preflight_ready"] is False
        assert blocked["verdict"]["camera_closure_possible"] is False
        assert "camera frame source ready" in blocked["blockers"]

    with tempfile.TemporaryDirectory(prefix="mission1_target_preflight_", dir=work_parent) as td:
        ready = tool.build_report(args(tool, Path(td) / "ready", camera_flags=True))
        assert ready["verdict"]["target_preflight_ready"] is True
        assert ready["verdict"]["camera_closure_possible"] is True
        assert ready["blockers"] == []
        assert ready["inputs"]["frame_source"] == "sensor DMA ring buffer"
        executable_names = ready["target_probe"]["executables"].keys()
        assert "labs_encoder_bench_cli" in executable_names

    with tempfile.TemporaryDirectory(prefix="mission1_target_preflight_", dir=work_parent) as td:
        camera_endpoint = tool.build_report(
            args(tool, Path(td) / "camera_endpoint", camera_flags=True, raw_source_kind="sensor_dma_capture")
        )
        assert camera_endpoint["verdict"]["target_preflight_ready"] is True
        assert camera_endpoint["verdict"]["camera_closure_possible"] is True
        assert camera_endpoint["blockers"] == []
        assert camera_endpoint["inputs"]["raw_source_kind"] == "sensor_dma_capture"
        assert any(
            check["name"] == "camera raw source endpoint is device-like" and check["passed"] is True
            for check in camera_endpoint["checks"]
        )

    with tempfile.TemporaryDirectory(prefix="mission1_target_preflight_", dir=work_parent) as td:
        missing_camera_endpoint = tool.build_report(
            args(
                tool,
                Path(td) / "missing_camera_endpoint",
                camera_flags=True,
                raw_source_kind="sensor_dma_capture",
                camera_endpoint_exists=False,
            )
        )
        assert missing_camera_endpoint["verdict"]["target_preflight_ready"] is False
        assert "camera raw source endpoint is missing on target" in missing_camera_endpoint["blockers"]

    with tempfile.TemporaryDirectory(prefix="mission1_target_preflight_", dir=work_parent) as td:
        missing_labels = tool.build_report(
            args(tool, Path(td) / "missing_labels", camera_flags=True, camera_labels=False)
        )
        assert missing_labels["verdict"]["target_preflight_ready"] is False
        assert missing_labels["verdict"]["camera_closure_possible"] is False
        assert "camera frame source ready label is missing" in missing_labels["blockers"]

    with tempfile.TemporaryDirectory(prefix="mission1_target_preflight_", dir=work_parent) as td:
        standin_labels = tool.build_report(
            args(tool, Path(td) / "standin_labels", camera_flags=True, standin_label=True)
        )
        assert standin_labels["verdict"]["target_preflight_ready"] is False
        assert standin_labels["verdict"]["camera_closure_possible"] is False
        assert any("stand-in token" in blocker for blocker in standin_labels["blockers"])

    with tempfile.TemporaryDirectory(prefix="mission1_target_preflight_", dir=work_parent) as td:
        closure_layout = tool.build_report(
            args(tool, Path(td) / "closure_layout", camera_flags=True, layout="build-closure")
        )
        assert closure_layout["verdict"]["target_preflight_ready"] is True
        executables = closure_layout["target_probe"]["executables"]
        assert has_executable_candidate(
            executables["bench_fused"], "build-closure/source/app/bench_fused/bench_fused"
        )
        assert has_executable_candidate(
            executables["labs_encoder_bench_cli"], "build-closure/bin/labs_encoder_bench_cli"
        )
        assert has_executable_candidate(
            executables["fused_decode_cli"], "build-closure/bin/fused_decode_cli"
        )
        assert has_executable_candidate(
            executables["gvid_preview_rgb_cli"], "build-closure/bin/gvid_preview_rgb_cli"
        )
    print("test_mission1_camera_target_preflight: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
