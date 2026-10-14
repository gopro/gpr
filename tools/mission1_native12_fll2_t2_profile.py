#!/usr/bin/env python3
"""Mission 1 native-12MP FLL2 per-band threshold profile helpers.

This is the reproducibility contract for the first true Bayer recompression
path that clears the active 20+ fps Mission 1 floor on the Pi 5 stand-in.
It does not claim strict 24 fps.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any


PROFILE_ID = "mission1_native12_fll2_t233_avg7555_fast_pinp2_20fps_v1"
TARGET_PROFILE_IDS = {
    PROFILE_ID,
    "mission1_native12_fll2_t233_avg7555_fast_pinp2_llcache_20fps_v1",
}
REQUIRED_IMAGES = ("GP017601", "GP017602", "GP017603")

ENV: dict[str, str] = {
    "FUSED_PIN": "1",
    "FUSED_PIN_P2": "1",
    "GPR_INCLUDE_LL": "1",
    "FUSED_RAW_LL": "1",
    "FUSED_LL_PREDICT": "1",
    "FUSED_LL_PREDICTOR": "avg",
    "FUSED_LL_RICE_KS": "7,5,5,5",
    "FUSED_LL_RICE_FAST": "1",
    "FUSED_LL_ASSUME_U16": "1",
    "FUSED_INLINE_TOKENIZE": "1",
    "FUSED_DEFER_RANS": "1",
    "GPR_BENCH_GVID_SCATTER": "1",
    "FUSED_REFERENCE_HORIZONTAL": "1",
    "FUSED_STRIPE_ROWS": "384",
    "GPR_INLINE_DENOISE_HARD": "1",
    "GPR_INLINE_DENOISE_T_LH": "2",
    "GPR_INLINE_DENOISE_T_HL": "3",
    "GPR_INLINE_DENOISE_T_HH": "3",
}

BENCH_ARGS: dict[str, Any] = {
    "frames": 1440,
    "target_fps": 20.0,
    "source_width": 4096,
    "source_height": 3072,
    "capture_width": 4096,
    "capture_height": 3072,
    "quality": 8,
    "wavelet_levels": 1,
    "no_decimate": True,
    "pixel_format": 1,
    "direct_gvid": True,
    "storage_target_name": "Lexar Professional SILVER PLUS SDXC/microSDXC UHS-I (128GB-1TB)",
    "storage_target_read_mbps": 205.0,
    "storage_target_write_mbps": 150.0,
    "storage_target_safety_margin": 0.90,
    "storage_target_note": "Published 128GB-1TB SILVER PLUS profile: 205 MB/s read, 150 MB/s write; 64GB microSD is 205/100.",
}

QUALITY_THRESHOLDS: dict[str, float] = {
    "min_psnr14_db": 75.0,
    "min_ssim": 0.99999,
    "max_required_write_mbps_at_20fps": 135.0,
}

LOCAL_ROUNDTRIP_EXPECTED: dict[str, dict[str, float | int]] = {
    "GP017601": {"encoded_bytes": 5471604, "psnr14_db": 84.47},
    "GP017602": {"encoded_bytes": 5571400, "psnr14_db": 85.14},
    "GP017603": {"encoded_bytes": 5200413, "psnr14_db": 75.35},
}

ENCODE_RE = re.compile(r"ENCODE:\s+(?P<bytes>\d+)\s+bytes")
DECODE_RE = re.compile(r"DECODE:\s+(?P<width>\d+)x(?P<height>\d+)")
PSNR_RE = re.compile(r"PSNR14\s+\(full-res\):\s+(?P<psnr>[0-9.]+)\s+dB\s+mse=(?P<mse>[0-9.]+)")
QUALITY_PSNR_RE = re.compile(r"^\s*PSNR:\s+(?P<psnr>[0-9.]+)\s+dB", re.MULTILINE)
QUALITY_SSIM_RE = re.compile(r"^\s*SSIM:\s+(?P<ssim>[0-9.]+)", re.MULTILINE)
QUALITY_RMSE_RE = re.compile(r"^\s*RMSE:\s+(?P<rmse>[0-9.]+)\s+DN", re.MULTILINE)
QUALITY_MAX_ERROR_RE = re.compile(r"^\s*Max error:\s+(?P<maxerr>[0-9]+)\s+DN", re.MULTILINE)


def profile_payload() -> dict[str, Any]:
    return {
        "schema": "gpr_mission1_native12_profile.v1",
        "profile_id": PROFILE_ID,
        "description": "True Mission 1 native-12MP Bayer recompression using q8 FLL2 exact-LL and hard per-band highpass dead-zones LH/HL/HH=2/3/3.",
        "production_scope": "Pi 5 / Mission 1 stand-in 20+ fps; strict 24 fps remains open.",
        "required_images": list(REQUIRED_IMAGES),
        "env": ENV,
        "bench_args": BENCH_ARGS,
        "quality_thresholds": QUALITY_THRESHOLDS,
    }


def profile_env(stripe_rows: int | None = None) -> dict[str, str]:
    env = os.environ.copy()
    env.update(ENV)
    if stripe_rows is not None:
        env["FUSED_STRIPE_ROWS"] = str(stripe_rows)
    env.update({
        "GPR_BENCH_PIXEL_FORMAT": str(BENCH_ARGS["pixel_format"]),
        "FUSED_QUALITY": str(BENCH_ARGS["quality"]),
    })
    return env


def effective_profile_env(env: dict[str, str]) -> dict[str, str]:
    keys = sorted(set(ENV) | {"GPR_BENCH_PIXEL_FORMAT", "FUSED_QUALITY"})
    return {key: env[key] for key in keys if key in env}


def quote_cmd(items: list[str]) -> str:
    return " ".join(shlex.quote(item) for item in items)


def bench_command(args: argparse.Namespace) -> str:
    cmd: list[str] = []
    if args.tmpdir:
        cmd.append(f"TMPDIR={shlex.quote(str(args.tmpdir))}")
    for key, value in ENV.items():
        cmd.append(f"{key}={shlex.quote(value)}")

    bench_args = [
        "python3",
        "tools/run_labs_target_bench.py",
        "--bench", str(args.bench),
        "--raw", str(args.raw),
        "--output-dir", str(args.output_dir),
        "--frames", str(args.frames),
        "--target-fps", str(args.target_fps),
        "--source-width", str(BENCH_ARGS["source_width"]),
        "--source-height", str(BENCH_ARGS["source_height"]),
        "--capture-width", str(BENCH_ARGS["capture_width"]),
        "--capture-height", str(BENCH_ARGS["capture_height"]),
        "--quality", str(BENCH_ARGS["quality"]),
        "--wavelet-levels", str(BENCH_ARGS["wavelet_levels"]),
        "--no-decimate",
        "--pixel-format", str(BENCH_ARGS["pixel_format"]),
        "--storage-target-name", str(BENCH_ARGS["storage_target_name"]),
        "--storage-target-read-mbps", str(BENCH_ARGS["storage_target_read_mbps"]),
        "--storage-target-write-mbps", str(BENCH_ARGS["storage_target_write_mbps"]),
        "--storage-target-safety-margin", str(BENCH_ARGS["storage_target_safety_margin"]),
        "--storage-target-note", str(BENCH_ARGS["storage_target_note"]),
        "--direct-gvid",
    ]
    command = quote_cmd(bench_args)
    if args.cleanup_payloads:
        cleanup_targets = [
            args.output_dir / "capture.gvid",
            args.output_dir / "capture_interrupted_tail.gvid",
            args.output_dir / "frames",
        ]
        cleanup_cmd = "rm -rf " + " ".join(shlex.quote(str(path)) for path in cleanup_targets)
        command = f"{command}; _gpr_bench_rc=$?; {cleanup_cmd}; exit $_gpr_bench_rc"
    cmd.append(command)
    return " ".join(cmd)


def load_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"{path} must contain a JSON object")
    return data


def rows_by_image(summary: dict[str, Any], path: Path) -> dict[str, dict[str, Any]]:
    rows = summary.get("rows")
    if rows is None:
        rows = summary.get("results")
    if not isinstance(rows, list):
        raise ValueError(f"{path}: rows/results must be a list")
    out: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError(f"{path}: rows/results entries must be objects")
        image = row.get("image")
        if not isinstance(image, str) or not image:
            raise ValueError(f"{path}: every row needs image")
        out[image] = row
    return out


def validate_quality_summary(summary: dict[str, Any], label: str, failures: list[str]) -> None:
    if summary.get("schema") != "mission1_native12_current_profile_quality.v1":
        failures.append(f"{label}: schema must be mission1_native12_current_profile_quality.v1")
    if summary.get("profile_id") != PROFILE_ID:
        failures.append(f"{label}: profile_id must be {PROFILE_ID}")
    rows = rows_by_image(summary, Path(label))
    missing = sorted(set(REQUIRED_IMAGES) - set(rows))
    if missing:
        failures.append(f"{label}: missing quality rows {', '.join(missing)}")
    if summary.get("passes_20fps_storage_budget_all") is not True:
        failures.append(f"{label}: passes_20fps_storage_budget_all must be true")
    for image in REQUIRED_IMAGES:
        row = rows.get(image)
        if not row:
            continue
        psnr = float(row.get("PSNR14_dB", -1.0))
        ssim = float(row.get("SSIM", -1.0))
        write = float(row.get("required_MBps_at_20fps", 1e9))
        if psnr < QUALITY_THRESHOLDS["min_psnr14_db"]:
            failures.append(f"{label}: {image} PSNR14 {psnr:.2f} below {QUALITY_THRESHOLDS['min_psnr14_db']:.2f}")
        if ssim < QUALITY_THRESHOLDS["min_ssim"]:
            failures.append(f"{label}: {image} SSIM {ssim:.6f} below {QUALITY_THRESHOLDS['min_ssim']:.6f}")
        if write > QUALITY_THRESHOLDS["max_required_write_mbps_at_20fps"]:
            failures.append(f"{label}: {image} requires {write:.1f} MB/s at 20 fps")
        if int(row.get("gpr_bytes", 0)) <= 0:
            failures.append(f"{label}: {image} gpr_bytes must be positive")


def validate_quality(path: Path, failures: list[str]) -> None:
    validate_quality_summary(load_json(path), str(path), failures)


def validate_target(path: Path, failures: list[str]) -> None:
    summary = load_json(path)
    profile_id = summary.get("profile_id")
    if profile_id not in TARGET_PROFILE_IDS:
        failures.append(f"{path}: profile_id must be one of {', '.join(sorted(TARGET_PROFILE_IDS))}")
    target_fps = float(summary.get("target_fps", BENCH_ARGS["target_fps"]))
    rows = rows_by_image(summary, path)
    missing = sorted(set(REQUIRED_IMAGES) - set(rows))
    if missing:
        failures.append(f"{path}: missing target rows {', '.join(missing)}")
    if "all_pass" in summary and summary.get("all_pass") is not True:
        failures.append(f"{path}: all_pass must be true")
    for image in REQUIRED_IMAGES:
        row = rows.get(image)
        if not row:
            continue
        row_profile_id = row.get("profile_id")
        if row_profile_id is not None and row_profile_id not in TARGET_PROFILE_IDS:
            failures.append(f"{path}: {image} profile_id {row_profile_id!r} is not accepted")
        verdict = row.get("verdict")
        if not isinstance(verdict, dict):
            failures.append(f"{path}: {image} verdict must be an object")
            continue
        for key in ("fps_target_met", "storage_target_met", "gvid_valid", "no_drops", "interruption_recovery_proven", "target_evidence"):
            if verdict.get(key) is not True:
                failures.append(f"{path}: {image} verdict.{key} must be true")
        fps = float(row.get("fps_median", 0.0))
        write = float(row.get("required_write_MBps", 1e9))
        budget = float(row.get("budget_write_MBps", 0.0))
        if fps < target_fps:
            failures.append(f"{path}: {image} fps_median {fps:.2f} below {target_fps:.2f}")
        if write > budget:
            failures.append(f"{path}: {image} required write {write:.1f} MB/s above budget {budget:.1f} MB/s")


def image_receipt_pair(text: str) -> tuple[str, Path]:
    if "=" not in text:
        raise argparse.ArgumentTypeError("receipt must be IMAGE=/path/to/labs_target_bench.json")
    image, path = text.split("=", 1)
    if image not in REQUIRED_IMAGES:
        raise argparse.ArgumentTypeError(f"image must be one of {', '.join(REQUIRED_IMAGES)}")
    return image, Path(path)


def row_from_labs_receipt(image: str, path: Path, profile_id: str) -> dict[str, Any]:
    receipt = load_json(path)
    if receipt.get("schema") != "gpr_labs_target_bench.v1":
        raise ValueError(f"{path}: expected gpr_labs_target_bench.v1")
    timing = receipt.get("timing")
    storage = receipt.get("storage")
    verdict = receipt.get("verdict")
    if not isinstance(timing, dict) or not isinstance(storage, dict) or not isinstance(verdict, dict):
        raise ValueError(f"{path}: missing timing/storage/verdict objects")
    target = storage.get("target")
    if not isinstance(target, dict):
        raise ValueError(f"{path}: missing storage.target object")
    phase = receipt.get("bench_phase_timing", {}).get("phase_ms", {})
    encode = phase.get("encode", {}) if isinstance(phase, dict) else {}
    write = phase.get("write", {}) if isinstance(phase, dict) else {}
    return {
        "image": image,
        "profile_id": profile_id,
        "verdict": verdict,
        "fps_median": float(timing.get("fps_median", 0.0)),
        "median_ms": float(timing.get("median_ms", 0.0)),
        "p95_ms": float(timing.get("p95_ms", 0.0)),
        "max_ms": float(timing.get("max_ms", 0.0)),
        "required_write_MBps": float(target.get("required_write_MBps", 0.0)),
        "budget_write_MBps": float(target.get("budget_write_MBps", 0.0)),
        "MiB_per_frame": float(target.get("MiB_per_frame", 0.0)),
        "gvid_bytes": int(storage.get("gvid_bytes", 0)),
        "encode_median_ms": float(encode.get("median_ms", 0.0)) if isinstance(encode, dict) else 0.0,
        "write_median_ms": float(write.get("median_ms", 0.0)) if isinstance(write, dict) else 0.0,
        "receipt": str(path),
    }


def target_fps_from_labs_receipt(path: Path) -> float:
    receipt = load_json(path)
    target = receipt.get("target")
    if not isinstance(target, dict):
        raise ValueError(f"{path}: missing target object")
    return float(target.get("fps", BENCH_ARGS["target_fps"]))


def cmd_summarize_target(args: argparse.Namespace) -> int:
    rows: list[dict[str, Any]] = []
    failures: list[str] = []
    seen: set[str] = set()
    target_fps_values: list[float] = []
    for image, path in args.receipt:
        if image in seen:
            failures.append(f"duplicate receipt for {image}")
            continue
        seen.add(image)
        try:
            target_fps_values.append(target_fps_from_labs_receipt(path))
            rows.append(row_from_labs_receipt(image, path, args.profile_id))
        except Exception as exc:
            failures.append(str(exc))
    target_fps = target_fps_values[0] if target_fps_values else args.target_fps
    for value in target_fps_values:
        if abs(value - target_fps) > 1.0e-6:
            failures.append(f"mixed target fps values: {target_fps_values}")
    if args.require_all:
        missing = sorted(set(REQUIRED_IMAGES) - seen)
        if missing:
            failures.append(f"missing receipts for {', '.join(missing)}")
    summary = {
        "schema": "mission1_fll2_T233_native12_target_summary.v1",
        "profile_id": args.profile_id,
        "target_fps": target_fps,
        "all_pass": not failures and all(
            row["verdict"].get(key) is True
            for row in rows
            for key in ("fps_target_met", "storage_target_met", "gvid_valid", "no_drops", "interruption_recovery_proven", "target_evidence")
        ),
        "rows": rows,
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    else:
        print(json.dumps(summary, indent=2, sort_keys=True))
    if failures:
        print("Mission 1 target summary failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    print("Mission 1 target summary OK", file=sys.stderr)
    return 0


def cmd_describe(args: argparse.Namespace) -> int:
    payload = profile_payload()
    if args.format == "json":
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        for key, value in ENV.items():
            print(f"export {key}={shlex.quote(value)}")
    return 0


def cmd_command(args: argparse.Namespace) -> int:
    print(bench_command(args))
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    failures: list[str] = []
    validate_quality(args.quality_summary, failures)
    validate_target(args.target_summary, failures)
    if failures:
        print("Mission 1 FLL2 T233 profile validation failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    print("Mission 1 FLL2 T233 profile validation OK")
    return 0


def validate_pending_manifest(path: Path, failures: list[str]) -> None:
    manifest = load_json(path)
    if manifest.get("schema") != "mission1_pending_pi_probe_manifest.v1":
        failures.append(f"{path}: unexpected schema {manifest.get('schema')!r}")
    if manifest.get("status") != "pending_pi_ssh":
        failures.append(f"{path}: status must be pending_pi_ssh before target receipts exist")
    cleanup = manifest.get("cleanup_policy")
    if not isinstance(cleanup, dict) or cleanup.get("enabled") is not True:
        failures.append(f"{path}: cleanup_policy.enabled must be true")
    else:
        after_success = cleanup.get("after_success")
        preserve = cleanup.get("preserve")
        for required in ("capture.gvid", "capture_interrupted_tail.gvid", "frames/"):
            if not isinstance(after_success, list) or required not in after_success:
                failures.append(f"{path}: cleanup_policy.after_success missing {required}")
        if not isinstance(preserve, list) or "labs_target_bench.json" not in preserve:
            failures.append(f"{path}: cleanup_policy.preserve must include labs_target_bench.json")

    commands = manifest.get("commands")
    if not isinstance(commands, list) or not commands:
        failures.append(f"{path}: commands must be a non-empty list")
        return

    root = path.parent
    for idx, command in enumerate(commands):
        if not isinstance(command, dict):
            failures.append(f"{path}: commands[{idx}] must be an object")
            continue
        rel_file = command.get("file")
        if not isinstance(rel_file, str) or not rel_file.endswith(".sh"):
            failures.append(f"{path}: commands[{idx}].file must name a shell script")
            continue
        script = root / rel_file
        if not script.is_file():
            failures.append(f"{path}: missing script {script}")
            continue
        digest = hashlib.sha256(script.read_bytes()).hexdigest()
        expected_digest = command.get("sha256")
        if digest != expected_digest:
            failures.append(f"{path}: {rel_file} sha256 {digest} != {expected_digest}")
        text = script.read_text(encoding="utf-8")
        frames = command.get("frames")
        target_fps = command.get("target_fps")
        output_dir = command.get("output_dir")
        if f"--frames {frames}" not in text:
            failures.append(f"{path}: {rel_file} missing --frames {frames}")
        if f"--target-fps {float(target_fps):.1f}" not in text:
            failures.append(f"{path}: {rel_file} missing --target-fps {float(target_fps):.1f}")
        if not isinstance(output_dir, str) or output_dir not in text:
            failures.append(f"{path}: {rel_file} missing output_dir {output_dir}")
        for suffix in ("capture.gvid", "capture_interrupted_tail.gvid", "frames"):
            if not isinstance(output_dir, str) or f"{output_dir}/{suffix}" not in text:
                failures.append(f"{path}: {rel_file} missing cleanup target {suffix}")
        for token in ("FUSED_LL_RICE_KS=7,5,5,5", "--direct-gvid", "rm -rf"):
            if token not in text:
                failures.append(f"{path}: {rel_file} missing {token}")


def cmd_validate_pending(args: argparse.Namespace) -> int:
    failures: list[str] = []
    validate_pending_manifest(args.manifest, failures)
    if failures:
        print("Mission 1 pending Pi manifest validation failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    print("Mission 1 pending Pi manifest validation OK")
    return 0


def parse_roundtrip_output(text: str, image: str) -> dict[str, Any]:
    enc = ENCODE_RE.search(text)
    dec = DECODE_RE.search(text)
    psnr = PSNR_RE.search(text)
    if not enc or not dec or not psnr:
        raise ValueError(f"{image}: could not parse encode/decode/PSNR from roundtrip output")
    return {
        "image": image,
        "encoded_bytes": int(enc.group("bytes")),
        "decoded_width": int(dec.group("width")),
        "decoded_height": int(dec.group("height")),
        "psnr14_db": float(psnr.group("psnr")),
        "mse": float(psnr.group("mse")),
    }


def parse_compare_quality_output(text: str, image: str) -> dict[str, Any]:
    psnr = QUALITY_PSNR_RE.search(text)
    ssim = QUALITY_SSIM_RE.search(text)
    rmse = QUALITY_RMSE_RE.search(text)
    maxerr = QUALITY_MAX_ERROR_RE.search(text)
    if not psnr or not ssim or not rmse or not maxerr:
        raise ValueError(f"{image}: could not parse compare_quality output")
    return {
        "PSNR14_dB": float(psnr.group("psnr")),
        "SSIM": float(ssim.group("ssim")),
        "RMSE_DN": float(rmse.group("rmse")),
        "max_abs_error": int(maxerr.group("maxerr")),
    }


def validate_roundtrip_row(row: dict[str, Any], failures: list[str]) -> None:
    image = str(row["image"])
    expected = LOCAL_ROUNDTRIP_EXPECTED[image]
    if row["encoded_bytes"] != expected["encoded_bytes"]:
        failures.append(f"{image}: encoded_bytes {row['encoded_bytes']} != {expected['encoded_bytes']}")
    if row["decoded_width"] != BENCH_ARGS["source_width"] or row["decoded_height"] != BENCH_ARGS["source_height"]:
        failures.append(
            f"{image}: decoded dimensions {row['decoded_width']}x{row['decoded_height']} "
            f"!= {BENCH_ARGS['source_width']}x{BENCH_ARGS['source_height']}"
        )
    if row["psnr14_db"] + 1.0e-6 < float(expected["psnr14_db"]):
        failures.append(f"{image}: PSNR14 {row['psnr14_db']:.2f} below expected {float(expected['psnr14_db']):.2f}")


def cmd_validate_local(args: argparse.Namespace) -> int:
    failures: list[str] = []
    if not args.roundtrip.is_file():
        failures.append(f"roundtrip binary missing: {args.roundtrip}")
    if not args.raw_dir.is_dir():
        failures.append(f"raw directory missing: {args.raw_dir}")
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    tmpdir = args.tmpdir or args.raw_dir
    tmpdir.mkdir(parents=True, exist_ok=True)
    env = profile_env(args.stripe_rows)

    rows: list[dict[str, Any]] = []
    for image in REQUIRED_IMAGES:
        raw = args.raw_dir / f"{image}.raw"
        if not raw.is_file():
            failures.append(f"{image}: missing raw {raw}")
            continue
        decoded = tmpdir / f"{image}.validate_local.decoded.raw"
        cmd = [
            str(args.roundtrip),
            str(raw),
            str(BENCH_ARGS["source_width"]),
            str(BENCH_ARGS["source_height"]),
            str(decoded),
        ]
        result = subprocess.run(cmd, env=env, text=True, capture_output=True, check=False)
        decoded.unlink(missing_ok=True)
        if result.returncode != 0:
            failures.append(f"{image}: roundtrip returned {result.returncode}: {result.stderr[-1000:]}")
            continue
        try:
            row = parse_roundtrip_output(result.stdout + "\n" + result.stderr, image)
        except ValueError as exc:
            failures.append(str(exc))
            continue
        validate_roundtrip_row(row, failures)
        rows.append(row)

    summary = {
        "schema": "mission1_native12_local_roundtrip.v1",
        "profile_id": PROFILE_ID,
        "profile_env": effective_profile_env(env),
        "roundtrip": str(args.roundtrip),
        "raw_dir": str(args.raw_dir),
        "rows": rows,
        "all_pass": not failures and len(rows) == len(REQUIRED_IMAGES),
    }
    if args.output_summary:
        args.output_summary.parent.mkdir(parents=True, exist_ok=True)
        args.output_summary.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if failures:
        print("Mission 1 local roundtrip validation failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    print("Mission 1 local roundtrip validation OK")
    return 0


def cmd_quality_local(args: argparse.Namespace) -> int:
    failures: list[str] = []
    if not args.roundtrip.is_file():
        failures.append(f"roundtrip binary missing: {args.roundtrip}")
    if not args.compare_quality.is_file():
        failures.append(f"compare_quality binary missing: {args.compare_quality}")
    if not args.raw_dir.is_dir():
        failures.append(f"raw directory missing: {args.raw_dir}")
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    tmpdir = args.tmpdir or args.raw_dir
    tmpdir.mkdir(parents=True, exist_ok=True)
    env = profile_env(args.stripe_rows)

    rows: list[dict[str, Any]] = []
    for image in REQUIRED_IMAGES:
        raw = args.raw_dir / f"{image}.raw"
        if not raw.is_file():
            failures.append(f"{image}: missing raw {raw}")
            continue
        decoded = tmpdir / f"{image}.quality_local.decoded.raw"
        roundtrip_cmd = [
            str(args.roundtrip),
            str(raw),
            str(BENCH_ARGS["source_width"]),
            str(BENCH_ARGS["source_height"]),
            str(decoded),
        ]
        rt = subprocess.run(roundtrip_cmd, env=env, text=True, capture_output=True, check=False)
        if rt.returncode != 0:
            decoded.unlink(missing_ok=True)
            failures.append(f"{image}: roundtrip returned {rt.returncode}: {rt.stderr[-1000:]}")
            continue
        try:
            rt_row = parse_roundtrip_output(rt.stdout + "\n" + rt.stderr, image)
        except ValueError as exc:
            decoded.unlink(missing_ok=True)
            failures.append(str(exc))
            continue

        compare_cmd = [
            str(args.compare_quality),
            str(raw),
            str(decoded),
            str(BENCH_ARGS["source_width"]),
            str(BENCH_ARGS["source_height"]),
            "14",
        ]
        cq = subprocess.run(compare_cmd, text=True, capture_output=True, check=False)
        if not args.keep_decoded:
            decoded.unlink(missing_ok=True)
        if cq.returncode != 0:
            failures.append(f"{image}: compare_quality returned {cq.returncode}: {cq.stderr[-1000:]}")
            continue
        try:
            q_row = parse_compare_quality_output(cq.stdout + "\n" + cq.stderr, image)
        except ValueError as exc:
            failures.append(str(exc))
            continue

        row = {
            "image": image,
            "gpr_bytes": int(rt_row["encoded_bytes"]),
            "MiB": float(rt_row["encoded_bytes"]) / (1024.0 * 1024.0),
            "required_MBps_at_20fps": float(rt_row["encoded_bytes"]) * 20.0 / 1_000_000.0,
            "required_MBps_at_24fps": float(rt_row["encoded_bytes"]) * 24.0 / 1_000_000.0,
            **q_row,
            "roundtrip_psnr14_db": rt_row["psnr14_db"],
            "roundtrip_mse": rt_row["mse"],
            "decoded_width": rt_row["decoded_width"],
            "decoded_height": rt_row["decoded_height"],
        }
        rows.append(row)

    quality_failures: list[str] = []
    validate_quality_summary({
        "schema": "mission1_native12_current_profile_quality.v1",
        "profile_id": PROFILE_ID,
        "passes_20fps_storage_budget_all": True,
        "rows": rows,
    }, "generated quality-local summary", quality_failures)
    failures.extend(quality_failures)

    summary = {
        "schema": "mission1_native12_current_profile_quality.v1",
        "profile_id": PROFILE_ID,
        "profile_env": effective_profile_env(env),
        "roundtrip": str(args.roundtrip),
        "compare_quality": str(args.compare_quality),
        "raw_dir": str(args.raw_dir),
        "rows": rows,
        "passes_20fps_storage_budget_all": all(
            float(row["required_MBps_at_20fps"]) <= QUALITY_THRESHOLDS["max_required_write_mbps_at_20fps"]
            for row in rows
        ) and len(rows) == len(REQUIRED_IMAGES),
        "all_pass": not failures and len(rows) == len(REQUIRED_IMAGES),
    }
    if args.output_summary:
        args.output_summary.parent.mkdir(parents=True, exist_ok=True)
        args.output_summary.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if failures:
        print("Mission 1 local quality validation failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    print("Mission 1 local quality validation OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    describe = sub.add_parser("describe", help="print profile JSON or shell env")
    describe.add_argument("--format", choices=("json", "shell"), default="json")
    describe.set_defaults(func=cmd_describe)

    command = sub.add_parser("command", help="print a run_labs_target_bench command for one raw")
    command.add_argument("--bench", type=Path, required=True)
    command.add_argument("--raw", type=Path, required=True)
    command.add_argument("--output-dir", type=Path, required=True)
    command.add_argument("--tmpdir", type=Path)
    command.add_argument("--frames", type=int, default=int(BENCH_ARGS["frames"]))
    command.add_argument("--target-fps", type=float, default=float(BENCH_ARGS["target_fps"]),
                         help="target FPS for the emitted bench command; default preserves the committed 20fps profile")
    command.add_argument("--cleanup-payloads", action="store_true",
                         help="append cleanup for large .gvid/frame payloads after the JSON receipt is written")
    command.set_defaults(func=cmd_command)

    validate = sub.add_parser("validate", help="validate compact quality and Pi target summaries")
    validate.add_argument("--quality-summary", type=Path, required=True)
    validate.add_argument("--target-summary", type=Path, required=True)
    validate.set_defaults(func=cmd_validate)

    summarize = sub.add_parser("summarize-target", help="convert labs_target_bench receipts into a compact target summary")
    summarize.add_argument("--receipt", action="append", type=image_receipt_pair, required=True,
                           help="IMAGE=/path/to/labs_target_bench.json; repeat for each image")
    summarize.add_argument("--output", type=Path, help="write compact summary JSON here; stdout if omitted")
    summarize.add_argument("--profile-id", default=PROFILE_ID)
    summarize.add_argument("--target-fps", type=float, default=float(BENCH_ARGS["target_fps"]),
                           help="fallback target FPS when a receipt omits target.fps")
    summarize.add_argument("--require-all", action="store_true", help="require all Mission 1 required images")
    summarize.set_defaults(func=cmd_summarize_target)

    pending = sub.add_parser("validate-pending", help="validate a pending Pi command manifest")
    pending.add_argument("--manifest", type=Path, required=True)
    pending.set_defaults(func=cmd_validate_pending)

    local = sub.add_parser("validate-local", help="run local Mission 1 roundtrip checks for required raws")
    local.add_argument("--roundtrip", type=Path, required=True, help="path to build-local/bin/test_fused_roundtrip")
    local.add_argument("--raw-dir", type=Path, required=True, help="directory containing GP017601/602/603.raw")
    local.add_argument("--tmpdir", type=Path, help="directory for decoded scratch output")
    local.add_argument("--output-summary", type=Path, help="optional compact JSON receipt path")
    local.add_argument("--stripe-rows", type=int, help="override FUSED_STRIPE_ROWS for diagnostic receipts")
    local.set_defaults(func=cmd_validate_local)

    quality = sub.add_parser("quality-local", help="run local exact-profile quality checks with compare_quality")
    quality.add_argument("--roundtrip", type=Path, required=True, help="path to build-local/bin/test_fused_roundtrip")
    quality.add_argument("--compare-quality", type=Path, required=True, help="path to compare_quality binary")
    quality.add_argument("--raw-dir", type=Path, required=True, help="directory containing GP017601/602/603.raw")
    quality.add_argument("--tmpdir", type=Path, help="directory for decoded scratch output")
    quality.add_argument("--output-summary", type=Path, help="optional compact JSON receipt path")
    quality.add_argument("--stripe-rows", type=int, help="override FUSED_STRIPE_ROWS for diagnostic receipts")
    quality.add_argument("--keep-decoded", action="store_true", help="retain decoded raw files for visual/debug inspection")
    quality.set_defaults(func=cmd_quality_local)

    args = ap.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
