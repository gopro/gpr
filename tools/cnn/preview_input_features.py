#!/usr/bin/env python3
"""PREVIEW coordinate and multiband inference input features."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import torch
from PIL import Image
from skimage.filters import gaussian


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))
sys.path.insert(0, str(REPO / "tools/cnn"))


Image.MAX_IMAGE_PIXELS = None
DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")


def coordinate_tensor(height: int, width: int) -> torch.Tensor:
    yy, xx = torch.meshgrid(
        torch.linspace(-1.0, 1.0, height),
        torch.linspace(-1.0, 1.0, width),
        indexing="ij",
    )
    return torch.stack([xx, yy], dim=0)


def source_multiband_tensor(source_low: np.ndarray) -> torch.Tensor:
    source01 = source_low.astype(np.float32) / 255.0
    height, width = source01.shape[:2]

    def blur(sigma: float) -> np.ndarray:
        out = np.empty_like(source01, dtype=np.float32)
        for channel in range(3):
            out[..., channel] = gaussian(source01[..., channel], sigma=sigma, mode="reflect", preserve_range=True)
        return out

    blur1 = blur(1.0)
    blur4 = blur(4.0)
    blur16 = blur(16.0)
    high1 = source01 - blur1
    high4 = source01 - blur4
    gray = source01.mean(axis=2)
    gy, gx = np.gradient(gray)
    grad = np.sqrt(gx * gx + gy * gy).astype(np.float32)
    lap = (np.gradient(gx, axis=1) + np.gradient(gy, axis=0)).astype(np.float32)
    planes = [
        torch.from_numpy(blur1).permute(2, 0, 1),
        torch.from_numpy(blur4).permute(2, 0, 1),
        torch.from_numpy(blur16).permute(2, 0, 1),
        torch.from_numpy(high1).permute(2, 0, 1),
        torch.from_numpy(high4).permute(2, 0, 1),
        torch.from_numpy(np.abs(high1)).permute(2, 0, 1),
        torch.from_numpy(np.abs(high4)).permute(2, 0, 1),
        torch.from_numpy(grad[None, ...]),
        torch.from_numpy(lap[None, ...]),
    ]
    assert all(tuple(plane.shape[-2:]) == (height, width) for plane in planes)
    return torch.cat(planes, dim=0).float()
