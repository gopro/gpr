#!/usr/bin/env python3
"""Convert a Labs target-bench receipt to a camera-handoff receipt."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr_labs_camera_handoff_receipt.v1"
STANDIN_LABEL_TOKENS = ("stand-in", "file-backed", "bench_fused", "page-cache", "filesystem stand-in")


def load_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"{path} must contain a JSON object")
    return data


def obj(data: dict[str, Any], key: str) -> dict[str, Any]:
    value = data.get(key)
    return value if isinstance(value, dict) else {}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def output_sha256(gvid: dict[str, Any]) -> str:
    value = gvid.get("sha256")
    if isinstance(value, str) and value:
        return value
    path_value = gvid.get("path")
    if isinstance(path_value, str) and path_value:
        path = Path(path_value)
        if path.is_file():
            return sha256_file(path)
    return ""


def contains_standin_label(value: str) -> bool:
    lowered = value.lower()
    return any(token in lowered for token in STANDIN_LABEL_TOKENS)


def convert(args: argparse.Namespace) -> dict[str, Any]:
    source = load_json(args.target_bench)
    target = obj(source, "target")
    capture = obj(source, "capture")
    timing = obj(source, "timing")
    storage = obj(source, "storage")
    memory = obj(source, "memory")
    gvid = obj(source, "gvid")
    validation = obj(gvid, "validation")
    recovery = obj(source, "interruption_recovery")
    verdict = obj(source, "verdict")

    if args.target_role == "camera":
        if args.raw_source_kind == "file_standin":
            raise ValueError("camera handoff receipts require --raw-source-kind sensor_dma_capture or camera_ring_buffer")
        source_provenance = source.get("source_provenance")
        if not isinstance(source_provenance, dict) or not source_provenance.get("available"):
            raise ValueError("camera handoff receipts require available source_provenance")
        if not args.sensor_dma_executed:
            raise ValueError("camera handoff receipts require --sensor-dma-executed")
        if not args.storage_handoff_executed:
            raise ValueError("camera handoff receipts require --storage-handoff-executed")
        for label, value in (
            ("--frame-source", args.frame_source),
            ("--write-path", args.write_path),
            ("--storage-medium", args.storage_medium),
            ("--storage-ownership", args.storage_ownership),
        ):
            if contains_standin_label(value):
                raise ValueError(f"camera handoff receipts cannot use stand-in label in {label}: {value!r}")

    target_fps = float(args.target_fps if args.target_fps is not None else target.get("fps", 24.0))
    fps_median = float(timing.get("fps_median", 0.0))
    dropped = int(capture.get("dropped_frames", 0))
    gvid_valid = bool(verdict.get("gvid_valid", validation.get("valid", False)))
    recovery_proven = bool(verdict.get("interruption_recovery_proven", False))
    no_drops = dropped == 0
    actual_wall_fps = target.get("actual_wall_fps")
    wall_fps = float(actual_wall_fps) if actual_wall_fps is not None else None
    median_target_met = fps_median >= target_fps
    wall_target_met = True if wall_fps is None else wall_fps >= target_fps
    fps_target_met = bool(verdict.get("fps_target_met", median_target_met and wall_target_met))
    gvid_sha256 = output_sha256(gvid)

    firmware_ready = (
        args.target_role == "camera"
        and args.sensor_dma_executed
        and args.storage_handoff_executed
        and bool(verdict.get("target_evidence", False))
        and fps_target_met
        and no_drops
        and gvid_valid
        and recovery_proven
        and bool(recovery.get("validator_rejects_truncated", False))
    )

    receipt: dict[str, Any] = {
        "schema": SCHEMA,
        "source_receipt": str(args.target_bench),
        "repo_commit": source.get("repo_commit"),
        "source_provenance": source.get("source_provenance"),
        "created_utc": source.get("created_utc"),
        "target": {
            "name": args.target_name or str(target.get("name") or "unknown target"),
            "role": args.target_role,
        },
        "integration": {
            "raw_source_kind": args.raw_source_kind,
            "frame_source": args.frame_source,
            "memory_ownership": args.memory_ownership,
            "write_path": args.write_path,
            "sensor_dma_handoff": {"executed": bool(args.sensor_dma_executed)},
            "storage_handoff": {
                "executed": bool(args.storage_handoff_executed),
                "medium": args.storage_medium,
                "ownership": args.storage_ownership,
            },
        },
        "input_frame": {
            "width": int(capture.get("source_width", validation.get("width", 0))),
            "height": int(capture.get("source_height", validation.get("height", 0))),
            "stride_bytes": int(args.stride_bytes),
            "bit_depth": int(args.bit_depth),
            "pixel_format": int(capture.get("pixel_format", args.pixel_format)),
            "target_fps": target_fps,
        },
        "capture": {
            "frames_requested": int(capture.get("frames_requested", timing.get("n", 0))),
            "frames_written": int(capture.get("frames_written", validation.get("frame_count", 0))),
            "dropped_frames": dropped,
        },
        "timing": {
            "fps_median": fps_median,
            "median_ms": float(timing.get("median_ms", 0.0)),
            "p95_ms": float(timing.get("p95_ms", 0.0)),
            "p99_ms": float(timing.get("p99_ms", 0.0)),
        },
        "storage": {
            "write_mb_s": float(storage.get("write_MBps_wall", 0.0)),
            "flush_policy": str(storage.get("fsync_policy") or args.flush_policy),
        },
        "memory": {
            "rss_kb": int(memory.get("bench_child_maxrss_kb", memory.get("wrapper_maxrss_kb", 0))),
        },
        "output": {
            "sha256": gvid_sha256,
            "validation": {
                "valid": gvid_valid,
                "frame_count": int(validation.get("frame_count", capture.get("frames_written", 0))),
            },
        },
        "interruption_recovery": {
            "proven": recovery_proven,
            "validator_rejects_truncated": bool(recovery.get("validator_rejects_truncated", False)),
            "complete_frames_recovered": int(recovery.get("complete_frames_recovered", 0)),
        },
        "verdict": {
            "firmware_ready": firmware_ready,
            "target_evidence": bool(verdict.get("target_evidence", False)),
            "fps_target_met": fps_target_met,
            "fps_median_target_met": bool(verdict.get("fps_median_target_met", median_target_met)),
            "fps_wall_target_met": bool(verdict.get("fps_wall_target_met", wall_target_met)),
            "no_drops": no_drops,
        },
    }
    if wall_fps is not None:
        receipt["timing"]["actual_wall_fps"] = wall_fps
        receipt["timing"]["actual_wall_s"] = float(target.get("actual_wall_s", 0.0))
    if not firmware_ready:
        receipt["blocker"] = {"cause": args.blocker_cause}
    return receipt


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("target_bench", type=Path, help="labs_target_bench.json")
    ap.add_argument("--output", type=Path, required=True, help="output camera handoff receipt")
    ap.add_argument("--target-name", default="", help="override target name")
    ap.add_argument("--target-role", choices=("stand-in", "camera"), default="stand-in")
    ap.add_argument("--raw-source-kind", choices=("file_standin", "sensor_dma_capture", "camera_ring_buffer"), default="file_standin")
    ap.add_argument("--target-fps", type=float, default=None)
    ap.add_argument("--frame-source", default="file-backed Bayer stand-in")
    ap.add_argument("--memory-ownership", default="synchronous submit; caller owns input through return")
    ap.add_argument("--write-path", default="bench_fused target-bench .gvid path")
    ap.add_argument("--sensor-dma-executed", action="store_true")
    ap.add_argument("--storage-handoff-executed", action="store_true")
    ap.add_argument("--storage-medium", default="target-bench filesystem stand-in")
    ap.add_argument("--storage-ownership", default="OS/page-cache writeback; not camera firmware DMA")
    ap.add_argument("--stride-bytes", type=int, default=16560)
    ap.add_argument("--bit-depth", type=int, default=14)
    ap.add_argument("--pixel-format", type=int, default=4)
    ap.add_argument("--flush-policy", default="recorded in source target-bench receipt")
    ap.add_argument("--blocker-cause", default="camera hardware target not firmware-ready")
    args = ap.parse_args()

    try:
        receipt = convert(args)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    except Exception as exc:
        print(f"labs_target_to_camera_handoff_receipt: {exc}", file=sys.stderr)
        return 1

    print(json.dumps({"output": str(args.output), "firmware_ready": receipt["verdict"]["firmware_ready"]}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
