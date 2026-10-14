#!/usr/bin/env python3
"""Validate a Labs camera-handoff receipt."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr_labs_camera_handoff_receipt.v1"
GVID_PIXEL_FORMAT_MAX = 5


def as_bool(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> bool | None:
    value = obj.get(key)
    if not isinstance(value, bool):
        failures.append(f"{prefix}.{key} must be boolean")
        return None
    return value


def as_number(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> float | None:
    value = obj.get(key)
    if isinstance(value, bool):
        failures.append(f"{prefix}.{key} must be numeric")
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        failures.append(f"{prefix}.{key} must be numeric")
        return None


def as_int(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> int | None:
    value = obj.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        failures.append(f"{prefix}.{key} must be integer")
        return None
    return value


def require_obj(root: dict[str, Any], key: str, failures: list[str]) -> dict[str, Any]:
    value = root.get(key)
    if not isinstance(value, dict):
        failures.append(f"{key} must be an object")
        return {}
    return value


def require_string(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> str | None:
    value = obj.get(key)
    if not isinstance(value, str) or not value:
        failures.append(f"{prefix}.{key} must be a non-empty string")
        return None
    return value


def validate_source_provenance(data: dict[str, Any], failures: list[str]) -> bool:
    value = data.get("source_provenance")
    if value is None:
        return False
    if not isinstance(value, dict):
        failures.append("source_provenance must be an object")
        return False
    available = as_bool(value, "available", failures, "source_provenance")
    require_string(value, "policy", failures, "source_provenance")
    if available is not True:
        return False
    sha = require_string(value, "sha256", failures, "source_provenance")
    if sha and (len(sha) != 64 or any(ch not in "0123456789abcdefABCDEF" for ch in sha)):
        failures.append("source_provenance.sha256 must be a 64-character hex digest")
    file_count = as_int(value, "file_count", failures, "source_provenance")
    if file_count is not None and file_count <= 0:
        failures.append("source_provenance.file_count must be positive")
    total_bytes = as_int(value, "total_bytes", failures, "source_provenance")
    if total_bytes is not None and total_bytes <= 0:
        failures.append("source_provenance.total_bytes must be positive")
    git = value.get("git")
    if git is not None and not isinstance(git, dict):
        failures.append("source_provenance.git must be an object when present")
    return bool(sha and file_count and file_count > 0 and total_bytes and total_bytes > 0)


def validate_receipt(data: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    if data.get("schema") != SCHEMA:
        failures.append(f"schema must be {SCHEMA}")
    source_provenance_available = validate_source_provenance(data, failures)

    target = require_obj(data, "target", failures)
    require_string(target, "name", failures, "target")
    role = require_string(target, "role", failures, "target")
    if role and role not in {"camera", "stand-in"}:
        failures.append("target.role must be camera or stand-in")

    integration = require_obj(data, "integration", failures)
    raw_source_kind_value = integration.get("raw_source_kind")
    raw_source_kind = raw_source_kind_value if isinstance(raw_source_kind_value, str) else None
    if raw_source_kind_value is not None and not raw_source_kind:
        failures.append("integration.raw_source_kind must be a non-empty string")
    if raw_source_kind and raw_source_kind not in {"file_standin", "sensor_dma_capture", "camera_ring_buffer"}:
        failures.append("integration.raw_source_kind must be file_standin, sensor_dma_capture, or camera_ring_buffer")
    sensor_dma = require_obj(integration, "sensor_dma_handoff", failures)
    sensor_dma_executed = as_bool(sensor_dma, "executed", failures, "integration.sensor_dma_handoff")
    storage_handoff_value = integration.get("storage_handoff")
    storage_handoff: dict[str, Any] = {}
    storage_handoff_executed: bool | None = None
    if storage_handoff_value is not None:
        if not isinstance(storage_handoff_value, dict):
            failures.append("integration.storage_handoff must be an object")
        else:
            storage_handoff = storage_handoff_value
            storage_handoff_executed = as_bool(storage_handoff, "executed", failures, "integration.storage_handoff")
            require_string(storage_handoff, "medium", failures, "integration.storage_handoff")
            require_string(storage_handoff, "ownership", failures, "integration.storage_handoff")
    require_string(integration, "frame_source", failures, "integration")
    require_string(integration, "memory_ownership", failures, "integration")
    write_path = require_string(integration, "write_path", failures, "integration")

    input_frame = require_obj(data, "input_frame", failures)
    width = as_int(input_frame, "width", failures, "input_frame")
    height = as_int(input_frame, "height", failures, "input_frame")
    stride = as_int(input_frame, "stride_bytes", failures, "input_frame")
    bit_depth = as_int(input_frame, "bit_depth", failures, "input_frame")
    pixel_format = as_int(input_frame, "pixel_format", failures, "input_frame")
    target_fps = as_number(input_frame, "target_fps", failures, "input_frame")
    if width is not None and width <= 0:
        failures.append("input_frame.width must be positive")
    if height is not None and height <= 0:
        failures.append("input_frame.height must be positive")
    if stride is not None and stride <= 0:
        failures.append("input_frame.stride_bytes must be positive")
    if bit_depth is not None and bit_depth <= 0:
        failures.append("input_frame.bit_depth must be positive")
    if pixel_format is not None and not (0 <= pixel_format <= GVID_PIXEL_FORMAT_MAX):
        failures.append(f"input_frame.pixel_format must be in 0..{GVID_PIXEL_FORMAT_MAX}")
    if target_fps is not None and target_fps <= 0:
        failures.append("input_frame.target_fps must be positive")

    capture = require_obj(data, "capture", failures)
    frames_requested = as_int(capture, "frames_requested", failures, "capture")
    frames_written = as_int(capture, "frames_written", failures, "capture")
    dropped = as_int(capture, "dropped_frames", failures, "capture")
    if frames_requested is not None and frames_requested <= 0:
        failures.append("capture.frames_requested must be positive")
    if frames_written is not None and frames_written <= 0:
        failures.append("capture.frames_written must be positive")
    if dropped is not None and dropped < 0:
        failures.append("capture.dropped_frames must be non-negative")

    timing = require_obj(data, "timing", failures)
    fps_median = as_number(timing, "fps_median", failures, "timing")
    actual_wall_fps = None
    if "actual_wall_fps" in timing:
        actual_wall_fps = as_number(timing, "actual_wall_fps", failures, "timing")
    if "actual_wall_s" in timing:
        as_number(timing, "actual_wall_s", failures, "timing")
    as_number(timing, "median_ms", failures, "timing")
    as_number(timing, "p95_ms", failures, "timing")
    as_number(timing, "p99_ms", failures, "timing")

    storage = require_obj(data, "storage", failures)
    as_number(storage, "write_mb_s", failures, "storage")
    require_string(storage, "flush_policy", failures, "storage")

    memory = require_obj(data, "memory", failures)
    if memory.get("heap_high_water_bytes") is None and memory.get("rss_kb") is None:
        failures.append("memory must include heap_high_water_bytes or rss_kb")
    if memory.get("heap_high_water_bytes") is not None:
        as_number(memory, "heap_high_water_bytes", failures, "memory")
    if memory.get("rss_kb") is not None:
        as_number(memory, "rss_kb", failures, "memory")

    output = require_obj(data, "output", failures)
    validation = require_obj(output, "validation", failures)
    gvid_valid = as_bool(validation, "valid", failures, "output.validation")
    validation_frames = as_int(validation, "frame_count", failures, "output.validation")
    require_string(output, "sha256", failures, "output")

    recovery = require_obj(data, "interruption_recovery", failures)
    recovery_proven = as_bool(recovery, "proven", failures, "interruption_recovery")
    validator_rejects = as_bool(recovery, "validator_rejects_truncated", failures, "interruption_recovery")

    verdict = require_obj(data, "verdict", failures)
    firmware_ready = as_bool(verdict, "firmware_ready", failures, "verdict")
    fps_target_met = as_bool(verdict, "fps_target_met", failures, "verdict")
    fps_median_target_met = None
    if "fps_median_target_met" in verdict:
        fps_median_target_met = as_bool(verdict, "fps_median_target_met", failures, "verdict")
    fps_wall_target_met = None
    if "fps_wall_target_met" in verdict:
        fps_wall_target_met = as_bool(verdict, "fps_wall_target_met", failures, "verdict")
    no_drops = as_bool(verdict, "no_drops", failures, "verdict")
    target_evidence = as_bool(verdict, "target_evidence", failures, "verdict")

    if frames_written is not None and validation_frames is not None and frames_written != validation_frames:
        failures.append("capture.frames_written must match output.validation.frame_count")
    if dropped is not None and no_drops is not None and no_drops != (dropped == 0):
        failures.append("verdict.no_drops must match capture.dropped_frames")
    if fps_median is not None and target_fps is not None and fps_target_met is not None:
        median_ok = fps_median >= target_fps
        wall_ok = True if actual_wall_fps is None else actual_wall_fps >= target_fps
        if fps_median_target_met is not None and fps_median_target_met != median_ok:
            failures.append("verdict.fps_median_target_met must match timing.fps_median >= input_frame.target_fps")
        if fps_wall_target_met is not None and fps_wall_target_met != wall_ok:
            failures.append("verdict.fps_wall_target_met must match timing.actual_wall_fps >= input_frame.target_fps")
        if fps_target_met != (median_ok and wall_ok):
            failures.append("verdict.fps_target_met must match median and wall fps target checks")

    if firmware_ready:
        if role != "camera":
            failures.append("firmware-ready receipt must use target.role=camera")
        if raw_source_kind in {None, "file_standin"}:
            failures.append("firmware-ready receipt must use integration.raw_source_kind=sensor_dma_capture or camera_ring_buffer")
        if sensor_dma_executed is not True:
            failures.append("firmware-ready receipt must execute sensor/DMA handoff")
        if storage_handoff_executed is not True:
            failures.append("firmware-ready receipt must execute storage handoff")
        if write_path and any(token in write_path.lower() for token in ("bench_fused", "file-backed", "stand-in")):
            failures.append("firmware-ready receipt write_path must be camera/firmware storage, not a stand-in path")
        if target_evidence is not True:
            failures.append("firmware-ready receipt must mark target_evidence true")
        if source_provenance_available is not True:
            failures.append("firmware-ready receipt must include available source_provenance")
        for label, value in (
            ("verdict.fps_target_met", fps_target_met),
            ("verdict.no_drops", no_drops),
            ("output.validation.valid", gvid_valid),
            ("interruption_recovery.proven", recovery_proven),
            ("interruption_recovery.validator_rejects_truncated", validator_rejects),
        ):
            if value is not True:
                failures.append(f"firmware-ready receipt requires {label}=true")
    elif role == "camera":
        blocker = data.get("blocker")
        if not isinstance(blocker, dict) or not blocker.get("cause"):
            failures.append("blocked camera receipt must include blocker.cause")

    if role == "camera" and sensor_dma_executed is not True:
        failures.append("camera receipt must set integration.sensor_dma_handoff.executed=true")
    if role == "camera" and raw_source_kind in {None, "file_standin"}:
        failures.append("camera receipt must set integration.raw_source_kind to sensor_dma_capture or camera_ring_buffer")
    if role == "camera" and storage_handoff_value is None:
        failures.append("camera receipt must include integration.storage_handoff")

    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("receipt", type=Path, help="camera handoff receipt JSON")
    args = ap.parse_args()

    data = json.loads(args.receipt.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        print("receipt must be a JSON object", file=sys.stderr)
        return 1
    failures = validate_receipt(data)
    if failures:
        print("Labs camera handoff receipt failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    print("Labs camera handoff receipt OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
