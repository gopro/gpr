#!/usr/bin/env python3
"""Validate a Labs raw media extension manifest."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gopro.labs.raw_media_extension.v1"
DELIVERY_VALUES = {"static_labs_feature", "signed_bundle", "firmware_builtin"}
REQUIRED_CAPABILITIES = {
    "raw_bayer_frame_source",
    "sd_writer",
    "preview_surface",
    "media_indexer",
    "receipt_writer",
}
PIXEL_FORMATS = {
    "rggb14",
    "gbrg14",
    "bggr14",
    "grbg14",
    "rggb16",
    "gbrg16",
    "bggr16",
    "grbg16",
}
BIT_DEPTHS = {14, 16}
PAYLOAD_KINDS = {"gpr_vc5_frame", "raw16_frame"}


def require(condition: bool, failures: list[str], message: str) -> None:
    if not condition:
        failures.append(message)


def is_nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def is_positive_int(value: Any) -> bool:
    return isinstance(value, int) and value > 0


def validate_capture_mode(mode: Any, failures: list[str], index: int) -> None:
    prefix = f"capture_modes[{index}]"
    if not isinstance(mode, dict):
        failures.append(f"{prefix} must be an object")
        return

    for key in ("id", "description", "pixel_format", "payload_kind"):
        require(is_nonempty_string(mode.get(key)), failures, f"{prefix}.{key} must be a non-empty string")
    for key in ("width", "height", "fps_floor"):
        require(is_positive_int(mode.get(key)), failures, f"{prefix}.{key} must be a positive integer")

    if is_nonempty_string(mode.get("pixel_format")):
        require(mode["pixel_format"] in PIXEL_FORMATS, failures, f"{prefix}.pixel_format is not supported")
    require(mode.get("bit_depth") in BIT_DEPTHS, failures, f"{prefix}.bit_depth must be one of {sorted(BIT_DEPTHS)}")
    if is_nonempty_string(mode.get("pixel_format")) and mode.get("bit_depth") in BIT_DEPTHS:
        require(mode["pixel_format"].endswith(str(mode["bit_depth"])), failures, f"{prefix}.pixel_format must match bit_depth")
    if is_nonempty_string(mode.get("payload_kind")):
        require(mode["payload_kind"] in PAYLOAD_KINDS, failures, f"{prefix}.payload_kind is not supported")

    preview = mode.get("preview")
    if not isinstance(preview, dict):
        failures.append(f"{prefix}.preview must be an object")
        return
    for key in ("width", "height", "fps_floor"):
        require(is_positive_int(preview.get(key)), failures, f"{prefix}.preview.{key} must be a positive integer")
    require(is_nonempty_string(preview.get("format")), failures, f"{prefix}.preview.format must be a non-empty string")

    if is_positive_int(mode.get("width")) and is_positive_int(preview.get("width")):
        require(preview["width"] <= mode["width"], failures, f"{prefix}.preview.width must not exceed capture width")
    if is_positive_int(mode.get("height")) and is_positive_int(preview.get("height")):
        require(preview["height"] <= mode["height"], failures, f"{prefix}.preview.height must not exceed capture height")


def validate_manifest(data: Any) -> list[str]:
    failures: list[str] = []
    if not isinstance(data, dict):
        return ["manifest must be a JSON object"]

    require(data.get("schema") == SCHEMA, failures, f"schema must be {SCHEMA}")
    require(is_nonempty_string(data.get("id")), failures, "id must be a non-empty string")
    require(is_nonempty_string(data.get("name")), failures, "name must be a non-empty string")
    require(is_positive_int(data.get("abi")), failures, "abi must be a positive integer")
    require(data.get("delivery") in DELIVERY_VALUES, failures, "delivery must be a supported value")

    camera_models = data.get("camera_models")
    require(isinstance(camera_models, list) and bool(camera_models), failures, "camera_models must be a non-empty list")
    if isinstance(camera_models, list):
        for idx, value in enumerate(camera_models):
            require(is_nonempty_string(value), failures, f"camera_models[{idx}] must be a non-empty string")

    requires = data.get("requires")
    require(isinstance(requires, list) and bool(requires), failures, "requires must be a non-empty list")
    if isinstance(requires, list):
        missing = REQUIRED_CAPABILITIES - set(requires)
        require(not missing, failures, "requires is missing capabilities: " + ", ".join(sorted(missing)))

    media_types = data.get("media_types")
    require(isinstance(media_types, list) and ".gvid" in media_types, failures, "media_types must include .gvid")

    modes = data.get("capture_modes")
    require(isinstance(modes, list) and bool(modes), failures, "capture_modes must be a non-empty list")
    if isinstance(modes, list):
        seen: set[str] = set()
        for idx, mode in enumerate(modes):
            validate_capture_mode(mode, failures, idx)
            if isinstance(mode, dict) and is_nonempty_string(mode.get("id")):
                require(mode["id"] not in seen, failures, f"duplicate capture mode id: {mode['id']}")
                seen.add(mode["id"])

    limits = data.get("resource_limits")
    if not isinstance(limits, dict):
        failures.append("resource_limits must be an object")
    else:
        for key in ("max_rss_mb", "max_frame_time_ms", "max_write_mbps", "max_dropped_frames"):
            require(isinstance(limits.get(key), int) and limits[key] >= 0, failures, f"resource_limits.{key} must be a non-negative integer")
        require(limits.get("max_dropped_frames") == 0, failures, "resource_limits.max_dropped_frames must be 0 for raw capture promotion")

    control = data.get("control")
    if not isinstance(control, dict):
        failures.append("control must be an object")
    else:
        require(isinstance(control.get("labs_commands"), list) and bool(control.get("labs_commands")), failures, "control.labs_commands must be a non-empty list")
        require(isinstance(control.get("open_gopro_http"), list) and bool(control.get("open_gopro_http")), failures, "control.open_gopro_http must be a non-empty list")

    receipts = data.get("receipts")
    if not isinstance(receipts, dict):
        failures.append("receipts must be an object")
    else:
        for key in ("capture", "preview", "install"):
            require(is_nonempty_string(receipts.get(key)), failures, f"receipts.{key} must be a non-empty string")

    security = data.get("security")
    if not isinstance(security, dict):
        failures.append("security must be an object")
    else:
        require(security.get("signature_required") is True, failures, "security.signature_required must be true")
        require(security.get("native_code_allowed") is False, failures, "security.native_code_allowed must be false for the initial proposal")
        require(security.get("rollback_protection") is True, failures, "security.rollback_protection must be true")
        require(security.get("resource_limits_enforced") is True, failures, "security.resource_limits_enforced must be true")

    return failures


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("manifest", type=Path)
    return ap


def main() -> int:
    args = parser().parse_args()
    try:
        data = json.loads(args.manifest.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"could not load manifest: {exc}", file=sys.stderr)
        return 1

    failures = validate_manifest(data)
    if failures:
        print("Labs extension manifest validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"OK - Labs extension manifest valid: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
