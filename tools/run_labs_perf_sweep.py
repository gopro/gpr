#!/usr/bin/env python3
"""Run a reproducible Labs target-performance sweep.

This is a thin orchestrator around ``tools/run_labs_target_bench.py``.  Each
variant gets its own receipt directory and the sweep writes a compact
``labs_perf_sweep.json`` that ranks the variants by median fps without turning
short probes into production claims.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[1]
TARGET_BENCH = REPO / "tools/run_labs_target_bench.py"

DEFAULT_VARIANTS: dict[str, dict[str, str]] = {
    "baseline": {},
    "stripe64_defer": {
        "FUSED_STRIPE_ROWS": "64",
        "FUSED_DEFER_RANS": "1",
    },
}


def parse_env_pair(text: str) -> tuple[str, str]:
    if "=" not in text:
        raise argparse.ArgumentTypeError(f"expected KEY=VALUE, got {text!r}")
    key, value = text.split("=", 1)
    if not key:
        raise argparse.ArgumentTypeError("environment key cannot be empty")
    return key, value


def parse_variant(text: str) -> tuple[str, dict[str, str]]:
    if ":" not in text:
        return text, {}
    name, env_text = text.split(":", 1)
    if not name:
        raise argparse.ArgumentTypeError("variant name cannot be empty")
    env: dict[str, str] = {}
    if env_text:
        for item in env_text.split(","):
            key, value = parse_env_pair(item)
            env[key] = value
    return name, env


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise TypeError(f"{path} must contain a JSON object")
    return data


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


def run_variant(args: argparse.Namespace, name: str, variant_env: dict[str, str]) -> dict[str, Any]:
    out_dir = args.output_dir / name
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable,
        str(TARGET_BENCH),
        "--frames",
        str(args.frames),
        "--output-dir",
        str(out_dir),
        "--target-fps",
        str(args.target_fps),
        "--source-width",
        str(args.source_width),
        "--source-height",
        str(args.source_height),
        "--capture-width",
        str(args.capture_width),
        "--capture-height",
        str(args.capture_height),
        "--quality",
        str(args.quality),
        "--wavelet-levels",
        str(args.wavelet_levels),
        "--pixel-format",
        str(args.pixel_format),
    ]
    if args.no_decimate:
        cmd.append("--no-decimate")
    else:
        cmd.extend(["--col-decimate", str(args.col_decimate), "--row-decimate", str(args.row_decimate)])
    if args.bench:
        cmd.extend(["--bench", str(args.bench)])
    if args.raw:
        cmd.extend(["--raw", str(args.raw)])
    if args.direct_gvid:
        cmd.append("--direct-gvid")
    if args.simulate:
        cmd.append("--simulate")
    cmd.extend([
        "--storage-target-name",
        args.storage_target_name,
        "--storage-target-read-mbps",
        str(args.storage_target_read_mbps),
        "--storage-target-write-mbps",
        str(args.storage_target_write_mbps),
        "--storage-target-safety-margin",
        str(args.storage_target_safety_margin),
    ])

    env = os.environ.copy()
    env.update(args.env)
    env.update(variant_env)

    result = subprocess.run(cmd, cwd=REPO, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    receipt_path = out_dir / "labs_target_bench.json"
    entry: dict[str, Any] = {
        "name": name,
        "env": variant_env,
        "receipt": str(receipt_path),
        "bench_exit_code": result.returncode,
        "completed": False,
        "stdout_tail": result.stdout[-2000:],
        "stderr_tail": result.stderr[-2000:],
    }
    if receipt_path.is_file():
        receipt = load_json(receipt_path)
        timing = receipt.get("timing", {})
        verdict = receipt.get("verdict", {})
        gvid = receipt.get("gvid", {})
        storage = receipt.get("storage", {})
        storage_target = storage.get("target", {}) if isinstance(storage, dict) else {}
        entry.update({
            "completed": True,
            "simulated": bool(receipt.get("simulated")),
            "repo_commit": receipt.get("repo_commit"),
            "fps_median": timing.get("fps_median") if isinstance(timing, dict) else None,
            "median_ms": timing.get("median_ms") if isinstance(timing, dict) else None,
            "p95_ms": timing.get("p95_ms") if isinstance(timing, dict) else None,
            "frames": timing.get("n") if isinstance(timing, dict) else None,
            "fps_target_met": verdict.get("fps_target_met") if isinstance(verdict, dict) else None,
            "storage_target_met": verdict.get("storage_target_met") if isinstance(verdict, dict) else None,
            "no_drops": verdict.get("no_drops") if isinstance(verdict, dict) else None,
            "gvid_valid": verdict.get("gvid_valid") if isinstance(verdict, dict) else None,
            "gvid_sha256": gvid.get("sha256") if isinstance(gvid, dict) else None,
            "storage_target_read_MBps": storage_target.get("target_read_MBps") if isinstance(storage_target, dict) else None,
            "storage_required_write_MBps": storage_target.get("required_write_MBps") if isinstance(storage_target, dict) else None,
            "storage_budget_write_MBps": storage_target.get("budget_write_MBps") if isinstance(storage_target, dict) else None,
            "storage_MiB_per_frame": storage_target.get("MiB_per_frame") if isinstance(storage_target, dict) else None,
        })
    return entry


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bench", type=Path, help="bench_fused binary on target")
    ap.add_argument("--raw", type=Path, help="source Bayer raw input on target")
    ap.add_argument("--output-dir", type=Path, default=Path(os.environ.get("GPR_ARTIFACT_ROOT", Path(os.environ.get("GPR_EXTERNAL_ROOT", Path(__file__).resolve().parents[1])) / "artifacts")) / "labs_perf_sweep")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--target-fps", type=float, default=24.0)
    ap.add_argument("--source-width", type=int, default=8280)
    ap.add_argument("--source-height", type=int, default=5520)
    ap.add_argument("--capture-width", type=int, default=8280)
    ap.add_argument("--capture-height", type=int, default=5520)
    ap.add_argument("--quality", type=int, default=3)
    ap.add_argument("--wavelet-levels", type=int, default=2, choices=(1, 2))
    ap.add_argument("--col-decimate", type=int, default=2)
    ap.add_argument("--row-decimate", type=int, default=2)
    ap.add_argument("--no-decimate", action="store_true")
    ap.add_argument("--pixel-format", type=int, default=4)
    ap.add_argument("--direct-gvid", action="store_true")
    ap.add_argument("--simulate", action="store_true")
    ap.add_argument("--storage-target-name", default="Lexar Professional SILVER PLUS SDXC/microSDXC UHS-I (128GB-1TB)")
    ap.add_argument("--storage-target-read-mbps", type=float, default=205.0)
    ap.add_argument("--storage-target-write-mbps", type=float, default=150.0)
    ap.add_argument("--storage-target-safety-margin", type=float, default=0.90)
    ap.add_argument("--env", type=parse_env_pair, action="append", default=[], metavar="KEY=VALUE")
    ap.add_argument(
        "--variant",
        type=parse_variant,
        action="append",
        help="Variant name or name:KEY=VALUE,KEY=VALUE. Defaults to baseline and stripe64_defer.",
    )
    args = ap.parse_args()
    args.env = dict(args.env)

    variants = dict(DEFAULT_VARIANTS)
    if args.variant:
        variants = {}
        for name, env in args.variant:
            variants[name] = env

    args.output_dir.mkdir(parents=True, exist_ok=True)
    entries = [run_variant(args, name, env) for name, env in variants.items()]
    ranked = sorted(
        entries,
        key=lambda item: float(item["fps_median"]) if isinstance(item.get("fps_median"), (int, float)) else -1.0,
        reverse=True,
    )
    sweep = {
        "schema": "gpr_labs_perf_sweep.v1",
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "repo_commit": git_commit(),
        "target_fps": args.target_fps,
        "frames_per_variant": args.frames,
        "direct_gvid": bool(args.direct_gvid),
        "simulated": bool(args.simulate),
        "target_shape": {
            "source_width": args.source_width,
            "source_height": args.source_height,
            "capture_width": args.capture_width,
            "capture_height": args.capture_height,
            "quality": args.quality,
            "wavelet_levels": args.wavelet_levels,
            "pixel_format": args.pixel_format,
            "no_decimate": bool(args.no_decimate),
            "col_decimate": None if args.no_decimate else args.col_decimate,
            "row_decimate": None if args.no_decimate else args.row_decimate,
        },
        "common_env": args.env,
        "storage_target": {
            "name": args.storage_target_name,
            "target_read_MBps": args.storage_target_read_mbps,
            "target_write_MBps": args.storage_target_write_mbps,
            "safety_margin": args.storage_target_safety_margin,
            "budget_read_MBps": args.storage_target_read_mbps * args.storage_target_safety_margin,
            "budget_write_MBps": args.storage_target_write_mbps * args.storage_target_safety_margin,
        },
        "variants": entries,
        "ranked_by_fps_median": [item["name"] for item in ranked],
        "best_variant": ranked[0]["name"] if ranked else None,
        "best_fps_median": ranked[0].get("fps_median") if ranked else None,
        "production_claim": False,
        "promotion_rule": "A sweep entry can promote only after a target receipt clears fps, no-drop, valid .gvid, recovery, timing, memory, and sustained-run requirements.",
    }
    out = args.output_dir / "labs_perf_sweep.json"
    out.write_text(json.dumps(sweep, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    failed = [entry for entry in entries if not entry.get("completed")]
    print(json.dumps({
        "sweep": str(out),
        "best_variant": sweep["best_variant"],
        "best_fps_median": sweep["best_fps_median"],
        "failed": [entry["name"] for entry in failed],
    }, indent=2, sort_keys=True))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
