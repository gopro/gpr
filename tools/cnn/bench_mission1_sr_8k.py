#!/usr/bin/env python3
"""Benchmark Mission 1 12MP Bayer-plane CNN upscaling to 50MP/8K-class raw.

This is a runtime receipt tool, not a quality gate. It loads a 4096x3072
uint16 Bayer raw, deinterleaves it into four same-color planes, applies the
Mission 1 residual SR checkpoint tile-by-tile, and optionally writes an
8192x6144 uint16 Bayer raw.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import resource
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission1_sr_models import RAW_SCALE  # noqa: E402
from mission1_sr_runtime import (  # noqa: E402
    coord_channels,
    deinterleave,
    load_model,
    reinterleave_to_path,
    run_tiles,
    sync_device,
)


DEFAULT_LO_W = 4096
DEFAULT_LO_H = 3072


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def estimate_highres_residual_macs(width: int, depth: int, high_plane_h: int, high_plane_w: int) -> int:
    """MACs for ResidualSR as currently implemented: interpolate first, conv at 2x."""
    pixels = high_plane_h * high_plane_w
    middle = max(0, depth - 2)
    macs_per_pixel = 9 * (4 * width + middle * width * width + width * 4)
    return int(pixels * macs_per_pixel)


def estimate_lowres_pixelshuffle_macs(width: int, depth: int, low_plane_h: int, low_plane_w: int) -> int:
    """Comparable MACs if the trunk runs at 12MP-plane resolution then PixelShuffles."""
    pixels = low_plane_h * low_plane_w
    middle = max(0, depth - 2)
    # Last conv emits 4 output channels * 2 * 2 for PixelShuffle -> 16 channels.
    macs_per_pixel = 9 * (4 * width + middle * width * width + width * 16)
    return int(pixels * macs_per_pixel)


def estimate_resblock_pixelshuffle_macs(width: int, depth: int, low_plane_h: int, low_plane_w: int) -> int:
    """MACs for ResBlockPixelShuffleSR: low-res head, residual blocks, subpixel tail."""
    pixels = low_plane_h * low_plane_w
    block_count = max(1, depth - 2)
    macs_per_pixel = 9 * (4 * width + block_count * 2 * width * width + width * 16)
    return int(pixels * macs_per_pixel)


def estimate_edge_pixelshuffle_macs(width: int, depth: int, low_plane_h: int, low_plane_w: int) -> int:
    """MACs for EdgePixelShuffleSR: low-res trunk plus direct highpass subpixel head."""
    pixels = low_plane_h * low_plane_w
    middle = max(0, depth - 2)
    macs_per_pixel = 9 * (4 * width + middle * width * width + width * 16 + 4 * 16)
    return int(pixels * macs_per_pixel)


def estimate_adapter_pixelshuffle_macs(width: int, depth: int, low_plane_h: int, low_plane_w: int) -> int:
    """MACs for AdapterPixelShuffleSR: low-res trunk plus dilated adapter branch."""
    pixels = low_plane_h * low_plane_w
    middle = max(0, depth - 2)
    trunk_per_pixel = 9 * (4 * width + middle * width * width + width * 16)
    adapter_per_pixel = 9 * (4 * width + 2 * width * width + width * 16)
    return int(pixels * (trunk_per_pixel + adapter_per_pixel))


def estimate_green_detail_adapter_pixelshuffle_macs(width: int, depth: int, low_plane_h: int, low_plane_w: int) -> int:
    """MACs for AdapterPixelShuffleSR plus a green-only detail residual branch."""
    pixels = low_plane_h * low_plane_w
    green_detail_per_pixel = 9 * (4 * width + 2 * width * width + width * 8)
    return int(estimate_adapter_pixelshuffle_macs(width, depth, low_plane_h, low_plane_w) + pixels * green_detail_per_pixel)


def estimate_preclean_adapter_pixelshuffle_macs(width: int, depth: int, low_plane_h: int, low_plane_w: int) -> int:
    """MACs for PrecleanAdapterPixelShuffleSR: low-res cleanup plus adapter SR."""
    pixels = low_plane_h * low_plane_w
    preclean_per_pixel = 9 * (4 * width + width * width + width * 4)
    return int(estimate_adapter_pixelshuffle_macs(width, depth, low_plane_h, low_plane_w) + pixels * preclean_per_pixel)


def estimate_coord_preclean_adapter_pixelshuffle_macs(width: int, depth: int, low_plane_h: int, low_plane_w: int) -> int:
    pixels = low_plane_h * low_plane_w
    middle = max(0, depth - 2)
    trunk_per_pixel = 9 * (6 * width + middle * width * width + width * 16)
    adapter_per_pixel = 9 * (6 * width + 2 * width * width + width * 16)
    preclean_per_pixel = 9 * (4 * width + width * width + width * 4)
    return int(pixels * (trunk_per_pixel + adapter_per_pixel + preclean_per_pixel))


def estimate_coord_deep_preclean_adapter_pixelshuffle_macs(
    width: int,
    depth: int,
    low_plane_h: int,
    low_plane_w: int,
) -> int:
    pixels = low_plane_h * low_plane_w
    deep_extra_per_pixel = 9 * (4 * width + 5 * width * width + width * 4)
    return int(
        estimate_coord_preclean_adapter_pixelshuffle_macs(width, depth, low_plane_h, low_plane_w)
        + pixels * deep_extra_per_pixel
    )


def estimate_coord_detail_preclean_adapter_pixelshuffle_macs(
    width: int,
    depth: int,
    low_plane_h: int,
    low_plane_w: int,
) -> int:
    pixels = low_plane_h * low_plane_w
    middle = max(0, depth - 2)
    trunk_per_pixel = 9 * (10 * width + middle * width * width + width * 16)
    adapter_per_pixel = 9 * (10 * width + 2 * width * width + width * 16)
    preclean_per_pixel = 9 * (4 * width + width * width + width * 4)
    return int(pixels * (trunk_per_pixel + adapter_per_pixel + preclean_per_pixel))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--raw", type=Path, required=True, help="4096x3072 uint16 Bayer raw")
    ap.add_argument("--checkpoint", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--low-width", type=int, default=DEFAULT_LO_W)
    ap.add_argument("--low-height", type=int, default=DEFAULT_LO_H)
    ap.add_argument("--high-width", type=int, help="defaults to 2x --low-width")
    ap.add_argument("--high-height", type=int, help="defaults to 2x --low-height")
    ap.add_argument("--tile", type=int, default=256)
    ap.add_argument("--overlap", type=int, default=0, help="Low-plane pixels of tile context; center crop is written.")
    ap.add_argument("--device", choices=("auto", "mps", "cpu"), default="auto")
    ap.add_argument("--write-output", action="store_true")
    ap.add_argument("--output-raw", type=Path)
    args = ap.parse_args()

    high_width = args.high_width or args.low_width * 2
    high_height = args.high_height or args.low_height * 2
    if high_width != args.low_width * 2 or high_height != args.low_height * 2:
        raise ValueError("this benchmark expects exactly 2x high dimensions")
    args.out_dir.mkdir(parents=True, exist_ok=True)
    if args.device == "auto":
        device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    else:
        device = torch.device(args.device)

    arr = np.fromfile(args.raw, dtype="<u2")
    expected = args.low_width * args.low_height
    if arr.size != expected:
        raise ValueError(f"{args.raw} has {arr.size} pixels, expected {expected}")
    bayer = arr.reshape((args.low_height, args.low_width))
    planes = deinterleave(bayer)

    model, config = load_model(args.checkpoint, device)
    param_count = sum(p.numel() for p in model.parameters())

    # One tiny warmup avoids charging graph/kernel setup to the frame receipt.
    coordinate_channels_enabled = bool(config.get("coordinate_channels")) or str(config.get("architecture")) in {
        "coord_preclean_adapter_pixelshuffle",
        "coord_deep_preclean_adapter_pixelshuffle",
        "coord_detail_preclean_adapter_pixelshuffle",
    }
    warm_patch = planes[:, :32, :32].astype(np.float32) / RAW_SCALE
    if coordinate_channels_enabled:
        warm_coords = coord_channels(x0=0, y0=0, tile=32, low_plane_w=args.low_width // 2, low_plane_h=args.low_height // 2)
        warm_patch = np.concatenate([warm_patch, warm_coords], axis=0)
    warm = torch.from_numpy(warm_patch[None]).to(device)
    with torch.inference_mode():
        _ = model(warm)
    sync_device(device)

    out_planes, timing = run_tiles(
        model,
        planes,
        device,
        args.tile,
        args.overlap,
        args.write_output,
        high_width,
        high_height,
        coordinate_channels_enabled,
    )
    write_s = 0.0
    ckpt_stem = args.checkpoint.name.replace(".pt", "")
    overlap_suffix = f"_ov{args.overlap}" if args.overlap else ""
    output_raw = args.output_raw or (args.out_dir / f"{args.raw.stem}_{ckpt_stem}_sr8k_{args.tile}{overlap_suffix}.raw")
    if out_planes is not None:
        t0 = time.perf_counter()
        reinterleave_to_path(output_raw, out_planes)
        write_s = time.perf_counter() - t0

    usage = resource.getrusage(resource.RUSAGE_SELF)
    high_macs = estimate_highres_residual_macs(
        int(config["width"]), int(config["depth"]), high_height // 2, high_width // 2
    )
    low_macs = estimate_lowres_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    resblock_macs = estimate_resblock_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    edge_macs = estimate_edge_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    adapter_macs = estimate_adapter_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    green_detail_adapter_macs = estimate_green_detail_adapter_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    preclean_adapter_macs = estimate_preclean_adapter_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    coord_preclean_adapter_macs = estimate_coord_preclean_adapter_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    coord_deep_preclean_adapter_macs = estimate_coord_deep_preclean_adapter_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    coord_detail_preclean_adapter_macs = estimate_coord_detail_preclean_adapter_pixelshuffle_macs(
        int(config["width"]), int(config["depth"]), args.low_height // 2, args.low_width // 2
    )
    architecture = str(config.get("architecture", "residual_highres"))
    if architecture == "lowres_pixelshuffle":
        actual_macs = low_macs
    elif architecture == "resblock_pixelshuffle":
        actual_macs = resblock_macs
    elif architecture == "edge_pixelshuffle":
        actual_macs = edge_macs
    elif architecture == "adapter_pixelshuffle":
        actual_macs = adapter_macs
    elif architecture == "green_detail_adapter_pixelshuffle":
        actual_macs = green_detail_adapter_macs
    elif architecture == "preclean_adapter_pixelshuffle":
        actual_macs = preclean_adapter_macs
    elif architecture == "coord_preclean_adapter_pixelshuffle":
        actual_macs = coord_preclean_adapter_macs
    elif architecture == "coord_detail_preclean_adapter_pixelshuffle":
        actual_macs = coord_detail_preclean_adapter_macs
    elif architecture == "coord_deep_preclean_adapter_pixelshuffle":
        actual_macs = coord_deep_preclean_adapter_macs
    else:
        actual_macs = high_macs
    payload: dict[str, Any] = {
        "schema": "mission1_sr_8k_bench.v1",
        "raw": str(args.raw),
        "raw_sha256": sha256_file(args.raw),
        "checkpoint": str(args.checkpoint),
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "device": str(device),
        "config": config,
        "parameter_count": int(param_count),
        "input_bayer": {"width": args.low_width, "height": args.low_height, "bytes": int(args.raw.stat().st_size)},
        "output_bayer": {
            "width": high_width,
            "height": high_height,
            "bytes": int(high_width * high_height * 2),
            "path": str(output_raw) if out_planes is not None else None,
            "written": out_planes is not None,
        },
        "timing": {
            **timing,
            "write_output_s": write_s,
            "total_with_write_s": timing["inference_plus_copy_s"] + write_s,
            "fps_inference_only": 1.0 / timing["inference_plus_copy_s"] if timing["inference_plus_copy_s"] else 0.0,
            "fps_with_write": 1.0 / (timing["inference_plus_copy_s"] + write_s)
            if timing["inference_plus_copy_s"] + write_s
            else 0.0,
        },
        "architecture_cost": {
            "actual_architecture": architecture,
            "actual_macs_per_frame": actual_macs,
            "actual_tmacs_per_frame": actual_macs / 1e12,
            "current_highres_residual_conv_macs_per_frame": high_macs,
            "current_highres_residual_conv_tmacs_per_frame": high_macs / 1e12,
            "same_width_lowres_pixelshuffle_macs_per_frame": low_macs,
            "same_width_lowres_pixelshuffle_tmacs_per_frame": low_macs / 1e12,
            "lowres_pixelshuffle_cost_ratio": low_macs / high_macs if high_macs else None,
            "same_width_resblock_pixelshuffle_macs_per_frame": resblock_macs,
            "same_width_resblock_pixelshuffle_tmacs_per_frame": resblock_macs / 1e12,
            "resblock_pixelshuffle_cost_ratio": resblock_macs / high_macs if high_macs else None,
            "same_width_edge_pixelshuffle_macs_per_frame": edge_macs,
            "same_width_edge_pixelshuffle_tmacs_per_frame": edge_macs / 1e12,
            "edge_pixelshuffle_cost_ratio": edge_macs / high_macs if high_macs else None,
            "same_width_adapter_pixelshuffle_macs_per_frame": adapter_macs,
            "same_width_adapter_pixelshuffle_tmacs_per_frame": adapter_macs / 1e12,
            "adapter_pixelshuffle_cost_ratio": adapter_macs / high_macs if high_macs else None,
            "same_width_green_detail_adapter_pixelshuffle_macs_per_frame": green_detail_adapter_macs,
            "same_width_green_detail_adapter_pixelshuffle_tmacs_per_frame": green_detail_adapter_macs / 1e12,
            "green_detail_adapter_pixelshuffle_cost_ratio": (
                green_detail_adapter_macs / high_macs if high_macs else None
            ),
            "same_width_preclean_adapter_pixelshuffle_macs_per_frame": preclean_adapter_macs,
            "same_width_preclean_adapter_pixelshuffle_tmacs_per_frame": preclean_adapter_macs / 1e12,
            "preclean_adapter_pixelshuffle_cost_ratio": preclean_adapter_macs / high_macs if high_macs else None,
            "same_width_coord_preclean_adapter_pixelshuffle_macs_per_frame": coord_preclean_adapter_macs,
            "same_width_coord_preclean_adapter_pixelshuffle_tmacs_per_frame": coord_preclean_adapter_macs / 1e12,
            "coord_preclean_adapter_pixelshuffle_cost_ratio": coord_preclean_adapter_macs / high_macs if high_macs else None,
            "same_width_coord_deep_preclean_adapter_pixelshuffle_macs_per_frame": coord_deep_preclean_adapter_macs,
            "same_width_coord_deep_preclean_adapter_pixelshuffle_tmacs_per_frame": coord_deep_preclean_adapter_macs / 1e12,
            "coord_deep_preclean_adapter_pixelshuffle_cost_ratio": (
                coord_deep_preclean_adapter_macs / high_macs if high_macs else None
            ),
            "same_width_coord_detail_preclean_adapter_pixelshuffle_macs_per_frame": coord_detail_preclean_adapter_macs,
            "same_width_coord_detail_preclean_adapter_pixelshuffle_tmacs_per_frame": (
                coord_detail_preclean_adapter_macs / 1e12
            ),
            "coord_detail_preclean_adapter_pixelshuffle_cost_ratio": (
                coord_detail_preclean_adapter_macs / high_macs if high_macs else None
            ),
        },
        "max_rss_mb": usage.ru_maxrss / 1024 / 1024 if sys.platform == "darwin" else usage.ru_maxrss / 1024,
        "note": "If overlap is non-zero, each tile is inferred with context and only the center crop is written; no alpha blending is used.",
    }
    out_json = args.out_dir / f"{args.raw.stem}_{ckpt_stem}_sr8k_{args.tile}{overlap_suffix}_bench.json"
    out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(json.dumps({"receipt": str(out_json), "timing": payload["timing"], "architecture_cost": payload["architecture_cost"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
