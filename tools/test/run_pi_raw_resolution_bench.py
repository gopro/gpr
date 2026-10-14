#!/usr/bin/env python3
"""Run 2K/4K raw target timing on a Pi-style frame corpus.

This script intentionally uses only the Python standard library. It is meant to
run on the Pi 5 capture target after `fused_decode_cli` has been built there.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import resource
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[2]
DECODE_RE = re.compile(r"DECODE: (\d+)x(\d+) in ([0-9.]+) ms .* in (\d+) bytes")
TARGET_RE = re.compile(r"TARGET: ([^ ]+) (\d+)x(\d+) in ([0-9.]+) ms")
TARGET_2K_CHOICES = ("2k_raw_0p5x", "2k_raw_0p5x_fast", "2k_raw_0p5x_l2hh")
TARGET_2K_POLICY = {
    "2k_raw_0p5x": "compatibility/env-driven",
    "2k_raw_0p5x_fast": "named target: drop L2 highpass",
    "2k_raw_0p5x_l2hh": "named target: restore selective L2 HH",
}
TARGET_2K_CHILD_DECODE_POLICY = {
    "2k_raw_0p5x": {
        "source": "environment",
        "halfres_stream": True,
        "halfres_drop_l2_hp": None,
        "halfres_l2_mask": None,
    },
    "2k_raw_0p5x_fast": {
        "source": "fused_decode_cli named target",
        "halfres_stream": True,
        "halfres_drop_l2_hp": True,
        "halfres_l2_mask": None,
        "stream_strips": 2,
    },
    "2k_raw_0p5x_l2hh": {
        "source": "fused_decode_cli named target",
        "halfres_stream": True,
        "halfres_drop_l2_hp": False,
        "halfres_l2_mask": 4,
        "stream_strips": 2,
    },
}


def percentile(sorted_values: list[float], frac: float) -> float:
    if not sorted_values:
        return 0.0
    idx = min(len(sorted_values) - 1, max(0, round((len(sorted_values) - 1) * frac)))
    return float(sorted_values[idx])


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
        "max_ms": vals[-1],
        "fps_mean": 1000.0 / mean if mean > 0 else 0.0,
        "fps_median": 1000.0 / median if median > 0 else 0.0,
    }


def git_commit() -> str | None:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return None


def file_sha256(path: Path) -> str | None:
    try:
        h = hashlib.sha256()
        with path.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return None


def maxrss_kb(who: int) -> int:
    value = int(resource.getrusage(who).ru_maxrss)
    if sys.platform == "darwin":
        return value // 1024
    return value


def run_target(cli: Path, frame: Path, tmp_dir: Path, target: str) -> dict[str, Any]:
    raw = tmp_dir / f"{frame.stem}_{target}.raw"
    cmd = [str(cli), str(frame), "8280", "5520", str(raw), target]
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(f"{target} failed on {frame}: {result.stderr[-1000:]}")
    decode_match = DECODE_RE.search(result.stderr)
    target_match = TARGET_RE.search(result.stderr)
    if not decode_match or not target_match:
        raise RuntimeError(f"parse failed on {frame}: {result.stderr}")
    dec_w, dec_h, dec_ms, in_bytes = decode_match.groups()
    target_name, width, height, target_ms = target_match.groups()
    if target_name != target:
        raise RuntimeError(f"target mismatch: requested {target}, decoder reported {target_name}")
    raw_bytes = raw.stat().st_size
    raw.unlink(missing_ok=True)
    return {
        "width": int(width),
        "height": int(height),
        "decode_width": int(dec_w),
        "decode_height": int(dec_h),
        "decode_ms": float(dec_ms),
        "target_ms": float(target_ms),
        "decode_plus_target_ms": float(dec_ms) + float(target_ms),
        "input_bytes": int(in_bytes),
        "raw_bytes": int(raw_bytes),
    }


def target_summary(rows: list[dict[str, Any]], name: str) -> dict[str, Any]:
    entries = [row["targets"][name] for row in rows]
    key = "decode_plus_target_ms" if name.startswith("2k_raw_0p5x") else "decode_ms"
    return {
        "count": len(entries),
        "dims": sorted({(entry["width"], entry["height"]) for entry in entries}),
        "timing_key": key,
        "timing": summarize([entry[key] for entry in entries]),
        "decode_only": summarize([entry["decode_ms"] for entry in entries]),
        "target_only": summarize([entry["target_ms"] for entry in entries]),
        "raw_bytes_mean": statistics.mean(entry["raw_bytes"] for entry in entries),
        "input_bytes_mean": statistics.mean(entry["input_bytes"] for entry in entries),
        "cnn": ["none"],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cli", type=Path, default=Path("build/bin/fused_decode_cli"))
    ap.add_argument("--frame-dir", type=Path, default=Path("/mnt/ssd/work/bench_pi2mac"))
    ap.add_argument("--output-dir", type=Path, default=Path("/mnt/ssd/work/raw_resolution_targets_20260613"))
    ap.add_argument("--limit", type=int, default=120)
    ap.add_argument("--target-2k", choices=TARGET_2K_CHOICES, default="2k_raw_0p5x")
    args = ap.parse_args()

    tmp_dir = args.output_dir / "tmp"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir.mkdir(parents=True, exist_ok=True)

    frames = sorted(args.frame_dir.glob("*.gpr"))[: args.limit]
    if not frames:
        raise RuntimeError(f"no .gpr frames found under {args.frame_dir}")

    rows: list[dict[str, Any]] = []
    t0 = time.time()
    for index, frame in enumerate(frames, start=1):
        target_4k = run_target(args.cli, frame, tmp_dir, "4k_raw_1x")
        target_2k = run_target(args.cli, frame, tmp_dir, args.target_2k)
        rows.append({"frame": frame.name, "targets": {"4k_raw_1x": target_4k, args.target_2k: target_2k}})
        if index % 20 == 0 or index == len(frames):
            print(
                f"{index}/{len(frames)} "
                f"4k={target_4k['decode_ms']:.1f}ms "
                f"2k={target_2k['decode_plus_target_ms']:.1f}ms",
                flush=True,
            )

    payload = {
        "schema": "raw_resolution_targets_pi5_bench.v1",
        "host": platform.node(),
        "machine": platform.machine(),
        "frame_dir": str(args.frame_dir),
        "frame_count": len(rows),
        "target_2k": args.target_2k,
        "target_2k_policy": TARGET_2K_POLICY[args.target_2k],
        "target_2k_child_decode_policy": TARGET_2K_CHILD_DECODE_POLICY[args.target_2k],
        "git_commit": git_commit(),
        "cli": str(args.cli),
        "cli_sha256": file_sha256(args.cli),
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "version": platform.version(),
            "machine": platform.machine(),
            "processor": platform.processor(),
        },
        "parent_decode_env": {
            "halfres_drop_l2_hp": os.environ.get("GPR_DECODE_HALFRES_DROP_L2_HP") == "1",
            "halfres_l2_mask": os.environ.get("GPR_DECODE_HALFRES_L2_MASK"),
            "halfres_stream": os.environ.get("GPR_DECODE_HALFRES_STREAM", "1") != "0",
        },
        "decode_mode": {
            "deprecated": True,
            "note": "Parent environment only. Named 2K targets set their decode policy inside fused_decode_cli; use target_2k_child_decode_policy.",
            "halfres_drop_l2_hp": os.environ.get("GPR_DECODE_HALFRES_DROP_L2_HP") == "1",
            "halfres_l2_mask": os.environ.get("GPR_DECODE_HALFRES_L2_MASK"),
            "halfres_stream": os.environ.get("GPR_DECODE_HALFRES_STREAM", "1") != "0",
        },
        "elapsed_s": time.time() - t0,
        "memory": {
            "parent_maxrss_kb": maxrss_kb(resource.RUSAGE_SELF),
            "children_maxrss_kb": maxrss_kb(resource.RUSAGE_CHILDREN),
        },
        "summary": {
            "4k_raw_1x": target_summary(rows, "4k_raw_1x"),
            args.target_2k: target_summary(rows, args.target_2k),
        },
        "rows": rows,
    }
    receipt = args.output_dir / "raw_resolution_targets_pi5_120f.json"
    receipt.write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps({"receipt": str(receipt), "summary": payload["summary"]}, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
