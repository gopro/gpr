#!/usr/bin/env python3
"""Runtime helpers retained for registered quality gates; research commands are archived."""
from __future__ import annotations
import numpy as np


def deinterleave(bayer: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    return (
        bayer[0::2, 0::2],
        bayer[0::2, 1::2],
        bayer[1::2, 0::2],
        bayer[1::2, 1::2],
    )


def area_down2_plane(plane: np.ndarray) -> np.ndarray:
    h = (plane.shape[0] // 2) * 2
    w = (plane.shape[1] // 2) * 2
    p = plane[:h, :w].astype(np.uint32)
    out = (
        p[0::2, 0::2]
        + p[0::2, 1::2]
        + p[1::2, 0::2]
        + p[1::2, 1::2]
        + 2
    ) >> 2
    return out.astype(np.uint16)


def downsample_bayer_0p5x(bayer: np.ndarray) -> np.ndarray:
    planes = [area_down2_plane(p) for p in deinterleave(bayer)]
    out_h = planes[0].shape[0] * 2
    out_w = planes[0].shape[1] * 2
    out = np.zeros((out_h, out_w), dtype=np.uint16)
    out[0::2, 0::2] = planes[0]
    out[0::2, 1::2] = planes[1]
    out[1::2, 0::2] = planes[2]
    out[1::2, 1::2] = planes[3]
    return out
