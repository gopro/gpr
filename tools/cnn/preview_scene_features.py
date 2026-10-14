#!/usr/bin/env python3
"""PREVIEW scene-routing features and source discovery."""
from __future__ import annotations


from pathlib import Path

import numpy as np
from PIL import Image



import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

DEFAULT_RECEIPT = Path(
    str(external_path('artifacts/preview_runtime_policy_20260606/runtime_refiner_priority_zero_cont_colorlight_timing/preview_runtime_policy.json'))
)
DEFAULT_SOURCE_ROOTS = [
    external_path('artifacts/upresable_preview_probe_20260606/crops'),
    external_path('artifacts/display_learned_atlas_20260606'),
    external_path('artifacts/display_rgb_refiner_20260606'),
]
FEATURE_NAMES = [
    "luma_mean",
    "luma_std",
    "luma_p05",
    "luma_p95",
    "contrast_p95_p05",
    "hf_rms",
    "edge_density",
    "sat_mean",
    "sat_p95",
    "sat_frac",
    "dark_frac",
    "bright_frac",
    "r_mean",
    "g_mean",
    "b_mean",
    "rg_bias",
    "bg_bias",
]


def parse_crop_png(path: Path) -> tuple[str, str, str] | None:
    parts = path.stem.split("_")
    if len(parts) < 4:
        return None
    image_id = "_".join(parts[:2])
    if parts[2] == "center":
        return image_id, "center", "_".join(parts[3:])
    if parts[2] == "upper" and len(parts) > 4 and parts[3] == "left":
        return image_id, "upper_left", "_".join(parts[4:])
    return None


def discover_sources(source_roots: list[Path]) -> dict[tuple[str, str, str], Path]:
    out: dict[tuple[str, str, str], Path] = {}
    for root in source_roots:
        for path in root.glob("Z8Z_*_*.png"):
            parsed = parse_crop_png(path)
            if parsed is None:
                continue
            image_id, crop, variant = parsed
            out[(image_id, crop, f"{root.name}:{variant}")] = path
    return out


def load_rgb01(path: Path, max_side: int = 512) -> np.ndarray:
    image = Image.open(path).convert("RGB")
    w, h = image.size
    scale = min(1.0, max_side / max(w, h))
    if scale < 1.0:
        image = image.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.Resampling.BICUBIC)
    return np.asarray(image, dtype=np.float32) / 255.0


def rgb_to_hsv_saturation(rgb: np.ndarray) -> np.ndarray:
    r = rgb[..., 0]
    g = rgb[..., 1]
    b = rgb[..., 2]
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    return np.where(mx > 1e-6, (mx - mn) / np.maximum(mx, 1e-6), 0.0)


def feature_vector_rgb(rgb: np.ndarray, max_side: int = 512) -> np.ndarray:
    h, w = rgb.shape[:2]
    if max_side > 0 and max(h, w) > max_side:
        scale = max_side / max(h, w)
        if float(np.max(rgb)) <= 1.5:
            image_u8 = np.clip(rgb * 255.0, 0, 255).astype(np.uint8)
        else:
            image_u8 = np.clip(rgb, 0, 255).astype(np.uint8)
        image = Image.fromarray(image_u8).resize(
            (max(1, int(w * scale)), max(1, int(h * scale))),
            Image.Resampling.BICUBIC,
        )
        rgb = np.asarray(image, dtype=np.float32) / 255.0
    else:
        if rgb.dtype == np.uint8:
            rgb = rgb.astype(np.float32) / 255.0
        else:
            if rgb.dtype != np.float32 and rgb.dtype != np.float64:
                rgb = rgb.astype(np.float32)
            if float(np.max(rgb)) > 1.5:
                rgb = rgb / 255.0
    r = rgb[..., 0]
    g = rgb[..., 1]
    b = rgb[..., 2]
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    sat = rgb_to_hsv_saturation(rgb)
    blur = np.asarray(
        Image.fromarray(np.clip(luma * 255.0, 0, 255).astype(np.uint8))
        .resize((max(1, luma.shape[1] // 8), max(1, luma.shape[0] // 8)), Image.Resampling.BICUBIC)
        .resize((luma.shape[1], luma.shape[0]), Image.Resampling.BICUBIC),
        dtype=np.float32,
    ) / 255.0
    hf = luma - blur
    gx = np.diff(luma, axis=1, append=luma[:, -1:])
    gy = np.diff(luma, axis=0, append=luma[-1:, :])
    grad = np.sqrt(gx * gx + gy * gy)
    p05, p95 = [float(value) for value in np.percentile(luma, [5, 95])]
    luma_mean = float(luma.mean())
    sat_mean = float(sat.mean())
    r_mean = float(r.mean())
    g_mean = float(g.mean())
    b_mean = float(b.mean())
    return np.array(
        [
            luma_mean,
            float(luma.std()),
            p05,
            p95,
            p95 - p05,
            float(np.sqrt(np.mean(hf * hf))),
            float(np.mean(grad > 0.035)),
            sat_mean,
            float(np.percentile(sat, 95)),
            float(np.mean(sat > 0.45)),
            float(np.mean(luma < 0.12)),
            float(np.mean(luma > 0.88)),
            r_mean,
            g_mean,
            b_mean,
            r_mean - g_mean,
            b_mean - g_mean,
        ],
        dtype=np.float64,
    )


def feature_vector(path: Path) -> np.ndarray:
    return feature_vector_rgb(load_rgb01(path))
