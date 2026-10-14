#!/usr/bin/env python3
"""Validate a Mission 1 camera closure-run receipt."""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr.mission1_camera_closure_run.v1"
ROOT = Path(__file__).resolve().parents[1]


def load_validator(script_name: str):
    path = ROOT / "tools" / script_name
    spec = importlib.util.spec_from_file_location(path.stem, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_receipt


def require_obj(data: dict[str, Any], key: str, failures: list[str]) -> dict[str, Any]:
    value = data.get(key)
    if not isinstance(value, dict):
        failures.append(f"{key} must be an object")
        return {}
    return value


def resolve_receipt(path_value: Any, base: Path) -> Path | None:
    if not isinstance(path_value, str) or not path_value:
        return None
    path = Path(path_value)
    if path.is_absolute():
        if path.exists():
            return path
        sibling = base / path.name
        if sibling.exists():
            return sibling
        return path
    return base / path


def sha256_value(data: dict[str, Any], *keys: str) -> str | None:
    current: Any = data
    for key in keys:
        if not isinstance(current, dict):
            return None
        current = current.get(key)
    if isinstance(current, str) and len(current) == 64:
        return current
    return None


def aggregate_consistency(
    target_bench: dict[str, Any],
    handoff: dict[str, Any],
    preview_ui: dict[str, Any],
) -> list[str]:
    failures: list[str] = []
    if target_bench.get("schema") != "gpr_labs_target_bench.v1":
        failures.append("target_bench.schema must be gpr_labs_target_bench.v1")
    if target_bench.get("simulated") is True:
        failures.append("target_bench must not be simulated")
    target_verdict = target_bench.get("verdict")
    if not isinstance(target_verdict, dict) or target_verdict.get("target_evidence") is not True:
        failures.append("target_bench.verdict.target_evidence must be true")

    target_capture = target_bench.get("capture") if isinstance(target_bench.get("capture"), dict) else {}
    handoff_input = handoff.get("input_frame") if isinstance(handoff.get("input_frame"), dict) else {}
    handoff_capture = handoff.get("capture") if isinstance(handoff.get("capture"), dict) else {}
    preview_source = preview_ui.get("source") if isinstance(preview_ui.get("source"), dict) else {}
    comparisons = (
        ("source width", target_capture.get("source_width"), handoff_input.get("width")),
        ("source height", target_capture.get("source_height"), handoff_input.get("height")),
        ("capture width", target_capture.get("capture_width"), preview_source.get("width")),
        ("capture height", target_capture.get("capture_height"), preview_source.get("height")),
        ("pixel format", target_capture.get("pixel_format"), handoff_input.get("pixel_format")),
        ("preview pixel format", target_capture.get("pixel_format"), preview_source.get("pixel_format")),
        ("frames written", target_capture.get("frames_written"), handoff_capture.get("frames_written")),
        ("preview frame count", target_capture.get("frames_written"), preview_source.get("frame_count")),
        ("dropped frames", target_capture.get("dropped_frames"), handoff_capture.get("dropped_frames")),
    )
    for label, left, right in comparisons:
        if left != right:
            failures.append(f"{label} mismatch: target_bench={left!r} receipt={right!r}")

    target_sha = sha256_value(target_bench, "gvid", "sha256")
    handoff_sha = sha256_value(handoff, "output", "sha256")
    preview_sha = sha256_value(preview_ui, "source", "gvid_sha256")
    if handoff_sha != preview_sha:
        failures.append("camera_handoff output.sha256 must match preview_ui source.gvid_sha256")
    if target_sha is not None and target_sha != handoff_sha:
        failures.append("target_bench gvid.sha256 must match camera_handoff output.sha256 when present")

    target_source_sha = sha256_value(target_bench, "source_provenance", "sha256")
    handoff_source_sha = sha256_value(handoff, "source_provenance", "sha256")
    preview_source_sha = sha256_value(preview_ui, "source_provenance", "sha256")
    if handoff_source_sha != preview_source_sha:
        failures.append("camera_handoff and preview_ui source_provenance.sha256 must match")
    if target_source_sha is not None and target_source_sha != handoff_source_sha:
        failures.append("target_bench source_provenance.sha256 must match receipt source provenance")
    return failures


def validate(data: dict[str, Any], *, base: Path) -> list[str]:
    failures: list[str] = []
    if data.get("schema") != SCHEMA:
        failures.append(f"schema must be {SCHEMA}")

    receipts = require_obj(data, "receipts", failures)
    steps = data.get("steps")
    if not isinstance(steps, list):
        failures.append("steps must be a list")
        steps = []
    step_by_name = {step.get("name"): step for step in steps if isinstance(step, dict)}
    for name in ("validate_camera_handoff_receipt", "validate_preview_ui_receipt"):
        step = step_by_name.get(name)
        if not isinstance(step, dict):
            failures.append(f"steps must include {name}")
        elif step.get("returncode") != 0:
            failures.append(f"{name} must return 0")

    handoff_path = resolve_receipt(receipts.get("camera_handoff"), base)
    preview_ui_path = resolve_receipt(receipts.get("preview_ui"), base)
    preflight_path = resolve_receipt(receipts.get("target_preflight"), base)
    target_bench_path = resolve_receipt(receipts.get("target_bench"), base)
    handoff: dict[str, Any] = {}
    preview_ui: dict[str, Any] = {}
    preflight: dict[str, Any] = {}
    target_bench: dict[str, Any] = {}
    if target_bench_path is None:
        failures.append("receipts.target_bench must be a non-empty path")
    elif not target_bench_path.exists():
        failures.append(f"target bench receipt does not exist: {target_bench_path}")
    else:
        target_bench = json.loads(target_bench_path.read_text(encoding="utf-8"))
        if not isinstance(target_bench, dict):
            failures.append("target bench receipt must be a JSON object")

    if handoff_path is None:
        failures.append("receipts.camera_handoff must be a non-empty path")
    elif not handoff_path.exists():
        failures.append(f"camera handoff receipt does not exist: {handoff_path}")
    else:
        handoff = json.loads(handoff_path.read_text(encoding="utf-8"))
        receipt_failures = list(load_validator("check_labs_camera_handoff_receipt.py")(handoff))
        failures.extend(f"camera_handoff: {failure}" for failure in receipt_failures)

    if preview_ui_path is None:
        failures.append("receipts.preview_ui must be a non-empty path")
    elif not preview_ui_path.exists():
        failures.append(f"preview UI receipt does not exist: {preview_ui_path}")
    else:
        preview_ui = json.loads(preview_ui_path.read_text(encoding="utf-8"))
        receipt_failures = list(load_validator("check_labs_preview_ui_receipt.py")(preview_ui))
        failures.extend(f"preview_ui: {failure}" for failure in receipt_failures)

    if receipts.get("target_preflight") is not None:
        if preflight_path is None:
            failures.append("receipts.target_preflight must be a non-empty path when present")
        elif not preflight_path.exists():
            failures.append(f"target preflight receipt does not exist: {preflight_path}")
        else:
            preflight = json.loads(preflight_path.read_text(encoding="utf-8"))
            if preflight.get("schema") != "gpr.mission1_camera_target_preflight.v1":
                failures.append("target_preflight.schema must be gpr.mission1_camera_target_preflight.v1")
            preflight_verdict = preflight.get("verdict")
            if not isinstance(preflight_verdict, dict):
                failures.append("target_preflight.verdict must be an object")
            else:
                for key in ("target_preflight_ready", "camera_closure_possible"):
                    if not isinstance(preflight_verdict.get(key), bool):
                        failures.append(f"target_preflight.verdict.{key} must be boolean")

    verdict = require_obj(data, "verdict", failures)
    firmware_ready = handoff.get("verdict", {}).get("firmware_ready")
    ui_ready = preview_ui.get("verdict", {}).get("ui_ready")
    target_preflight_ready = preflight.get("verdict", {}).get("target_preflight_ready")
    camera_closure_possible = preflight.get("verdict", {}).get("camera_closure_possible")
    production_ready = (
        firmware_ready is True
        and ui_ready is True
        and target_preflight_ready is True
        and camera_closure_possible is True
        and handoff.get("target", {}).get("role") == "camera"
        and preview_ui.get("target", {}).get("role") == "camera"
    )
    aggregate_failures = aggregate_consistency(target_bench, handoff, preview_ui) if target_bench and handoff and preview_ui else []
    aggregate_ready = not aggregate_failures
    failures.extend(f"aggregate_consistency: {failure}" for failure in aggregate_failures)
    production_ready = production_ready and aggregate_ready
    if verdict.get("aggregate_consistency_ready") is not None and verdict.get("aggregate_consistency_ready") != aggregate_ready:
        failures.append("verdict.aggregate_consistency_ready must match aggregate receipt consistency")
    if verdict.get("firmware_ready") != firmware_ready:
        failures.append("verdict.firmware_ready must match camera handoff receipt")
    if verdict.get("ui_ready") != ui_ready:
        failures.append("verdict.ui_ready must match preview UI receipt")
    if preflight:
        if verdict.get("target_preflight_ready") != target_preflight_ready:
            failures.append("verdict.target_preflight_ready must match target preflight receipt")
        if verdict.get("camera_closure_possible") != camera_closure_possible:
            failures.append("verdict.camera_closure_possible must match target preflight receipt")
    if verdict.get("production_ready") != production_ready:
        failures.append(
            "verdict.production_ready must equal camera-role firmware_ready && "
            "camera-role ui_ready && target_preflight_ready && camera_closure_possible"
        )

    if production_ready:
        if preflight:
            if target_preflight_ready is not True:
                failures.append("production-ready closure run requires target_preflight_ready=true")
            if camera_closure_possible is not True:
                failures.append("production-ready closure run requires camera_closure_possible=true")
        if verdict.get("handoff_blocker") not in (None, ""):
            failures.append("production-ready closure run must not include handoff_blocker")
        if verdict.get("preview_blocker") not in (None, ""):
            failures.append("production-ready closure run must not include preview_blocker")
        if handoff.get("target", {}).get("role") != "camera":
            failures.append("production-ready closure run requires camera-role handoff receipt")
        if preview_ui.get("target", {}).get("role") != "camera":
            failures.append("production-ready closure run requires camera-role preview UI receipt")
        if aggregate_ready is not True:
            failures.append("production-ready closure run requires aggregate-consistent target, handoff, and preview receipts")
    else:
        if firmware_ready is not True and not verdict.get("handoff_blocker"):
            failures.append("blocked closure run must include handoff_blocker when firmware is not ready")
        if ui_ready is not True and not verdict.get("preview_blocker"):
            failures.append("blocked closure run must include preview_blocker when UI is not ready")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("receipt", type=Path)
    args = ap.parse_args()
    data = json.loads(args.receipt.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        print("closure run must be a JSON object", file=sys.stderr)
        return 1
    failures = validate(data, base=args.receipt.parent)
    if failures:
        print("Mission 1 camera closure run failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    print("Mission 1 camera closure run OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
