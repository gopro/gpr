"""Mission 1 Bayer SR checkpoint loading and tiled inference."""
from __future__ import annotations

import time
from pathlib import Path
from typing import Any

import numpy as np
import torch

from mission1_sr_models import RAW_SCALE, make_model_from_config


def sync_device(device: torch.device) -> None:
    if device.type == "mps" and hasattr(torch, "mps"):
        torch.mps.synchronize()
    elif device.type == "cuda":
        torch.cuda.synchronize(device)


def deinterleave(bayer: np.ndarray) -> np.ndarray:
    return np.stack(
        [
            bayer[0::2, 0::2],
            bayer[0::2, 1::2],
            bayer[1::2, 0::2],
            bayer[1::2, 1::2],
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
    out.tofile(path)


def load_model(checkpoint: Path, device: torch.device) -> tuple[torch.nn.Module, dict[str, Any]]:
    ckpt = torch.load(checkpoint, map_location="cpu", weights_only=False)
    config = dict(ckpt["config"])
    model = make_model_from_config(config).to(device)
    model.load_state_dict(ckpt["model"], strict=True)
    model.eval()
    return model, config


def coord_channels(
    *,
    x0: int,
    y0: int,
    tile: int,
    low_plane_w: int,
    low_plane_h: int,
) -> np.ndarray:
    x_den = max(1, low_plane_w - 1)
    y_den = max(1, low_plane_h - 1)
    x_base = np.arange(tile, dtype=np.float32)[None, :]
    y_base = np.arange(tile, dtype=np.float32)[:, None]
    x_coord = ((float(x0) + x_base) / x_den) * 2.0 - 1.0
    y_coord = ((float(y0) + y_base) / y_den) * 2.0 - 1.0
    return np.stack(
        [
            np.broadcast_to(x_coord, (tile, tile)),
            np.broadcast_to(y_coord, (tile, tile)),
        ],
        axis=0,
    )


def run_tiles(
    model: torch.nn.Module,
    planes: np.ndarray,
    device: torch.device,
    tile: int,
    overlap: int,
    write_output: bool,
    high_width: int,
    high_height: int,
    coordinate_channels: bool,
) -> tuple[np.ndarray | None, dict[str, Any]]:
    if not isinstance(planes, np.ndarray) or planes.ndim != 3 or planes.shape[0] != 4:
        raise ValueError("planes must have shape (4, height, width)")
    for name, value in (
        ("tile", tile),
        ("overlap", overlap),
        ("high_width", high_width),
        ("high_height", high_height),
    ):
        if isinstance(value, (bool, np.bool_)) or not isinstance(value, (int, np.integer)):
            raise ValueError(f"{name} must be an integer")
    _, low_h, low_w = planes.shape
    if low_h <= 0 or low_w <= 0:
        raise ValueError("plane dimensions must be positive")
    if tile <= 0:
        raise ValueError("--tile must be positive")
    if overlap < 0 or overlap >= tile:
        raise ValueError("--overlap must be nonnegative and smaller than --tile")
    # Smaller images need an explicit padding policy, not negative tile origins.
    if tile > low_h or tile > low_w:
        raise ValueError("--tile must not exceed either plane dimension")
    high_h = low_h * 2
    high_w = low_w * 2
    if high_height != high_h * 2 or high_width != high_w * 2:
        raise ValueError(f"unexpected plane dimensions {low_w}x{low_h} -> {high_w}x{high_h}")

    out = np.empty((4, high_h, high_w), dtype=np.uint16) if write_output else None
    tile_times: list[float] = []
    n_tiles = 0
    t0 = time.perf_counter()
    step = tile - overlap
    y_starts = list(range(0, max(1, low_h - tile + 1), step))
    x_starts = list(range(0, max(1, low_w - tile + 1), step))
    if y_starts[-1] != low_h - tile:
        y_starts.append(low_h - tile)
    if x_starts[-1] != low_w - tile:
        x_starts.append(low_w - tile)
    with torch.inference_mode():
        for yi, y in enumerate(y_starts):
            for xi, x in enumerate(x_starts):
                patch = planes[:, y : y + tile, x : x + tile].astype(np.float32)
                patch *= 1.0 / RAW_SCALE
                if coordinate_channels:
                    coords = coord_channels(x0=x, y0=y, tile=tile, low_plane_w=low_w, low_plane_h=low_h)
                    patch = np.concatenate([patch, coords], axis=0)
                xt = torch.from_numpy(patch[None]).to(device)
                sync_device(device)
                tt0 = time.perf_counter()
                pred = model(xt)
                sync_device(device)
                tile_times.append(time.perf_counter() - tt0)
                if out is not None:
                    pred_np = pred[0].detach().cpu().numpy()
                    pred_u16 = np.clip(pred_np * RAW_SCALE + 0.5, 0, 65535).astype(np.uint16)
                    crop_y0 = 0 if yi == 0 else overlap // 2
                    crop_x0 = 0 if xi == 0 else overlap // 2
                    crop_y1 = tile if yi == len(y_starts) - 1 else tile - overlap // 2
                    crop_x1 = tile if xi == len(x_starts) - 1 else tile - overlap // 2
                    dst_y0 = (y + crop_y0) * 2
                    dst_x0 = (x + crop_x0) * 2
                    dst_y1 = (y + crop_y1) * 2
                    dst_x1 = (x + crop_x1) * 2
                    out[:, dst_y0:dst_y1, dst_x0:dst_x1] = pred_u16[
                        :, crop_y0 * 2 : crop_y1 * 2, crop_x0 * 2 : crop_x1 * 2
                    ]
                n_tiles += 1
    total_s = time.perf_counter() - t0
    stats = {
        "tile": tile,
        "overlap": overlap,
        "tile_count": n_tiles,
        "inference_plus_copy_s": total_s,
        "tile_time_s_mean": float(np.mean(tile_times)) if tile_times else 0.0,
        "tile_time_s_median": float(np.median(tile_times)) if tile_times else 0.0,
        "tile_time_s_p95": float(np.percentile(tile_times, 95)) if tile_times else 0.0,
    }
    return out, stats
