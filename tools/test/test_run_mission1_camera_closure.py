#!/usr/bin/env python3
"""Smoke-test the Mission 1 camera closure runner with existing receipts."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/run_mission1_camera_closure.py"


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def create_fixture(root: Path) -> tuple[Path, Path, Path]:
    target = root / "labs_target_bench.json"
    preflight = root / "target_preflight_receipt.json"
    preview = root / "preview_decode_1024x768/receipt.json"
    write_json(
        target,
        {
            "schema": "gpr_labs_target_bench.v1",
            "target": {"name": "Pi 5 stand-in", "fps": 20.0, "actual_wall_fps": 21.0, "actual_wall_s": 2.0},
            "source_provenance": {
                "available": True,
                "policy": "source_tree_digest_v1",
                "sha256": "1" * 64,
                "file_count": 3,
                "total_bytes": 1234,
            },
            "capture": {
                "source_width": 4096,
                "source_height": 3072,
                "capture_width": 4096,
                "capture_height": 3072,
                "pixel_format": 1,
                "frames_requested": 4,
                "frames_written": 4,
                "dropped_frames": 0,
            },
            "timing": {"n": 4, "fps_median": 22.0, "median_ms": 45.0, "p95_ms": 50.0, "p99_ms": 51.0},
            "storage": {"write_MBps_wall": 100.0, "fsync_policy": "fixture"},
            "memory": {"bench_child_maxrss_kb": 1000},
            "gvid": {
                "path": str(root / "capture.gvid"),
                "sha256": "2" * 64,
                "validation": {"valid": True, "width": 4096, "height": 3072, "frame_count": 4},
            },
            "interruption_recovery": {
                "validator_rejects_truncated": True,
                "complete_frames_recovered": 3,
            },
            "verdict": {
                "target_evidence": True,
                "fps_target_met": True,
                "fps_median_target_met": True,
                "fps_wall_target_met": True,
                "gvid_valid": True,
                "no_drops": True,
                "interruption_recovery_proven": True,
            },
        },
    )
    write_json(
        preflight,
        {
            "schema": "gpr.mission1_camera_target_preflight.v1",
            "target": {"name": "Pi 5 stand-in", "role": "stand-in"},
            "verdict": {"target_preflight_ready": True, "camera_closure_possible": False},
            "blockers": [],
        },
    )
    write_json(
        preview,
        {
            "schema": "gvid_decode_target_bench.v1",
            "gvid_sha256": "2" * 64,
            "frame_count": 4,
            "summary": {
                "dims": [[1024, 768]],
                "actual_wall_fps_including_extract_process": 21.0,
                "decode_plus_target": {"fps_median": 40.0, "median_ms": 25.0, "p95_ms": 28.0, "p99_ms": 30.0},
                "process_wall": {"median_ms": 28.0},
            },
            "memory": {"children_maxrss_kb": 2000},
        },
    )
    return target, preflight, preview


def main() -> int:
    work_parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
    work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mission1_camera_closure_run_", dir=work_parent) as td:
        fixture = Path(td)
        target, preflight, preview = create_fixture(fixture)
        out = fixture / "out"
        result = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--output-dir",
                str(out),
                "--target-bench-receipt",
                str(target),
                "--target-preflight-receipt",
                str(preflight),
                "--preview-receipt",
                str(preview),
                "--target-name",
                "Pi 5 stand-in",
                "--target-role",
                "stand-in",
                "--target-fps",
                "20",
                "--pixel-format",
                "1",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            return result.returncode
        summary = json.loads((out / "mission1_camera_closure_run.json").read_text(encoding="utf-8"))
        assert summary["schema"] == "gpr.mission1_camera_closure_run.v1"
        assert summary["verdict"]["production_ready"] is False
        assert summary["verdict"]["target_preflight_ready"] is True
        assert summary["verdict"]["camera_closure_possible"] is False
        assert summary["verdict"]["firmware_ready"] is False
        assert summary["verdict"]["ui_ready"] is False
        assert (out / "target_preflight_receipt.json").exists()
        assert (out / "camera_handoff_receipt.json").exists()
        assert (out / "preview_ui_receipt.json").exists()
        copied_out = fixture / "copied_out"
        copied = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--output-dir",
                str(copied_out),
                "--target-bench-receipt",
                str(target),
                "--target-preflight-receipt",
                str(preflight),
                "--camera-handoff-receipt",
                str(out / "camera_handoff_receipt.json"),
                "--preview-receipt",
                str(preview),
                "--preview-ui-receipt",
                str(out / "preview_ui_receipt.json"),
                "--target-name",
                "Pi 5 stand-in",
                "--target-role",
                "stand-in",
                "--target-fps",
                "20",
                "--pixel-format",
                "1",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if copied.returncode != 0:
            print(copied.stdout)
            print(copied.stderr, file=sys.stderr)
            return copied.returncode
        copied_summary = json.loads((copied_out / "mission1_camera_closure_run.json").read_text(encoding="utf-8"))
        step_names = [step["name"] for step in copied_summary["steps"]]
        assert "copy_target_preflight_receipt" in step_names
        assert "copy_camera_handoff_receipt" in step_names
        assert "copy_preview_ui_receipt" in step_names
        assert copied_summary["verdict"]["production_ready"] is False
        simulated_camera = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--output-dir",
                str(fixture / "simulated_camera_out"),
                "--simulate-target-bench",
                "--target-role",
                "camera",
                "--target-name",
                "Mission 1",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert simulated_camera.returncode != 0
        assert "cannot use --simulate-target-bench" in simulated_camera.stderr
        camera_without_preflight = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--output-dir",
                str(fixture / "camera_without_preflight_out"),
                "--target-bench-receipt",
                str(target),
                "--preview-receipt",
                str(preview),
                "--target-role",
                "camera",
                "--target-name",
                "Mission 1",
                "--raw-source-kind",
                "sensor_dma_capture",
                "--raw",
                "/dev/mission1/sensor_dma_ring",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert camera_without_preflight.returncode != 0
        assert "requires --target-preflight-receipt" in camera_without_preflight.stderr
        mismatched_camera = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--output-dir",
                str(fixture / "mismatched_camera_out"),
                "--target-bench-receipt",
                str(target),
                "--target-preflight-receipt",
                str(preflight),
                "--camera-handoff-receipt",
                str(out / "camera_handoff_receipt.json"),
                "--preview-receipt",
                str(preview),
                "--preview-ui-receipt",
                str(out / "preview_ui_receipt.json"),
                "--target-name",
                "Mission 1",
                "--target-role",
                "camera",
                "--target-fps",
                "20",
                "--pixel-format",
                "1",
                "--raw-source-kind",
                "sensor_dma_capture",
                "--raw",
                "/dev/mission1/sensor_dma_ring",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert mismatched_camera.returncode != 0
        assert "target preflight target.role='camera'" in mismatched_camera.stderr

        camera_handoff = json.loads((out / "camera_handoff_receipt.json").read_text(encoding="utf-8"))
        camera_handoff["target"] = {"name": "Mission 1", "role": "camera"}
        camera_handoff["integration"]["raw_source_kind"] = "sensor_dma_capture"
        camera_handoff["integration"]["frame_source"] = "Mission 1 sensor DMA"
        camera_handoff["integration"]["write_path"] = "Mission 1 camera storage writer"
        camera_handoff["integration"]["sensor_dma_handoff"]["executed"] = True
        camera_handoff["integration"]["storage_handoff"]["executed"] = True
        camera_handoff["verdict"]["firmware_ready"] = True
        camera_handoff.pop("blocker", None)
        camera_handoff_path = fixture / "camera_handoff_ready.json"
        write_json(camera_handoff_path, camera_handoff)

        camera_preview_ui = json.loads((out / "preview_ui_receipt.json").read_text(encoding="utf-8"))
        camera_preview_ui["target"] = {"name": "Mission 1", "role": "camera"}
        camera_preview_ui["integration"]["ui_path_executed"] = True
        camera_preview_ui["validation"]["visual_checked"] = True
        camera_preview_ui["verdict"]["ui_ready"] = True
        camera_preview_ui.pop("blocker", None)
        camera_preview_ui_path = fixture / "camera_preview_ui_ready.json"
        write_json(camera_preview_ui_path, camera_preview_ui)

        blocked_camera_preflight = json.loads(preflight.read_text(encoding="utf-8"))
        blocked_camera_preflight["target"] = {"name": "Mission 1", "role": "camera"}
        blocked_camera_preflight["inputs"] = {
            "raw": "/dev/mission1/sensor_dma_ring",
            "raw_source_kind": "sensor_dma_capture",
        }
        blocked_camera_preflight["verdict"] = {
            "target_preflight_ready": False,
            "camera_closure_possible": False,
        }
        blocked_camera_preflight["blockers"] = [
            "camera frame source ready",
            "camera storage path ready",
            "camera display path ready",
        ]
        blocked_camera_preflight_path = fixture / "blocked_camera_preflight.json"
        write_json(blocked_camera_preflight_path, blocked_camera_preflight)

        camera_ready_receipts_blocked_preflight = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--output-dir",
                str(fixture / "camera_ready_receipts_blocked_preflight"),
                "--target-bench-receipt",
                str(target),
                "--target-preflight-receipt",
                str(blocked_camera_preflight_path),
                "--camera-handoff-receipt",
                str(camera_handoff_path),
                "--preview-receipt",
                str(preview),
                "--preview-ui-receipt",
                str(camera_preview_ui_path),
                "--target-name",
                "Mission 1",
                "--target-role",
                "camera",
                "--target-fps",
                "20",
                "--pixel-format",
                "1",
                "--raw-source-kind",
                "sensor_dma_capture",
                "--raw",
                "/dev/mission1/sensor_dma_ring",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert camera_ready_receipts_blocked_preflight.returncode != 0
        assert "requires target_preflight_ready=true" in camera_ready_receipts_blocked_preflight.stderr

        ready_camera_preflight = json.loads(blocked_camera_preflight_path.read_text(encoding="utf-8"))
        ready_camera_preflight["verdict"] = {
            "target_preflight_ready": True,
            "camera_closure_possible": True,
            "remaining_blocker_count": 0,
        }
        ready_camera_preflight["blockers"] = []
        ready_camera_preflight_path = fixture / "ready_camera_preflight.json"
        write_json(ready_camera_preflight_path, ready_camera_preflight)

        camera_ready_receipts = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--output-dir",
                str(fixture / "camera_ready_receipts"),
                "--target-bench-receipt",
                str(target),
                "--target-preflight-receipt",
                str(ready_camera_preflight_path),
                "--camera-handoff-receipt",
                str(camera_handoff_path),
                "--preview-receipt",
                str(preview),
                "--preview-ui-receipt",
                str(camera_preview_ui_path),
                "--target-name",
                "Mission 1",
                "--target-role",
                "camera",
                "--target-fps",
                "20",
                "--pixel-format",
                "1",
                "--raw-source-kind",
                "sensor_dma_capture",
                "--raw",
                "/dev/mission1/sensor_dma_ring",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert camera_ready_receipts.returncode == 0, camera_ready_receipts.stderr
        ready_summary = json.loads(
            (
                fixture
                / "camera_ready_receipts"
                / "mission1_camera_closure_run.json"
            ).read_text(encoding="utf-8")
        )
        assert ready_summary["verdict"]["firmware_ready"] is True
        assert ready_summary["verdict"]["ui_ready"] is True
        assert ready_summary["verdict"]["target_preflight_ready"] is True
        assert ready_summary["verdict"]["camera_closure_possible"] is True
        assert ready_summary["verdict"]["aggregate_consistency_ready"] is True
        assert ready_summary["verdict"]["aggregate_consistency_failures"] == []
        assert ready_summary["verdict"]["production_ready"] is True
    print("test_run_mission1_camera_closure: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
