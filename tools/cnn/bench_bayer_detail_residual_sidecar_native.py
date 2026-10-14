#!/usr/bin/env python3
"""Benchmark the native Bayer detail-residual sidecar encoder/decoder."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any


SCHEMA = "gpr.native_bayer_detail_residual_sidecar_thread_sweep.v1"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_threads(values: list[str]) -> list[int]:
    out: list[int] = []
    seen: set[int] = set()
    for value in values:
        for part in value.split(","):
            part = part.strip()
            if not part:
                continue
            threads = int(part)
            if threads < 1:
                raise ValueError("thread counts must be >= 1")
            if threads not in seen:
                out.append(threads)
                seen.add(threads)
    if not out:
        out.append(1)
    return out


def manifest_images(manifest: dict[str, Any], stems: set[str] | None) -> list[dict[str, Any]]:
    images = []
    for image in manifest.get("images", []):
        image_id = image.get("image_id") or image.get("image")
        if stems and image_id not in stems:
            continue
        images.append(image)
    if not images:
        raise ValueError("manifest produced no images")
    return images


def run_cmd(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True, check=True)


def mean(values: list[float]) -> float:
    return float(sum(values) / len(values)) if values else 0.0


def median(values: list[float]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[mid])
    return float((ordered[mid - 1] + ordered[mid]) / 2.0)


def benchmark(args: argparse.Namespace) -> dict[str, Any]:
    started = time.perf_counter()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    width = int(args.width or manifest.get("width12") or manifest.get("width"))
    height = int(args.height or manifest.get("height12") or manifest.get("height"))
    if width <= 0 or height <= 0:
        raise ValueError("width/height are required")
    images = manifest_images(manifest, set(args.stem or []) or None)
    threads = parse_threads(args.threads)

    if args.out_dir.exists():
        shutil.rmtree(args.out_dir)
    args.out_dir.mkdir(parents=True)

    rows: list[dict[str, Any]] = []
    for thread_count in threads:
        root = args.out_dir / f"threads_{thread_count}"
        sidecar_dir = root / "sidecar"
        decoded_dir = root / "decoded"
        receipt_dir = root / "receipts"
        sidecar_dir.mkdir(parents=True)
        decoded_dir.mkdir()
        receipt_dir.mkdir()
        for image in images:
            image_id = image.get("image_id") or image.get("image")
            codec = Path(image["low_source_raw"])
            clean = Path(image["low_clean_raw"])
            sidecar = sidecar_dir / f"{image_id}.bdrs"
            decoded = decoded_dir / f"{image_id}.raw"
            encode_json = receipt_dir / f"{image_id}.encode.json"
            decode_json = receipt_dir / f"{image_id}.decode.json"
            env = os.environ.copy()
            env["BDRS_ENCODE_THREADS"] = str(thread_count)
            if args.compact:
                env["BDRS_COMPACT"] = "1"
            run_cmd(
                [
                    str(args.tool),
                    "encode",
                    str(codec),
                    str(clean),
                    str(sidecar),
                    str(width),
                    str(height),
                    str(args.plane_mask),
                    str(args.significant_detail_threshold),
                    str(args.residual_threshold),
                    str(args.quant_step),
                    str(args.max_value),
                    str(encode_json),
                ],
                cwd=args.repo,
                env=env,
            )
            run_cmd(
                [
                    str(args.tool),
                    "decode",
                    str(codec),
                    str(sidecar),
                    str(decoded),
                    str(width),
                    str(height),
                    str(clean),
                    str(decode_json),
                ],
                cwd=args.repo,
            )
            encode_receipt = json.loads(encode_json.read_text(encoding="utf-8"))
            decode_receipt = json.loads(decode_json.read_text(encoding="utf-8"))
            rows.append(
                {
                    "threads": thread_count,
                    "image_id": image_id,
                    "sidecar_sha256": sha256(sidecar),
                    "sidecar_bytes": sidecar.stat().st_size,
                    "sidecar_format": encode_receipt.get("sidecar_format", "bitmap_i16"),
                    "value_payload_bytes": int(encode_receipt.get("value_payload_bytes", 0)),
                    "encode_ms": float(encode_receipt["elapsed_ms"]),
                    "decode_ms": float(decode_receipt["elapsed_ms"]),
                    "encode_threads_receipt": encode_receipt.get("encode_threads"),
                    "codec_clean_rmse": float(decode_receipt.get("codec_clean_rmse", 0.0)),
                    "output_clean_rmse": float(decode_receipt.get("output_clean_rmse", 0.0)),
                }
            )
            if not args.keep_decoded:
                decoded.unlink(missing_ok=True)

    baseline = {row["image_id"]: row for row in rows if row["threads"] == threads[0]}
    for row in rows:
        row["matches_baseline_sidecar"] = row["sidecar_sha256"] == baseline[row["image_id"]]["sidecar_sha256"]

    summary_rows: list[dict[str, Any]] = []
    for thread_count in threads:
        subset = [row for row in rows if row["threads"] == thread_count]
        summary_rows.append(
            {
                "threads": thread_count,
                "mean_encode_ms": mean([row["encode_ms"] for row in subset]),
                "median_encode_ms": median([row["encode_ms"] for row in subset]),
                "max_encode_ms": max([row["encode_ms"] for row in subset], default=0.0),
                "mean_decode_ms": mean([row["decode_ms"] for row in subset]),
                "mean_sidecar_mib": mean([row["sidecar_bytes"] for row in subset]) / (1024 * 1024),
                "mean_value_payload_mib": mean([row["value_payload_bytes"] for row in subset]) / (1024 * 1024),
                "sidecar_formats": sorted({str(row["sidecar_format"]) for row in subset}),
                "all_match_baseline_sidecar": all(row["matches_baseline_sidecar"] for row in subset),
            }
        )

    if not args.keep_sidecars:
        for sidecar_dir in args.out_dir.glob("threads_*/sidecar"):
            shutil.rmtree(sidecar_dir, ignore_errors=True)

    return {
        "schema": SCHEMA,
        "elapsed_s": time.perf_counter() - started,
        "note": "Native BDRS sidecar thread sweep. Sidecar payloads may be removed after hashing unless keep_sidecars is true.",
        "tool": str(args.tool),
        "tool_sha256": sha256(args.tool),
        "manifest": str(args.manifest),
        "width": width,
        "height": height,
        "params": {
            "plane_mask": args.plane_mask,
            "significant_detail_threshold": args.significant_detail_threshold,
            "residual_threshold": args.residual_threshold,
            "quant_step": args.quant_step,
            "max_value": args.max_value,
            "compact": args.compact,
        },
        "threads": threads,
        "keep_sidecars": args.keep_sidecars,
        "keep_decoded": args.keep_decoded,
        "summary_rows": summary_rows,
        "rows": rows,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--tool", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument("--threads", action="append", default=[])
    ap.add_argument("--stem", action="append")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--plane-mask", type=int, default=15)
    ap.add_argument("--significant-detail-threshold", type=int, default=2)
    ap.add_argument("--residual-threshold", type=int, default=1)
    ap.add_argument("--quant-step", type=int, default=2)
    ap.add_argument("--max-value", type=int, default=65535)
    ap.add_argument("--keep-sidecars", action="store_true")
    ap.add_argument("--keep-decoded", action="store_true")
    ap.add_argument("--compact", action="store_true", help="set BDRS_COMPACT=1 for compact varint sidecars")
    args = ap.parse_args()

    payload = benchmark(args)
    out_json = args.out_dir / "summary.json"
    out_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"out_json": str(out_json), "summary_rows": payload["summary_rows"]}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
