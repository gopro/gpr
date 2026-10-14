#!/usr/bin/env python3
"""Runtime model and inference helpers; experiment commands are archived."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))
sys.path.insert(0, str(REPO / "tools/cnn"))

from build_preview_holdout_runtime_receipt import scaled_box
from preview_scene_features import feature_vector_rgb  # noqa: E402


Image.MAX_IMAGE_PIXELS = None


def load_source_receipt(path: Path) -> dict[str, Path]:
    payload = json.loads(path.read_text())
    root = path.parent
    out: dict[str, Path] = {}
    for image in payload.get("images") or []:
        source = root / str(image["stitched_png"])
        if not source.exists():
            raise FileNotFoundError(f"missing q8 source fullframe {source}")
        out[str(image["image_id"])] = source
    return out


def feature_schema(crop_names: list[str]) -> dict[str, Any]:
    return {
        "schema": "preview_q8_hard_router_features.v1",
        "source": "q8_source_fullframe_rgb",
        "regions": ["full_image", *[f"manifest_crop:{name}" for name in crop_names]],
        "per_region_features": [
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
        ],
        "forbidden_router_inputs": ["ref_rgb", "ref_dng", "gate_metrics", "sample_index", "crop_identity_key_planes"],
    }


def feature_for_image(source_rgb: np.ndarray, manifest_image: dict[str, Any], crops: dict[str, dict[str, int]], crop_names: list[str]) -> np.ndarray:
    feats = [feature_vector_rgb(source_rgb, max_side=512).astype(np.float64)]
    for crop_name in crop_names:
        crop = crops[crop_name]
        box = scaled_box(crop, manifest_image["sensor_dims"], (source_rgb.shape[1], source_rgb.shape[0]))
        crop_rgb = np.asarray(Image.fromarray(source_rgb).crop(box).resize((512, 512), Image.Resampling.LANCZOS), dtype=np.uint8)
        feats.append(feature_vector_rgb(crop_rgb, max_side=512).astype(np.float64))
    return np.concatenate(feats)


def build_features(args: argparse.Namespace) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    manifest = json.loads(args.manifest.read_text())
    manifest_images = {str(image["id"]): image for image in manifest["images"]}
    crops = {str(key): value for key, value in manifest["crops"].items() if not str(key).startswith("$")}
    crop_names = sorted(crops)
    source_paths = load_source_receipt(args.source_fullframe_receipt)
    hard_payload = json.loads(args.q8_hard_receipt.read_text())
    hard_ids = sorted({str(row["image_id"]) for row in hard_payload.get("rows") or []})
    rows: list[dict[str, Any]] = []
    for image_id in sorted(source_paths):
        source_rgb = np.asarray(Image.open(source_paths[image_id]).convert("RGB"), dtype=np.uint8)
        feature = feature_for_image(source_rgb, manifest_images[image_id], crops, crop_names)
        rows.append(
            {
                "image_id": image_id,
                "feature": feature,
                "is_hard": image_id in hard_ids,
                "source_png": str(source_paths[image_id]),
            }
        )
    return rows, {"crop_names": crop_names, "hard_image_ids": hard_ids, "feature_schema": feature_schema(crop_names)}


def normalize(features: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    mean = features.mean(axis=0)
    std = features.std(axis=0)
    std[std < 1e-6] = 1.0
    return (features - mean) / std, mean, std
