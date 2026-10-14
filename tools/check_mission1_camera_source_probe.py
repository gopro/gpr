#!/usr/bin/env python3
"""Validate a Mission 1 camera source probe receipt."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr.mission1_camera_source_probe.v1"
CAMERA_SOURCE_KINDS = {"sensor_dma_capture", "camera_ring_buffer"}


def load_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise ValueError(f"{path}: cannot read JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError(f"{path}: receipt root must be a JSON object")
    return data


def validate(data: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    if data.get("schema") != SCHEMA:
        failures.append(f"schema must be {SCHEMA}")

    target = data.get("target")
    if not isinstance(target, dict):
        failures.append("target must be an object")
        target = {}
    inputs = data.get("inputs")
    if not isinstance(inputs, dict):
        failures.append("inputs must be an object")
        inputs = {}
    verdict = data.get("verdict")
    if not isinstance(verdict, dict):
        failures.append("verdict must be an object")
        verdict = {}

    if verdict.get("source_ready") is not True:
        blockers = data.get("blockers")
        if isinstance(blockers, list) and blockers:
            failures.append("source_ready is false: " + "; ".join(str(item) for item in blockers))
        else:
            failures.append("source_ready must be true")

    checks = data.get("checks")
    if not isinstance(checks, list) or not checks:
        failures.append("checks must be a non-empty list")
    else:
        failed_checks = [
            str(check.get("name", "<unnamed>"))
            for check in checks
            if not isinstance(check, dict) or check.get("passed") is not True
        ]
        if failed_checks:
            failures.append("failed checks: " + ", ".join(failed_checks))

    raw_source_kind = inputs.get("raw_source_kind")
    if raw_source_kind not in {"file_standin", *CAMERA_SOURCE_KINDS}:
        failures.append("inputs.raw_source_kind is missing or unsupported")

    if target.get("role") == "camera" and raw_source_kind not in CAMERA_SOURCE_KINDS:
        failures.append("camera target requires sensor_dma_capture or camera_ring_buffer raw source")

    probe = data.get("probe")
    if not isinstance(probe, dict):
        failures.append("probe must be an object")
    elif not isinstance(probe.get("path"), dict):
        failures.append("probe.path must be an object")

    return failures


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("receipt", type=Path)
    return ap


def main() -> int:
    args = parser().parse_args()
    try:
        data = load_json(args.receipt)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    failures = validate(data)
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("mission1 camera source probe: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
