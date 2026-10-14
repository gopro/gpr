#!/usr/bin/env python3
"""Benchmark decoding a .gvid stream to a named fused_decode_cli raw target.

This is intentionally Python-stdlib only so it can be copied to the Pi 5 target
without setting up the project venv. It extracts one payload at a time, runs
`fused_decode_cli`, deletes intermediates, and writes a JSON receipt.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import resource
import statistics
import struct
import subprocess
import time
from pathlib import Path
from typing import Any


GVID_MAGIC = 0x44495647
FRAME_MAGIC = 0x004D5246
DECODE_RE = re.compile(r"DECODE: (\d+)x(\d+) in ([0-9.]+) ms .* in (\d+) bytes")
TARGET_RE = re.compile(r"TARGET: ([^ ]+) (\d+)x(\d+) in ([0-9.]+) ms")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def percentile(values: list[float], frac: float) -> float:
    if not values:
        return 0.0
    idx = min(len(values) - 1, max(0, round((len(values) - 1) * frac)))
    return float(values[idx])


def summarize(values: list[float]) -> dict[str, float | int]:
    vals = sorted(float(v) for v in values)
    if not vals:
        return {"n": 0}
    mean = statistics.mean(vals)
    median = statistics.median(vals)
    return {
        "n": len(vals),
        "mean_ms": mean,
        "median_ms": median,
        "min_ms": vals[0],
        "p25_ms": percentile(vals, 0.25),
        "p75_ms": percentile(vals, 0.75),
        "p95_ms": percentile(vals, 0.95),
        "p99_ms": percentile(vals, 0.99),
        "max_ms": vals[-1],
        "fps_mean": 1000.0 / mean if mean > 0 else 0.0,
        "fps_median": 1000.0 / median if median > 0 else 0.0,
    }


def read_gvid_frames(path: Path) -> list[dict[str, int]]:
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError(f"{path} is too small to be a .gvid stream")
    header = struct.unpack("<IBBHHHIIIII", data[:32])
    if header[0] != GVID_MAGIC:
        raise ValueError(f"{path} does not have GVID magic")
    frame_count_hint = int(header[10])
    frames: list[dict[str, int]] = []
    pos = 32
    while pos < len(data):
        if pos + 16 > len(data):
            raise ValueError(f"{path} has a truncated frame header at byte {pos}")
        magic, payload_size, frame_tag = struct.unpack("<IIQ", data[pos : pos + 16])
        if magic != FRAME_MAGIC:
            raise ValueError(f"{path} has bad frame magic at byte {pos}")
        pos += 16
        if pos + payload_size > len(data):
            raise ValueError(f"{path} has truncated payload at byte {pos}")
        frames.append(
            {
                "frame_index": len(frames),
                "frame_tag": int(frame_tag),
                "payload_offset": pos,
                "payload_size": int(payload_size),
            }
        )
        pos += int(payload_size)
    if frame_count_hint not in (0, len(frames)):
        raise ValueError(f"{path} frame_count_hint={frame_count_hint} but found {len(frames)}")
    return frames


def run_one(
    *,
    gvid_file,
    frame: dict[str, int],
    cli: Path,
    tmp_dir: Path,
    sensor_width: int,
    sensor_height: int,
    target: str,
) -> dict[str, Any]:
    stem = f"frame_{frame['frame_index']:06d}"
    payload = tmp_dir / f"{stem}.gpr"
    raw = tmp_dir / f"{stem}_{target}.raw"
    gvid_file.seek(int(frame["payload_offset"]))
    remaining = int(frame["payload_size"])
    with payload.open("wb") as out:
        while remaining:
            chunk = gvid_file.read(min(1024 * 1024, remaining))
            if not chunk:
                raise EOFError(f"unexpected EOF extracting {stem}")
            out.write(chunk)
            remaining -= len(chunk)

    t0 = time.perf_counter()
    proc = subprocess.run(
        [str(cli), str(payload), str(sensor_width), str(sensor_height), str(raw), target],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    wall_ms = (time.perf_counter() - t0) * 1000.0
    if proc.returncode != 0:
        raise RuntimeError(f"{target} failed on {stem}: {proc.stderr[-2000:]}")
    decode_match = DECODE_RE.search(proc.stderr)
    target_match = TARGET_RE.search(proc.stderr)
    if not decode_match or not target_match:
        raise RuntimeError(f"could not parse decoder output for {stem}: {proc.stderr}")

    dec_w, dec_h, dec_ms, in_bytes = decode_match.groups()
    target_name, width, height, target_ms = target_match.groups()
    if target_name != target:
        raise RuntimeError(f"target mismatch for {stem}: requested {target}, got {target_name}")
    row = {
        "frame_index": int(frame["frame_index"]),
        "frame_tag": int(frame["frame_tag"]),
        "payload_size": int(frame["payload_size"]),
        "decode_width": int(dec_w),
        "decode_height": int(dec_h),
        "target": target_name,
        "width": int(width),
        "height": int(height),
        "decode_ms": float(dec_ms),
        "target_ms": float(target_ms),
        "decode_plus_target_ms": float(dec_ms) + float(target_ms),
        "process_wall_ms": wall_ms,
        "raw_bytes": raw.stat().st_size,
        "input_bytes_reported": int(in_bytes),
    }
    payload.unlink(missing_ok=True)
    raw.unlink(missing_ok=True)
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gvid", type=Path, required=True)
    ap.add_argument("--cli", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--sensor-width", type=int, required=True)
    ap.add_argument("--sensor-height", type=int, required=True)
    ap.add_argument("--target", default="mission1_preview_4x_1024x768")
    ap.add_argument("--limit", type=int, default=0, help="0 means all frames")
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir = args.out_dir / "tmp"
    tmp_dir.mkdir(parents=True, exist_ok=True)

    frames = read_gvid_frames(args.gvid)
    if args.limit > 0:
        frames = frames[: args.limit]

    rows: list[dict[str, Any]] = []
    t_all = time.perf_counter()
    with args.gvid.open("rb") as gvid_file:
        for frame in frames:
            rows.append(
                run_one(
                    gvid_file=gvid_file,
                    frame=frame,
                    cli=args.cli,
                    tmp_dir=tmp_dir,
                    sensor_width=args.sensor_width,
                    sensor_height=args.sensor_height,
                    target=args.target,
                )
            )
            if len(rows) % 50 == 0 or len(rows) == len(frames):
                median = statistics.median(row["decode_plus_target_ms"] for row in rows)
                print(f"{len(rows)}/{len(frames)} median={median:.2f}ms", flush=True)
    wall_s = time.perf_counter() - t_all

    receipt = {
        "schema": "gvid_decode_target_bench.v1",
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": platform.node(),
        "machine": platform.machine(),
        "gvid": str(args.gvid),
        "gvid_sha256": sha256_file(args.gvid),
        "cli": str(args.cli),
        "cli_sha256": sha256_file(args.cli),
        "sensor_width": args.sensor_width,
        "sensor_height": args.sensor_height,
        "raw_target": args.target,
        "frame_count": len(rows),
        "summary": {
            "decode_only": summarize([row["decode_ms"] for row in rows]),
            "target_only": summarize([row["target_ms"] for row in rows]),
            "decode_plus_target": summarize([row["decode_plus_target_ms"] for row in rows]),
            "process_wall": summarize([row["process_wall_ms"] for row in rows]),
            "actual_wall_s": wall_s,
            "actual_wall_fps_including_extract_process": len(rows) / wall_s if wall_s > 0 else 0.0,
            "dims": sorted({(row["width"], row["height"]) for row in rows}),
            "raw_bytes_mean": statistics.mean(row["raw_bytes"] for row in rows) if rows else 0,
            "payload_bytes_mean": statistics.mean(row["payload_size"] for row in rows) if rows else 0,
        },
        "memory": {
            "children_maxrss_kb": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
            "parent_maxrss_kb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        },
        "rows": rows,
    }
    receipt_path = args.out_dir / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"receipt": str(receipt_path), "summary": receipt["summary"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
