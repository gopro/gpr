#!/usr/bin/env python3
"""Validate camera-noise calibration sidecars and their source contracts."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

NOISE_SCHEMA = "gpr.camera_noise_calibration.v1"
NORMAL_BAYER_PHASES = {"RGGB", "GBRG", "BGGR", "GRBG"}
NOISE_SOURCE_KINDS = {"darkframes", "frame_stack", "dng_noise_profile", "flat_dark_pair"}


def require_obj(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> dict[str, Any]:
    value = obj.get(key)
    label = f"{prefix}.{key}" if prefix else key
    if not isinstance(value, dict):
        failures.append(f"{label} must be an object")
        return {}
    return value

def require_list(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> list[Any]:
    value = obj.get(key)
    label = f"{prefix}.{key}" if prefix else key
    if not isinstance(value, list) or not value:
        failures.append(f"{label} must be a non-empty list")
        return []
    return value

def require_string(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> str | None:
    value = obj.get(key)
    label = f"{prefix}.{key}" if prefix else key
    if not isinstance(value, str) or not value:
        failures.append(f"{label} must be a non-empty string")
        return None
    return value

def require_sha256(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> str | None:
    value = require_string(obj, key, failures, prefix)
    label = f"{prefix}.{key}" if prefix else key
    if value and (len(value) != 64 or any(ch not in "0123456789abcdefABCDEF" for ch in value)):
        failures.append(f"{label} must be a 64-character hex digest")
    return value

def require_bool(obj: dict[str, Any], key: str, failures: list[str], prefix: str) -> bool | None:
    value = obj.get(key)
    label = f"{prefix}.{key}" if prefix else key
    if not isinstance(value, bool):
        failures.append(f"{label} must be boolean")
        return None
    return value

def require_int(
    obj: dict[str, Any],
    key: str,
    failures: list[str],
    prefix: str,
    *,
    minimum: int | None = None,
) -> int | None:
    value = obj.get(key)
    label = f"{prefix}.{key}" if prefix else key
    if isinstance(value, bool) or not isinstance(value, int):
        failures.append(f"{label} must be integer")
        return None
    if minimum is not None and value < minimum:
        failures.append(f"{label} must be >= {minimum}")
    return value

def require_number(
    obj: dict[str, Any],
    key: str,
    failures: list[str],
    prefix: str,
    *,
    minimum: float | None = None,
    maximum: float | None = None,
) -> float | None:
    value = obj.get(key)
    label = f"{prefix}.{key}" if prefix else key
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        failures.append(f"{label} must be numeric")
        return None
    result = float(value)
    if minimum is not None and result < minimum:
        failures.append(f"{label} must be >= {minimum}")
    if maximum is not None and result > maximum:
        failures.append(f"{label} must be <= {maximum}")
    return result

def validate_artifact_ref(obj: dict[str, Any], failures: list[str], prefix: str) -> None:
    require_string(obj, "path", failures, prefix)
    require_sha256(obj, "sha256", failures, prefix)

def validate_noise_calibration(data: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    if data.get("schema") != NOISE_SCHEMA:
        failures.append(f"schema must be {NOISE_SCHEMA}")

    camera = require_obj(data, "camera", failures, "")
    require_string(camera, "make", failures, "camera")
    require_string(camera, "model", failures, "camera")
    require_int(camera, "width", failures, "camera", minimum=2)
    require_int(camera, "height", failures, "camera", minimum=2)
    require_int(camera, "bit_depth", failures, "camera", minimum=8)
    cfa = require_string(camera, "cfa_phase", failures, "camera")
    if cfa and cfa not in NORMAL_BAYER_PHASES:
        failures.append("camera.cfa_phase must be one of RGGB, GBRG, BGGR, GRBG")
    require_number(camera, "black_level", failures, "camera", minimum=0)
    require_number(camera, "white_level", failures, "camera", minimum=1)

    calibrations = require_list(data, "calibrations", failures, "")
    usable_count = 0
    for idx, item in enumerate(calibrations):
        prefix = f"calibrations[{idx}]"
        if not isinstance(item, dict):
            failures.append(f"{prefix} must be an object")
            continue
        require_int(item, "iso", failures, prefix, minimum=1)
        require_string(item, "calibration_method", failures, prefix)
        source_kind = require_string(item, "source_kind", failures, prefix)
        if source_kind and source_kind not in NOISE_SOURCE_KINDS:
            failures.append(f"{prefix}.source_kind must be one of {sorted(NOISE_SOURCE_KINDS)}")
        require_int(item, "sample_count", failures, prefix, minimum=1)
        validate_artifact_ref(require_obj(item, "source", failures, prefix), failures, f"{prefix}.source")
        planes = require_obj(item, "per_plane", failures, prefix)
        for plane in ("r", "g1", "b", "g2"):
            metrics = require_obj(planes, plane, failures, f"{prefix}.per_plane")
            require_number(metrics, "noise_profile_scale", failures, f"{prefix}.per_plane.{plane}", minimum=0)
            require_number(metrics, "noise_profile_offset", failures, f"{prefix}.per_plane.{plane}", minimum=0)
            require_number(metrics, "mean_black", failures, f"{prefix}.per_plane.{plane}", minimum=0)
            require_number(metrics, "sigma_black", failures, f"{prefix}.per_plane.{plane}", minimum=0)
        audit = require_obj(item, "noise_signal_audit", failures, prefix)
        separates = require_bool(audit, "separates_noise_from_signal", failures, f"{prefix}.noise_signal_audit")
        require_string(audit, "method", failures, f"{prefix}.noise_signal_audit")
        require_string(audit, "evidence", failures, f"{prefix}.noise_signal_audit")
        usable = require_bool(item, "usable_for_training_targets", failures, prefix)
        if usable:
            usable_count += 1
            if separates is not True:
                failures.append(f"{prefix} cannot be usable_for_training_targets without a passing noise/signal audit")
            if item.get("sample_count", 0) < 4 and source_kind in {"darkframes", "frame_stack", "flat_dark_pair"}:
                failures.append(f"{prefix} needs at least 4 frames before training targets can use it")
            if source_kind == "dng_noise_profile":
                failures.append(f"{prefix} cannot mark a metadata-only DNG NoiseProfile as usable_for_training_targets")

    if require_bool(data, "production_ready", failures, "") is True and usable_count == 0:
        failures.append("production_ready noise calibration requires at least one usable calibration")
    return failures

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("receipt", type=Path, nargs="+", help="receipt JSON file(s)")
    args = ap.parse_args()

    failed = False
    for path in args.receipt:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            print(f"{path}: receipt must be a JSON object", file=sys.stderr)
            failed = True
            continue
        failures = validate_noise_calibration(data)
        if failures:
            print(f"{path}: noise-calibration receipt failed:", file=sys.stderr)
            for failure in failures:
                print(f" - {failure}", file=sys.stderr)
            failed = True
        else:
            print(f"{path}: noise-calibration receipt OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
