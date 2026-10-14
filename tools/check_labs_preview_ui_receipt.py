#!/usr/bin/env python3
"""Validate a Labs camera-back preview UI receipt."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr_labs_preview_ui_receipt.v1"
GVID_PIXEL_FORMAT_MAX = 5


def as_bool(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> bool | None:
    value = obj.get(key)
    if not isinstance(value, bool):
        failures.append(f"{prefix}.{key} must be boolean")
        return None
    return value


def as_int(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> int | None:
    value = obj.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        failures.append(f"{prefix}.{key} must be integer")
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


def require_sha256(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> str | None:
    value = require_string(obj, key, failures, prefix)
    if value and (len(value) != 64 or any(ch not in "0123456789abcdefABCDEF" for ch in value)):
        failures.append(f"{prefix}.{key} must be a 64-character hex digest")
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
    sha = require_sha256(value, "sha256", failures, "source_provenance") if available else None
    file_count = as_int(value, "file_count", failures, "source_provenance") if available else None
    total_bytes = as_int(value, "total_bytes", failures, "source_provenance") if available else None
    if file_count is not None and file_count <= 0:
        failures.append("source_provenance.file_count must be positive")
    if total_bytes is not None and total_bytes <= 0:
        failures.append("source_provenance.total_bytes must be positive")
    return bool(available is True and sha and file_count and file_count > 0 and total_bytes and total_bytes > 0)


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

    source = require_obj(data, "source", failures)
    source_width = as_int(source, "width", failures, "source")
    source_height = as_int(source, "height", failures, "source")
    source_frames = as_int(source, "frame_count", failures, "source")
    source_bit_depth = as_int(source, "bit_depth", failures, "source")
    source_pixel_format = as_int(source, "pixel_format", failures, "source")
    require_sha256(source, "gvid_sha256", failures, "source")
    if source_width is not None and source_width <= 0:
        failures.append("source.width must be positive")
    if source_height is not None and source_height <= 0:
        failures.append("source.height must be positive")
    if source_frames is not None and source_frames <= 0:
        failures.append("source.frame_count must be positive")
    if source_bit_depth is not None and source_bit_depth <= 0:
        failures.append("source.bit_depth must be positive")
    if source_pixel_format is not None and not (0 <= source_pixel_format <= GVID_PIXEL_FORMAT_MAX):
        failures.append(f"source.pixel_format must be in 0..{GVID_PIXEL_FORMAT_MAX}")

    preview = require_obj(data, "preview", failures)
    preview_width = as_int(preview, "width", failures, "preview")
    preview_height = as_int(preview, "height", failures, "preview")
    preview_frames = as_int(preview, "frame_count", failures, "preview")
    target_fps = as_number(preview, "target_fps", failures, "preview")
    full_frame_downsample = as_bool(preview, "full_frame_downsample", failures, "preview")
    require_string(preview, "color_pipeline", failures, "preview")
    require_string(preview, "tone_pipeline", failures, "preview")
    if preview_width is not None and preview_width <= 0:
        failures.append("preview.width must be positive")
    if preview_height is not None and preview_height <= 0:
        failures.append("preview.height must be positive")
    if preview_frames is not None and preview_frames <= 0:
        failures.append("preview.frame_count must be positive")
    if target_fps is not None and target_fps <= 0:
        failures.append("preview.target_fps must be positive")
    if source_frames is not None and preview_frames is not None and source_frames != preview_frames:
        failures.append("source.frame_count must match preview.frame_count")

    integration = require_obj(data, "integration", failures)
    ui_path_executed = as_bool(integration, "ui_path_executed", failures, "integration")
    require_string(integration, "decode_path", failures, "integration")
    require_string(integration, "presentation_path", failures, "integration")
    require_string(integration, "buffer_ownership", failures, "integration")
    require_string(integration, "display_surface", failures, "integration")

    timing = require_obj(data, "timing", failures)
    fps_median = as_number(timing, "fps_median", failures, "timing")
    actual_wall_fps = None
    if "actual_wall_fps" in timing:
        actual_wall_fps = as_number(timing, "actual_wall_fps", failures, "timing")
    as_number(timing, "median_ms", failures, "timing")
    as_number(timing, "p95_ms", failures, "timing")
    as_number(timing, "p99_ms", failures, "timing")

    memory = require_obj(data, "memory", failures)
    if memory.get("heap_high_water_bytes") is None and memory.get("rss_kb") is None:
        failures.append("memory must include heap_high_water_bytes or rss_kb")
    if memory.get("heap_high_water_bytes") is not None:
        as_number(memory, "heap_high_water_bytes", failures, "memory")
    if memory.get("rss_kb") is not None:
        as_number(memory, "rss_kb", failures, "memory")

    validation = require_obj(data, "validation", failures)
    output_valid = as_bool(validation, "output_valid", failures, "validation")
    no_drops = as_bool(validation, "no_drops", failures, "validation")
    visual_checked = as_bool(validation, "visual_checked", failures, "validation")

    verdict = require_obj(data, "verdict", failures)
    ui_ready = as_bool(verdict, "ui_ready", failures, "verdict")
    fps_target_met = as_bool(verdict, "fps_target_met", failures, "verdict")
    target_evidence = as_bool(verdict, "target_evidence", failures, "verdict")
    if fps_median is not None and target_fps is not None and fps_target_met is not None:
        median_ok = fps_median >= target_fps
        wall_ok = True if actual_wall_fps is None else actual_wall_fps >= target_fps
        if fps_target_met != (median_ok and wall_ok):
            failures.append("verdict.fps_target_met must match median and wall fps target checks")

    if ui_ready:
        if role != "camera":
            failures.append("ui-ready receipt must use target.role=camera")
        if ui_path_executed is not True:
            failures.append("ui-ready receipt must execute the camera UI path")
        if target_evidence is not True:
            failures.append("ui-ready receipt must mark target_evidence true")
        if source_provenance_available is not True:
            failures.append("ui-ready receipt must include available source_provenance")
        for label, value in (
            ("verdict.fps_target_met", fps_target_met),
            ("validation.output_valid", output_valid),
            ("validation.no_drops", no_drops),
            ("validation.visual_checked", visual_checked),
            ("preview.full_frame_downsample", full_frame_downsample),
        ):
            if value is not True:
                failures.append(f"ui-ready receipt requires {label}=true")
    elif role == "camera":
        blocker = data.get("blocker")
        if not isinstance(blocker, dict) or not blocker.get("cause"):
            failures.append("blocked camera preview receipt must include blocker.cause")

    if role == "camera" and ui_path_executed is not True:
        failures.append("camera preview receipt must set integration.ui_path_executed=true")

    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("receipt", type=Path, help="preview UI receipt JSON")
    args = ap.parse_args()

    data = json.loads(args.receipt.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        print("receipt must be a JSON object", file=sys.stderr)
        return 1
    failures = validate_receipt(data)
    if failures:
        print("Labs preview UI receipt failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    print("Labs preview UI receipt OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
