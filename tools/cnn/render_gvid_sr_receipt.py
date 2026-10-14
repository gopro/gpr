#!/usr/bin/env python3
"""Render a compact .gvid -> decoded Bayer -> 2x SR receipt.

The tool is intentionally receipt-oriented: it loads the SR checkpoint once,
extracts selected .gvid frame payloads, decodes each payload with the existing
fused decoder CLI, runs the 2x Bayer SR model, optionally writes the SR raw
sequence, and records per-frame timing. Temporary .gpr and decoded raw files
are removed after each frame.
"""
from __future__ import annotations


import argparse
import hashlib
import json
import os
import re
import resource
import subprocess
import sys
import time
from pathlib import Path
from typing import Any



sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gvid_metadata import read_gvid_frames, sha256_gvid_payload  # noqa: E402


DECODE_RE = re.compile(r"DECODE:\s+(\d+)x(\d+) in ([0-9.]+) ms")
TARGET_RE = re.compile(r"TARGET:\s+(\S+)\s+(\d+)x(\d+) in ([0-9.]+) ms")


REPO_ROOT = Path(__file__).resolve().parents[2]


def default_external_root() -> Path:
    return external_path()


def resolve_artifact_path(path_text: str | None, *, external_root: Path | None = None) -> Path | None:
    if not path_text:
        return None
    path = Path(path_text)
    if path.is_absolute():
        return path
    root = external_root or default_external_root()
    if path.parts and path.parts[0] in {"artifacts", "models", "checkpoints", "cnn"}:
        return root / path
    return REPO_ROOT / path


def load_registry(registry_path: Path) -> dict[str, Any]:
    with registry_path.open("r", encoding="utf-8") as f:
        return json.load(f)


def resolve_pipeline(
    registry_path: Path,
    pipeline_id: str,
    *,
    external_root: Path | None = None,
) -> dict[str, Any]:
    registry = load_registry(registry_path)
    pipelines = registry.get("pipelines") or {}
    cnns = registry.get("cnns") or {}
    codecs = registry.get("codecs") or {}
    if pipeline_id not in pipelines:
        raise KeyError(f"pipeline {pipeline_id!r} not found in {registry_path}")
    pipeline = dict(pipelines[pipeline_id])
    codec_id = pipeline.get("codec")
    cnn_id = pipeline.get("cnn")
    if codec_id not in codecs:
        raise KeyError(f"pipeline {pipeline_id!r} references missing codec {codec_id!r}")
    if cnn_id not in cnns:
        raise KeyError(f"pipeline {pipeline_id!r} references missing cnn {cnn_id!r}")
    codec = dict(codecs[codec_id])
    cnn = dict(cnns[cnn_id])
    checkpoint = resolve_artifact_path(cnn.get("ckpt_path"), external_root=external_root)
    if checkpoint is None:
        raise ValueError(f"cnn {cnn_id!r} has no ckpt_path")
    return {
        "registry_path": str(registry_path),
        "pipeline_id": pipeline_id,
        "pipeline": pipeline,
        "codec_id": codec_id,
        "codec": codec,
        "cnn_id": cnn_id,
        "cnn": cnn,
        "checkpoint": checkpoint,
    }


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def extract_payload(gvid: Path, frame: dict[str, int], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with gvid.open("rb") as src, output.open("wb") as dst:
        src.seek(int(frame["payload_offset"]))
        remaining = int(frame["payload_size"])
        while remaining:
            chunk = src.read(min(1024 * 1024, remaining))
            if not chunk:
                raise EOFError(f"{gvid} ended while extracting frame tag {frame['frame_tag']}")
            dst.write(chunk)
            remaining -= len(chunk)


def parse_decode_stderr(stderr: str) -> dict[str, Any]:
    decode = DECODE_RE.search(stderr)
    target = TARGET_RE.search(stderr)
    return {
        "stderr": stderr,
        "width": int(decode.group(1)) if decode else None,
        "height": int(decode.group(2)) if decode else None,
        "decode_ms_reported": float(decode.group(3)) if decode else None,
        "target": target.group(1) if target else None,
        "target_width": int(target.group(2)) if target else None,
        "target_height": int(target.group(3)) if target else None,
        "target_ms_reported": float(target.group(4)) if target else None,
    }


def decode_payload(
    decoder: Path,
    payload: Path,
    out_raw: Path,
    *,
    sensor_width: int,
    sensor_height: int,
    target: str,
) -> dict[str, Any]:
    cmd = [
        str(decoder),
        str(payload),
        str(sensor_width),
        str(sensor_height),
        str(out_raw),
        target,
    ]
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    process_s = time.perf_counter() - t0
    parsed = parse_decode_stderr(proc.stderr)
    parsed.update(
        {
            "command": cmd,
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "process_s": process_s,
        }
    )
    if proc.returncode != 0:
        raise RuntimeError(f"decode failed for {payload}: {proc.stderr[-1000:]}")
    return parsed


def read_low_raw(path: Path, width: int, height: int) -> np.ndarray:
    import numpy as np

    arr = np.fromfile(path, dtype="<u2")
    expected = width * height
    if arr.size != expected:
        raise ValueError(f"{path} has {arr.size} pixels, expected {expected}")
    return arr.reshape((height, width))


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    import numpy as np

    arr = np.asarray(values, dtype=np.float64)
    return float(np.percentile(arr, pct))


def max_rss_mb() -> float:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    if sys.platform == "darwin":
        return float(usage.ru_maxrss) / 1024.0 / 1024.0
    return float(usage.ru_maxrss) / 1024.0


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    keys = {
        "decode_process_s": [float(r["decode"]["process_s"]) for r in rows],
        "decode_reported_s": [float(r["decode"]["decode_ms_reported"]) / 1000.0 for r in rows],
        "sr_inference_plus_copy_s": [float(r["sr_timing"]["inference_plus_copy_s"]) for r in rows],
        "sr_write_output_s": [float(r["sr_timing"]["write_output_s"]) for r in rows],
        "sr_total_with_write_s": [float(r["sr_timing"]["total_with_write_s"]) for r in rows],
        "decode_plus_sr_total_s": [float(r["combined_timing"]["decode_plus_sr_total_s"]) for r in rows],
    }
    out: dict[str, Any] = {}
    for key, values in keys.items():
        out[key] = {
            "min": min(values) if values else 0.0,
            "median": percentile(values, 50),
            "p95": percentile(values, 95),
            "max": max(values) if values else 0.0,
            "mean": float(sum(values) / len(values)) if values else 0.0,
        }
    total = out["decode_plus_sr_total_s"]["median"]
    out["fps_median_decode_plus_sr"] = (1.0 / total) if total else 0.0
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gvid", type=Path, required=True)
    ap.add_argument("--checkpoint", type=Path)
    ap.add_argument("--registry", type=Path, default=REPO_ROOT / "pipelines" / "registry.json")
    ap.add_argument("--pipeline", help="Pipeline id from pipelines/registry.json; resolves the SR checkpoint and defaults.")
    ap.add_argument("--decoder", type=Path, default=REPO_ROOT / "build-local/bin/fused_decode_cli")
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--frame-start", type=int, default=0)
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--sensor-width", type=int)
    ap.add_argument("--sensor-height", type=int)
    ap.add_argument("--low-width", type=int)
    ap.add_argument("--low-height", type=int)
    ap.add_argument("--high-width", type=int)
    ap.add_argument("--high-height", type=int)
    ap.add_argument("--decode-target", default="4k_raw_1x")
    ap.add_argument("--tile", type=int)
    ap.add_argument("--overlap", type=int)
    ap.add_argument("--device", choices=("auto", "mps", "cpu"), default="auto")
    ap.add_argument("--write-sr-raw", action="store_true")
    ap.add_argument("--keep-sr-raw", action="store_true")
    args = ap.parse_args()

    pipeline_receipt: dict[str, Any] | None = None
    if args.pipeline:
        resolved = resolve_pipeline(args.registry, args.pipeline)
        pipeline_receipt = {k: v for k, v in resolved.items() if k != "checkpoint"}
        if args.checkpoint is None:
            args.checkpoint = resolved["checkpoint"]
        codec = resolved["codec"]
        cnn = resolved["cnn"]
        args.sensor_width = args.sensor_width or int(codec.get("source_width") or 4096)
        args.sensor_height = args.sensor_height or int(codec.get("source_height") or 3072)
        args.low_width = args.low_width or args.sensor_width
        args.low_height = args.low_height or args.sensor_height
        args.high_width = args.high_width or args.low_width * 2
        args.high_height = args.high_height or args.low_height * 2
        args.tile = args.tile or int(cnn.get("tile") or 512)
        args.overlap = args.overlap if args.overlap is not None else int(cnn.get("overlap") or 64)
    if args.checkpoint is None:
        raise SystemExit("--checkpoint is required unless --pipeline resolves a checkpoint")
    args.sensor_width = args.sensor_width or 4096
    args.sensor_height = args.sensor_height or 3072
    args.low_width = args.low_width or args.sensor_width
    args.low_height = args.low_height or args.sensor_height
    args.high_width = args.high_width or args.low_width * 2
    args.high_height = args.high_height or args.low_height * 2
    args.tile = args.tile or 512
    args.overlap = 64 if args.overlap is None else args.overlap

    args.out_dir.mkdir(parents=True, exist_ok=True)
    scratch = args.out_dir / "scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    sr_dir = args.out_dir / "sr_raw"
    if args.keep_sr_raw:
        sr_dir.mkdir(parents=True, exist_ok=True)

    import torch

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from mission1_sr_runtime import deinterleave, load_model, reinterleave_to_path, run_tiles, sync_device  # noqa: E402

    if args.device == "auto":
        device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    model, config = load_model(args.checkpoint, device)
    coordinate_channels_enabled = bool(config.get("coordinate_channels")) or str(config.get("architecture")) in {
        "coord_preclean_adapter_pixelshuffle",
        "coord_detail_preclean_adapter_pixelshuffle",
        "coord_deep_preclean_adapter_pixelshuffle",
    }
    rss_after_model_load_mb = max_rss_mb()

    frames = read_gvid_frames(args.gvid)
    selected = frames[args.frame_start : args.frame_start + args.frames]
    if len(selected) != args.frames:
        raise ValueError(f"requested {args.frames} frames from {args.frame_start}, stream has {len(frames)}")

    warm_channels = 6 if coordinate_channels_enabled else 4
    warm = torch.zeros((1, warm_channels, 32, 32), dtype=torch.float32, device=device)
    with torch.inference_mode():
        _ = model(warm)
    sync_device(device)

    rows: list[dict[str, Any]] = []
    t_all0 = time.perf_counter()
    for frame in selected:
        frame_index = int(frame["frame_index"])
        stem = f"frame_{frame_index:06d}"
        payload = scratch / f"{stem}.gpr"
        decoded = scratch / f"{stem}_decoded.raw"
        sr_raw = sr_dir / f"{stem}_sr8k.raw" if args.keep_sr_raw else scratch / f"{stem}_sr8k.raw"
        try:
            extract_payload(args.gvid, frame, payload)
            decode = decode_payload(
                args.decoder,
                payload,
                decoded,
                sensor_width=args.sensor_width,
                sensor_height=args.sensor_height,
                target=args.decode_target,
            )
            low = read_low_raw(decoded, args.low_width, args.low_height)
            planes = deinterleave(low)
            out_planes, sr_timing = run_tiles(
                model,
                planes,
                device,
                args.tile,
                args.overlap,
                args.write_sr_raw,
                args.high_width,
                args.high_height,
                coordinate_channels_enabled,
            )
            write_s = 0.0
            if out_planes is not None:
                t0 = time.perf_counter()
                reinterleave_to_path(sr_raw, out_planes)
                write_s = time.perf_counter() - t0
                if not args.keep_sr_raw:
                    sr_raw.unlink(missing_ok=True)
            sr_timing = {
                **sr_timing,
                "write_output_s": write_s,
                "total_with_write_s": sr_timing["inference_plus_copy_s"] + write_s,
                "fps_inference_only": 1.0 / sr_timing["inference_plus_copy_s"]
                if sr_timing["inference_plus_copy_s"]
                else 0.0,
                "fps_with_write": 1.0 / (sr_timing["inference_plus_copy_s"] + write_s)
                if sr_timing["inference_plus_copy_s"] + write_s
                else 0.0,
            }
            decode_reported_s = float(decode["decode_ms_reported"]) / 1000.0 if decode["decode_ms_reported"] else 0.0
            combined_total = decode_reported_s + sr_timing["total_with_write_s"]
            rows.append(
                {
                    "frame_index": frame_index,
                    "frame_tag": int(frame["frame_tag"]),
                    "payload_size": int(frame["payload_size"]),
                    "payload_sha256": sha256_gvid_payload(args.gvid, frame),
                    "decode": decode,
                    "decoded_raw_sha256": sha256_file(decoded),
                    "sr_timing": sr_timing,
                    "max_rss_mb": max_rss_mb(),
                    "sr_raw": str(sr_raw) if args.keep_sr_raw and args.write_sr_raw else None,
                    "sr_raw_sha256": sha256_file(sr_raw) if args.keep_sr_raw and args.write_sr_raw else None,
                    "combined_timing": {
                        "decode_reported_s": decode_reported_s,
                        "sr_total_with_write_s": sr_timing["total_with_write_s"],
                        "decode_plus_sr_total_s": combined_total,
                        "decode_plus_sr_fps": 1.0 / combined_total if combined_total else 0.0,
                    },
                }
            )
        finally:
            payload.unlink(missing_ok=True)
            decoded.unlink(missing_ok=True)
            if not args.keep_sr_raw:
                sr_raw.unlink(missing_ok=True)

    try:
        scratch.rmdir()
    except OSError:
        pass

    receipt = {
        "schema": "mission1_native12_gvid_to_8k_sr_multiframe.v1",
        "gvid": str(args.gvid),
        "gvid_sha256": sha256_file(args.gvid),
        "checkpoint": str(args.checkpoint),
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "registry_pipeline": pipeline_receipt,
        "decoder": str(args.decoder),
        "decoder_sha256": sha256_file(args.decoder),
        "device": str(device),
        "config": config,
        "rss_after_model_load_mb": rss_after_model_load_mb,
        "max_rss_mb": max_rss_mb(),
        "frame_start": args.frame_start,
        "frames_requested": args.frames,
        "frames_rendered": len(rows),
        "write_sr_raw": args.write_sr_raw,
        "keep_sr_raw": args.keep_sr_raw,
        "input_bayer": {"width": args.low_width, "height": args.low_height},
        "output_bayer": {"width": args.high_width, "height": args.high_height},
        "tile": args.tile,
        "overlap": args.overlap,
        "elapsed_s": time.perf_counter() - t_all0,
        "summary": summarize(rows),
        "frames": rows,
    }
    receipt_path = args.out_dir / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"receipt": str(receipt_path), "summary": receipt["summary"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
