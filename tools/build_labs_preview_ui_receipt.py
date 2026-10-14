#!/usr/bin/env python3
"""Build a Labs preview UI receipt from target and preview bench receipts."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


SCHEMA = "gpr_labs_preview_ui_receipt.v1"
STANDIN_LABEL_TOKENS = ("stand-in", "off-camera", "file/video")


def load_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"{path} must contain a JSON object")
    return data


def stat(summary: dict[str, Any], key: str) -> float:
    value = summary.get(key)
    return float(value if value is not None else 0.0)


def contains_standin_label(value: str) -> bool:
    lowered = value.lower()
    return any(token in lowered for token in STANDIN_LABEL_TOKENS)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target-bench", type=Path, required=True)
    ap.add_argument("--preview-receipt", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--target-name", default="Pi 5 stand-in")
    ap.add_argument("--target-role", choices=("stand-in", "camera"), default="stand-in")
    ap.add_argument("--ui-path-executed", action="store_true")
    ap.add_argument("--visual-checked", action="store_true")
    ap.add_argument("--display-surface", default="stand-in file/video output")
    ap.add_argument("--presentation-path", default="off-camera preview receipt path")
    ap.add_argument("--buffer-ownership", default="process-owned RGB output buffer")
    ap.add_argument("--decode-path", default="fused_decode_cli mission1_preview_4x_1024x768")
    ap.add_argument("--color-pipeline", default="full-frame Bayer decode to RGB preview")
    ap.add_argument("--tone-pipeline", default="preview tone path from fused decoder target")
    ap.add_argument("--blocker-cause", default="camera UI path not executed")
    args = ap.parse_args()

    target = load_json(args.target_bench)
    preview = load_json(args.preview_receipt)

    target_capture = target["capture"]
    source_provenance = target.get("source_provenance")
    if not isinstance(source_provenance, dict):
        source_provenance = {"available": False, "policy": "not recorded"}
    if args.target_role == "camera":
        if not source_provenance.get("available"):
            raise ValueError("camera preview receipts require available source_provenance")
        if not args.ui_path_executed:
            raise ValueError("camera preview receipts require --ui-path-executed")
        if not args.visual_checked:
            raise ValueError("camera preview receipts require --visual-checked")
        for label, value in (
            ("--display-surface", args.display_surface),
            ("--presentation-path", args.presentation_path),
            ("--buffer-ownership", args.buffer_ownership),
        ):
            if contains_standin_label(value):
                raise ValueError(f"camera preview receipts cannot use stand-in label in {label}: {value!r}")

    summary = preview["summary"]
    decode_plus = summary["decode_plus_target"]
    process_wall = summary.get("process_wall", {})
    dims = summary["dims"]
    if len(dims) != 1:
        raise ValueError(f"{args.preview_receipt}: expected one preview dimension, got {dims!r}")
    preview_width, preview_height = dims[0]
    frame_count = int(preview["frame_count"])
    target_fps = float(target["target"]["fps"])
    fps_median = stat(decode_plus, "fps_median")
    actual_wall_fps = stat(summary, "actual_wall_fps_including_extract_process")
    fps_target_met = fps_median >= target_fps and actual_wall_fps >= target_fps
    output_valid = frame_count == int(target_capture["frames_written"])
    no_drops = int(target_capture["dropped_frames"]) == 0 and output_valid
    ui_ready = (
        args.target_role == "camera"
        and args.ui_path_executed
        and args.visual_checked
        and fps_target_met
        and output_valid
        and no_drops
    )

    receipt: dict[str, Any] = {
        "schema": SCHEMA,
        "source_provenance": {
            "available": bool(source_provenance.get("available")),
            "policy": str(source_provenance.get("policy", "not recorded")),
        },
        "target": {
            "name": args.target_name,
            "role": args.target_role,
        },
        "source": {
            "width": int(target_capture["capture_width"]),
            "height": int(target_capture["capture_height"]),
            "frame_count": frame_count,
            "bit_depth": 14,
            "pixel_format": int(target_capture["pixel_format"]),
            "gvid_sha256": preview["gvid_sha256"],
        },
        "preview": {
            "width": int(preview_width),
            "height": int(preview_height),
            "frame_count": frame_count,
            "target_fps": target_fps,
            "full_frame_downsample": True,
            "color_pipeline": args.color_pipeline,
            "tone_pipeline": args.tone_pipeline,
        },
        "integration": {
            "ui_path_executed": bool(args.ui_path_executed),
            "decode_path": args.decode_path,
            "presentation_path": args.presentation_path,
            "buffer_ownership": args.buffer_ownership,
            "display_surface": args.display_surface,
        },
        "timing": {
            "fps_median": fps_median,
            "actual_wall_fps": actual_wall_fps,
            "median_ms": stat(decode_plus, "median_ms"),
            "p95_ms": stat(decode_plus, "p95_ms"),
            "p99_ms": stat(decode_plus, "p99_ms"),
            "process_wall_median_ms": stat(process_wall, "median_ms") if isinstance(process_wall, dict) else 0.0,
        },
        "memory": {
            "rss_kb": int(
                preview.get("memory", {}).get("children_maxrss_kb")
                or preview.get("memory", {}).get("parent_maxrss_kb")
                or 0
            )
        },
        "validation": {
            "output_valid": output_valid,
            "no_drops": no_drops,
            "visual_checked": bool(args.visual_checked),
        },
        "verdict": {
            "ui_ready": ui_ready,
            "target_evidence": args.target_role in {"stand-in", "camera"},
            "fps_target_met": fps_target_met,
        },
        "blocker": {
            "cause": "none" if ui_ready else args.blocker_cause,
        },
    }
    if source_provenance.get("available"):
        receipt["source_provenance"].update(
            {
                "sha256": source_provenance["sha256"],
                "file_count": int(source_provenance["file_count"]),
                "total_bytes": int(source_provenance["total_bytes"]),
            }
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "ui_ready": ui_ready}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
