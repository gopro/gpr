#!/usr/bin/env python3
"""Runtime model and inference helpers; experiment commands are archived."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from pathlib import Path
from typing import Any

import cv2
import numpy as np
import torch
import torch.nn as nn


RAW_SCALE = 16383.0
DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
DEMOSAIC_CODES = {
    "rggb": cv2.COLOR_BayerRGGB2RGB_EA,
    "bggr": cv2.COLOR_BayerBGGR2RGB_EA,
    "grbg": cv2.COLOR_BayerGRBG2RGB_EA,
    "gbrg": cv2.COLOR_BayerGBRG2RGB_EA,
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_u16(path: Path, width: int, height: int) -> np.ndarray:
    arr = np.fromfile(path, dtype="<u2")
    expected = width * height
    if arr.size != expected:
        raise ValueError(f"{path} has {arr.size} pixels, expected {expected}")
    return arr.reshape((height, width))


def deinterleave(raw: np.ndarray) -> np.ndarray:
    return np.stack(
        [
            raw[0::2, 0::2],
            raw[0::2, 1::2],
            raw[1::2, 0::2],
            raw[1::2, 1::2],
        ],
        axis=0,
    )


def reinterleave_to_path(path: Path, planes: np.ndarray) -> None:
    _, h, w = planes.shape
    out = np.empty((h * 2, w * 2), dtype="<u2")
    out[0::2, 0::2] = planes[0]
    out[0::2, 1::2] = planes[1]
    out[1::2, 0::2] = planes[2]
    out[1::2, 1::2] = planes[3]
    path.parent.mkdir(parents=True, exist_ok=True)
    out.tofile(path)


class CleanupNet(nn.Module):
    def __init__(self, width: int = 48, depth: int = 5, residual_scale: float = 0.04) -> None:
        super().__init__()
        self.residual_scale = residual_scale
        layers: list[nn.Module] = [nn.Conv2d(4, width, 3, padding=1), nn.GELU()]
        for _ in range(max(0, depth - 2)):
            layers += [nn.Conv2d(width, width, 3, padding=1), nn.GELU()]
        tail = nn.Conv2d(width, 4, 3, padding=1)
        nn.init.zeros_(tail.weight)
        nn.init.zeros_(tail.bias)
        layers.append(tail)
        self.net = nn.Sequential(*layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return torch.clamp(x + self.net(x) * self.residual_scale, 0.0, 1.0)


def make_model(config: dict[str, Any]) -> CleanupNet:
    return CleanupNet(
        width=int(config["width"]),
        depth=int(config["depth"]),
        residual_scale=float(config["residual_scale"]),
    )


def apply(args: argparse.Namespace) -> int:
    ckpt = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    config = dict(ckpt["config"])
    model = make_model(config).to(DEVICE)
    model.load_state_dict(ckpt["model"], strict=True)
    model.eval()
    raw = read_u16(args.in_raw, args.width, args.height)
    planes = deinterleave(raw)
    _, plane_h, plane_w = planes.shape
    step = args.tile - args.overlap
    if step <= 0:
        raise ValueError("--overlap must be smaller than --tile")
    y_starts = list(range(0, max(1, plane_h - args.tile + 1), step))
    x_starts = list(range(0, max(1, plane_w - args.tile + 1), step))
    if y_starts[-1] != plane_h - args.tile:
        y_starts.append(plane_h - args.tile)
    if x_starts[-1] != plane_w - args.tile:
        x_starts.append(plane_w - args.tile)
    out = np.empty_like(planes)
    tile_times: list[float] = []
    started = time.perf_counter()
    with torch.inference_mode():
        for yi, y0 in enumerate(y_starts):
            for xi, x0 in enumerate(x_starts):
                patch = planes[:, y0 : y0 + args.tile, x0 : x0 + args.tile].astype(np.float32) / RAW_SCALE
                x = torch.from_numpy(patch[None]).to(DEVICE)
                t0 = time.perf_counter()
                pred = model(x)
                if DEVICE.type == "mps":
                    torch.mps.synchronize()
                tile_times.append(time.perf_counter() - t0)
                pred_np = np.clip(pred[0].cpu().numpy() * RAW_SCALE + 0.5, 0, 65535).astype(np.uint16)
                crop_y0 = 0 if yi == 0 else args.overlap // 2
                crop_x0 = 0 if xi == 0 else args.overlap // 2
                crop_y1 = args.tile if yi == len(y_starts) - 1 else args.tile - args.overlap // 2
                crop_x1 = args.tile if xi == len(x_starts) - 1 else args.tile - args.overlap // 2
                out[:, y0 + crop_y0 : y0 + crop_y1, x0 + crop_x0 : x0 + crop_x1] = pred_np[
                    :, crop_y0:crop_y1, crop_x0:crop_x1
                ]
    total_s = time.perf_counter() - started
    reinterleave_to_path(args.out_raw, out)
    receipt = {
        "schema": "gpr.bayer_rgb_target_cleanup_apply.v1",
        "checkpoint": str(args.checkpoint),
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "input": str(args.in_raw),
        "output": str(args.out_raw),
        "width": args.width,
        "height": args.height,
        "tile": args.tile,
        "overlap": args.overlap,
        "tile_count": len(tile_times),
        "total_s": total_s,
        "fps": 1.0 / total_s if total_s else 0.0,
        "tile_time_s_median": float(np.median(tile_times)) if tile_times else 0.0,
    }
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    aply = sub.add_parser("apply")
    aply.add_argument("--checkpoint", type=Path, required=True)
    aply.add_argument("--in-raw", type=Path, required=True)
    aply.add_argument("--out-raw", type=Path, required=True)
    aply.add_argument("--width", type=int, required=True)
    aply.add_argument("--height", type=int, required=True)
    aply.add_argument("--tile", type=int, default=512)
    aply.add_argument("--overlap", type=int, default=64)
    aply.add_argument("--receipt", type=Path)

    args = ap.parse_args()
    if args.cmd == "apply":
        return apply(args)
    raise ValueError(args.cmd)


if __name__ == "__main__":
    raise SystemExit(main())
