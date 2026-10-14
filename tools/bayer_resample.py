#!/usr/bin/env python3
"""CFA-preserving Bayer resampling helpers."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np


def deinterleave_bayer(bayer: np.ndarray) -> np.ndarray:
    """Return four same-color Bayer planes in RGGB/GBRG mosaic order."""
    if bayer.ndim != 2:
        raise ValueError(f"expected 2D Bayer image, got shape={bayer.shape}")
    return np.stack(
        [
            bayer[0::2, 0::2],
            bayer[0::2, 1::2],
            bayer[1::2, 0::2],
            bayer[1::2, 1::2],
        ],
        axis=0,
    )


def reinterleave_bayer(planes: np.ndarray) -> np.ndarray:
    """Rebuild a Bayer mosaic from four same-color planes."""
    if planes.ndim != 3 or planes.shape[0] != 4:
        raise ValueError(f"expected 4xHxW planes, got shape={planes.shape}")
    _, h, w = planes.shape
    out = np.empty((h * 2, w * 2), dtype=np.uint16)
    out[0::2, 0::2] = planes[0]
    out[0::2, 1::2] = planes[1]
    out[1::2, 0::2] = planes[2]
    out[1::2, 1::2] = planes[3]
    return out


def _validate_u16_plane(plane: np.ndarray) -> None:
    if plane.ndim != 2:
        raise ValueError(f"expected 2D plane, got shape={plane.shape}")
    h, w = plane.shape
    if w % 2 != 0 or h % 2 != 0:
        raise ValueError(f"plane dimensions must be divisible by 2, got {w}x{h}")
    if not np.issubdtype(plane.dtype, np.integer):
        raise TypeError(f"expected integer plane dtype, got {plane.dtype}")


def downsample_plane_2x_area(plane: np.ndarray) -> np.ndarray:
    """Downsample one color plane by 2x with rounded 2x2 area average."""
    _validate_u16_plane(plane)
    p = plane.astype(np.uint32, copy=False)
    return (
        (p[0::2, 0::2] + p[0::2, 1::2] + p[1::2, 0::2] + p[1::2, 1::2] + 2) >> 2
    ).astype(np.uint16)


def downsample_plane_2x_gaussian_area(plane: np.ndarray) -> np.ndarray:
    """Downsample one color plane by 2x with a same-plane anti-aliasing filter.

    This uses a separable 5-tap binomial low-pass filter followed by 2x2 area
    averaging. It is pure NumPy so CI can verify the production resampling
    contract without requiring OpenCV.
    """
    _validate_u16_plane(plane)
    src = plane.astype(np.float32, copy=False)
    p = np.pad(src, ((2, 2), (2, 2)), mode="edge")
    h = (
        p[:, 0:-4]
        + 4.0 * p[:, 1:-3]
        + 6.0 * p[:, 2:-2]
        + 4.0 * p[:, 3:-1]
        + p[:, 4:]
    ) * (1.0 / 16.0)
    v = (
        h[0:-4, :]
        + 4.0 * h[1:-3, :]
        + 6.0 * h[2:-2, :]
        + 4.0 * h[3:-1, :]
        + h[4:, :]
    ) * (1.0 / 16.0)
    out = (
        v[0::2, 0::2]
        + v[0::2, 1::2]
        + v[1::2, 0::2]
        + v[1::2, 1::2]
    ) * 0.25
    return np.clip(out + 0.5, 0, 65535).astype(np.uint16)


def downsample_plane_2x_sample(plane: np.ndarray) -> np.ndarray:
    """Nearest-neighbor 2x plane downsample, useful only as a diagnostic."""
    _validate_u16_plane(plane)
    return plane[0::2, 0::2].astype(np.uint16, copy=True)


def downsample_plane_2x(plane: np.ndarray, mode: str = "gaussian_area") -> np.ndarray:
    """Downsample one Bayer color plane by 2x."""
    if mode == "gaussian_area":
        return downsample_plane_2x_gaussian_area(plane)
    if mode == "area":
        return downsample_plane_2x_area(plane)
    if mode == "sample":
        return downsample_plane_2x_sample(plane)
    raise ValueError(f"unknown Bayer downsample mode: {mode}")


def cfa_downsample_2x(src: np.ndarray, mode: str = "gaussian_area") -> np.ndarray:
    """Downsample an RGGB/GBRG Bayer mosaic by 2x without mixing CFA planes."""
    if src.ndim != 2:
        raise ValueError(f"expected 2D Bayer image, got shape={src.shape}")
    src_h, src_w = src.shape
    if src_w % 4 != 0 or src_h % 4 != 0:
        raise ValueError(f"source dimensions must be divisible by 4, got {src_w}x{src_h}")
    if not np.issubdtype(src.dtype, np.integer):
        raise TypeError(f"expected integer Bayer dtype, got {src.dtype}")
    planes = deinterleave_bayer(src)
    low = np.stack([downsample_plane_2x(plane, mode=mode) for plane in planes], axis=0)
    return reinterleave_bayer(low)


def cfa_downsample_2x_area(src: np.ndarray) -> np.ndarray:
    """Downsample an RGGB/GBRG Bayer mosaic by 2x without mixing CFA planes.

    Each output sample is the rounded average of a 2x2 block from the same
    source color plane. For example, the output R sample at (0, 0) averages
    source R samples at (0, 0), (0, 2), (2, 0), and (2, 2). Green planes stay
    separate, so G1 and G2 are not averaged into each other.
    """
    return cfa_downsample_2x(src, mode="area")


def load_u16_raw(path: Path, width: int, height: int) -> np.ndarray:
    arr = np.fromfile(path, dtype="<u2")
    expected = width * height
    if arr.size != expected:
        raise ValueError(f"{path} has {arr.size} pixels, expected {expected}")
    return arr.reshape((height, width))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--mode", choices=("gaussian_area", "area", "sample"), default="gaussian_area")
    args = parser.parse_args()

    src = load_u16_raw(args.input, args.width, args.height)
    dst = cfa_downsample_2x(src, mode=args.mode)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    dst.astype("<u2", copy=False).tofile(args.output)
    print(f"wrote {args.output} {dst.shape[1]}x{dst.shape[0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
