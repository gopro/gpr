#!/usr/bin/env python3
"""Regression test for the Mission 1 camera closure-run validator."""
from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools/check_mission1_camera_closure_run.py"


def load_checker():
    spec = importlib.util.spec_from_file_location("check_mission1_camera_closure_run", CHECKER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def camera_handoff() -> dict:
    return {
        "schema": "gpr_labs_camera_handoff_receipt.v1",
        "source_provenance": {
            "available": True,
            "policy": "source_tree_digest_v1",
            "sha256": "1" * 64,
            "file_count": 3,
            "total_bytes": 1234,
        },
        "target": {"name": "Pi 5 stand-in", "role": "stand-in"},
        "integration": {
            "frame_source": "file-backed Bayer stand-in",
            "memory_ownership": "synchronous submit; caller owns input through return",
            "write_path": "bench_fused target-bench .gvid path",
            "sensor_dma_handoff": {"executed": False},
            "storage_handoff": {
                "executed": False,
                "medium": "target-bench filesystem stand-in",
                "ownership": "OS/page-cache writeback; not camera firmware DMA",
            },
        },
        "input_frame": {
            "width": 4096,
            "height": 3072,
            "stride_bytes": 8192,
            "bit_depth": 14,
            "pixel_format": 1,
            "target_fps": 20.0,
        },
        "capture": {"frames_requested": 4, "frames_written": 4, "dropped_frames": 0},
        "timing": {
            "fps_median": 22.0,
            "median_ms": 45.0,
            "p95_ms": 50.0,
            "p99_ms": 51.0,
        },
        "storage": {"write_mb_s": 100.0, "flush_policy": "fixture"},
        "memory": {"rss_kb": 1000},
        "output": {"sha256": "2" * 64, "validation": {"valid": True, "frame_count": 4}},
        "interruption_recovery": {
            "proven": True,
            "validator_rejects_truncated": True,
            "complete_frames_recovered": 3,
        },
        "verdict": {
            "firmware_ready": False,
            "target_evidence": True,
            "fps_target_met": True,
            "fps_median_target_met": True,
            "fps_wall_target_met": True,
            "no_drops": True,
        },
        "blocker": {"cause": "camera sensor/DMA and camera storage handoff not executed"},
    }


def preview_ui() -> dict:
    return {
        "schema": "gpr_labs_preview_ui_receipt.v1",
        "source_provenance": {
            "available": True,
            "policy": "source_tree_digest_v1",
            "sha256": "1" * 64,
            "file_count": 3,
            "total_bytes": 1234,
        },
        "target": {"name": "Pi 5 stand-in", "role": "stand-in"},
        "source": {
            "width": 4096,
            "height": 3072,
            "frame_count": 4,
            "bit_depth": 14,
            "pixel_format": 1,
            "gvid_sha256": "2" * 64,
        },
        "preview": {
            "width": 1024,
            "height": 768,
            "frame_count": 4,
            "target_fps": 20.0,
            "full_frame_downsample": True,
            "color_pipeline": "full-frame Bayer decode to RGB preview",
            "tone_pipeline": "preview tone path from fused decoder target",
        },
        "integration": {
            "ui_path_executed": False,
            "decode_path": "fused_decode_cli mission1_preview_4x_1024x768",
            "presentation_path": "off-camera preview decode receipt",
            "buffer_ownership": "process-owned RGB output buffer",
            "display_surface": "stand-in raw preview receipt output",
        },
        "timing": {
            "fps_median": 40.0,
            "actual_wall_fps": 21.0,
            "median_ms": 25.0,
            "p95_ms": 28.0,
            "p99_ms": 30.0,
        },
        "memory": {"rss_kb": 2000},
        "validation": {"output_valid": True, "no_drops": True, "visual_checked": False},
        "verdict": {"ui_ready": False, "target_evidence": True, "fps_target_met": True},
        "blocker": {"cause": "Mission 1 camera UI/display path not executed"},
    }


def target_bench() -> dict:
    return {
        "schema": "gpr_labs_target_bench.v1",
        "target": {"name": "Pi 5 stand-in", "fps": 20.0, "actual_wall_fps": 21.0},
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
            "frames_written": 4,
            "dropped_frames": 0,
        },
        "gvid": {
            "sha256": "2" * 64,
            "validation": {"valid": True, "width": 4096, "height": 3072, "frame_count": 4},
        },
        "verdict": {"target_evidence": True},
    }


def target_preflight() -> dict:
    return {
        "schema": "gpr.mission1_camera_target_preflight.v1",
        "target": {"name": "Pi 5 stand-in", "role": "stand-in"},
        "verdict": {"target_preflight_ready": True, "camera_closure_possible": False},
        "blockers": [],
    }


def camera_ready_handoff() -> dict:
    receipt = camera_handoff()
    receipt["target"] = {"name": "Mission 1", "role": "camera"}
    receipt["integration"]["raw_source_kind"] = "sensor_dma_capture"
    receipt["integration"]["frame_source"] = "Mission 1 sensor DMA"
    receipt["integration"]["write_path"] = "Mission 1 camera storage writer"
    receipt["integration"]["sensor_dma_handoff"]["executed"] = True
    receipt["integration"]["storage_handoff"]["executed"] = True
    receipt["integration"]["storage_handoff"]["medium"] = "Mission 1 SD path"
    receipt["integration"]["storage_handoff"]["ownership"] = (
        "camera firmware owns write buffer through storage completion"
    )
    receipt["verdict"]["firmware_ready"] = True
    receipt.pop("blocker", None)
    return receipt


def camera_ready_preview_ui() -> dict:
    receipt = preview_ui()
    receipt["target"] = {"name": "Mission 1", "role": "camera"}
    receipt["integration"]["ui_path_executed"] = True
    receipt["integration"]["presentation_path"] = "Mission 1 rear display presentation path"
    receipt["integration"]["display_surface"] = "Mission 1 rear display"
    receipt["integration"]["buffer_ownership"] = "camera display process RGB output buffer"
    receipt["validation"]["visual_checked"] = True
    receipt["verdict"]["ui_ready"] = True
    receipt.pop("blocker", None)
    return receipt


def blocked_camera_preflight() -> dict:
    return {
        "schema": "gpr.mission1_camera_target_preflight.v1",
        "target": {"name": "Mission 1", "role": "camera"},
        "verdict": {"target_preflight_ready": False, "camera_closure_possible": False},
        "blockers": [
            "camera frame source ready",
            "camera storage path ready",
            "camera display path ready",
        ],
    }


def closure_run(root: Path) -> dict:
    return {
        "schema": "gpr.mission1_camera_closure_run.v1",
        "receipts": {
            "target_bench": str(root / "labs_target_bench.json"),
            "target_preflight": str(root / "target_preflight_receipt.json"),
            "camera_handoff": str(root / "camera_handoff_receipt.json"),
            "preview_decode": str(root / "preview_decode_1024x768/receipt.json"),
            "preview_ui": str(root / "preview_ui_receipt.json"),
        },
        "steps": [
            {"name": "validate_camera_handoff_receipt", "returncode": 0},
            {"name": "validate_preview_ui_receipt", "returncode": 0},
        ],
        "verdict": {
            "production_ready": False,
            "target_preflight_ready": True,
            "camera_closure_possible": False,
            "firmware_ready": False,
            "ui_ready": False,
            "handoff_blocker": "camera sensor/DMA and camera storage handoff not executed",
            "preview_blocker": "Mission 1 camera UI/display path not executed",
        },
    }


def main() -> int:
    checker = load_checker()
    work_parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
    work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mission1_camera_closure_run_", dir=work_parent) as td:
        root = Path(td)
        write_json(root / "labs_target_bench.json", target_bench())
        write_json(root / "target_preflight_receipt.json", target_preflight())
        write_json(root / "camera_handoff_receipt.json", camera_handoff())
        write_json(root / "preview_ui_receipt.json", preview_ui())
        data = closure_run(root)
        assert checker.validate(data, base=root) == []

        portable = json.loads(json.dumps(data))
        portable["receipts"]["camera_handoff"] = "/mnt/ssd/gpr_work/artifacts/remote/camera_handoff_receipt.json"
        portable["receipts"]["preview_ui"] = "/mnt/ssd/gpr_work/artifacts/remote/preview_ui_receipt.json"
        portable["receipts"]["target_preflight"] = "/mnt/ssd/gpr_work/artifacts/remote/target_preflight_receipt.json"
        assert checker.validate(portable, base=root) == []

        bad = json.loads(json.dumps(data))
        bad["verdict"]["production_ready"] = True
        failures = checker.validate(bad, base=root)
        assert any("production_ready" in failure for failure in failures), failures

        bad_preflight = json.loads(json.dumps(data))
        bad_preflight["verdict"]["camera_closure_possible"] = True
        failures = checker.validate(bad_preflight, base=root)
        assert any("camera_closure_possible" in failure for failure in failures), failures

        bad_step = json.loads(json.dumps(data))
        bad_step["steps"][0]["returncode"] = 1
        failures = checker.validate(bad_step, base=root)
        assert any("validate_camera_handoff_receipt" in failure for failure in failures), failures

        mismatched = json.loads(json.dumps(data))
        target_mismatch = target_bench()
        target_mismatch["capture"]["frames_written"] = 3
        write_json(root / "labs_target_bench.json", target_mismatch)
        failures = checker.validate(mismatched, base=root)
        assert any("frames written mismatch" in failure for failure in failures), failures

        write_json(root / "labs_target_bench.json", target_bench())
        write_json(root / "target_preflight_receipt.json", blocked_camera_preflight())
        write_json(root / "camera_handoff_receipt.json", camera_ready_handoff())
        write_json(root / "preview_ui_receipt.json", camera_ready_preview_ui())
        blocked_preflight_ready_receipts = closure_run(root)
        blocked_preflight_ready_receipts["verdict"] = {
            "production_ready": False,
            "target_preflight_ready": False,
            "camera_closure_possible": False,
            "firmware_ready": True,
            "ui_ready": True,
            "handoff_blocker": None,
            "preview_blocker": None,
        }
        assert checker.validate(blocked_preflight_ready_receipts, base=root) == []

        false_production_claim = json.loads(json.dumps(blocked_preflight_ready_receipts))
        false_production_claim["verdict"]["production_ready"] = True
        failures = checker.validate(false_production_claim, base=root)
        assert any("target_preflight_ready" in failure for failure in failures), failures
    print("test_mission1_camera_closure_run: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
