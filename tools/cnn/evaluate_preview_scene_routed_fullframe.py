#!/usr/bin/env python3
"""Evaluate tiled full-frame PREVIEW scene routing.

This diagnostic renders full source/REF images, runs the scene-routed PREVIEW
policy on overlapped source tiles, stitches the full output, and scores the
manifest crops from that stitched output. REF is used only for scoring.
"""
from __future__ import annotations


import argparse
import json
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image, ImageFilter




sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))
sys.path.insert(0, str(REPO / "tools/cnn"))

from build_preview_holdout_runtime_receipt import resolve_ref, resolve_source, scaled_box  # noqa: E402
from preview_scene_features import feature_vector_rgb  # noqa: E402
from evaluate_preview_runtime_policy import build_input, load_rgb, summarize, write_html  # noqa: E402
from evaluate_preview_scene_routed import (  # noqa: E402
    parse_cluster_checkpoint,
    parse_cluster_conditioning,
    parse_override_checkpoint,
    parse_override_conditioning,
    route_features_from_sidecar,
    route_from_sidecar,
    sha256_file,
)
from metrics import compute_visual_metrics  # noqa: E402
from preview_rgb_models import DirectRGBRefiner, build_rgb_refiner, pass_preview  # noqa: E402


DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")


def parse_spatial_checkpoint(values: list[str]) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--spatial-checkpoint must be NAME=PATH, got {value!r}")
        name, path = value.split("=", 1)
        if not name:
            raise ValueError("--spatial-checkpoint name cannot be empty")
        out[name] = Path(path)
    return out


def parse_spatial_region(values: list[str]) -> list[dict[str, Any]]:
    regions: list[dict[str, Any]] = []
    for value in values:
        if "=" not in value:
            raise ValueError(f"--spatial-region must be NAME=X0,Y0,X1,Y1[,CLUSTER], got {value!r}")
        name, spec = value.split("=", 1)
        parts = spec.split(",")
        if len(parts) not in {4, 5}:
            raise ValueError(f"--spatial-region must have 4 normalized bounds plus optional cluster, got {value!r}")
        x0, y0, x1, y1 = [float(v) for v in parts[:4]]
        if not (0.0 <= x0 < x1 <= 1.0 and 0.0 <= y0 < y1 <= 1.0):
            raise ValueError(f"spatial bounds must be normalized and ordered, got {value!r}")
        regions.append(
            {
                "name": name,
                "x0": x0,
                "y0": y0,
                "x1": x1,
                "y1": y1,
                "cluster": int(parts[4]) if len(parts) == 5 and parts[4] else None,
            }
        )
    return regions


def parse_spatial_conditioning(values: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--spatial-conditioning must be NAME=MODE, got {value!r}")
        name, mode = value.split("=", 1)
        if mode not in {"zero", "content_stats", "color_stats", "global_color_stats"}:
            raise ValueError(f"unsupported spatial conditioning mode for {name}: {mode!r}")
        out[name] = mode
    return out


def parse_spatial_scene_role_threshold(values: list[str]) -> dict[str, dict[str, int]]:
    out: dict[str, dict[str, int]] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--spatial-scene-role-min must be NAME=ROLE,COUNT, got {value!r}")
        name, spec = value.split("=", 1)
        parts = spec.split(",")
        if len(parts) != 2:
            raise ValueError(f"--spatial-scene-role-min must be NAME=ROLE,COUNT, got {value!r}")
        role, count_text = parts
        if not name or not role:
            raise ValueError(f"--spatial-scene-role-min needs non-empty NAME and ROLE, got {value!r}")
        count = int(count_text)
        if count < 0:
            raise ValueError(f"--spatial-scene-role-min count must be non-negative, got {value!r}")
        out.setdefault(name, {})[role] = count
    return out


def parse_tile_offsets(values: list[str]) -> list[tuple[int, int]]:
    if not values:
        return [(0, 0)]
    offsets: list[tuple[int, int]] = []
    for value in values:
        parts = value.split(",")
        if len(parts) != 2:
            raise ValueError(f"--tile-offset must be X,Y pixels, got {value!r}")
        x, y = int(parts[0]), int(parts[1])
        if x < 0 or y < 0:
            raise ValueError(f"--tile-offset values must be non-negative, got {value!r}")
        offsets.append((x, y))
    return offsets


def max_rss_mb() -> float:
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if sys.platform == "darwin":
        return float(rss) / (1024.0 * 1024.0)
    return float(rss) / 1024.0


def mps_memory_mb() -> dict[str, float]:
    if not hasattr(torch, "mps") or not torch.backends.mps.is_available():
        return {}
    out: dict[str, float] = {}
    for name in ("current_allocated_memory", "driver_allocated_memory"):
        fn = getattr(torch.mps, name, None)
        if callable(fn):
            out[name.replace("_memory", "_mb")] = float(fn()) / (1024.0 * 1024.0)
    return out


def render_dng_to_tiff(dng_path: Path, tiff_path: Path) -> float:
    t0 = time.perf_counter()
    result = subprocess.run(
        ["sips", "-s", "format", "tiff", str(dng_path), "--out", str(tiff_path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"sips failed for {dng_path}: {result.stderr[-400:]}")
    return (time.perf_counter() - t0) * 1000.0


def load_model(path: Path) -> DirectRGBRefiner:
    ckpt = torch.load(str(path), map_location="cpu", weights_only=False)
    model = build_rgb_refiner(
        str(ckpt.get("architecture", "direct")),
        width=int(ckpt.get("width", 40)),
        in_channels=int(ckpt.get("in_channels", 9)),
        residual_scale=float(ckpt.get("residual_scale", 0.5)),
    ).to(DEVICE)
    model.load_state_dict(ckpt["state_dict"])
    model.eval()
    return model


def load_model_with_receipt(path: Path) -> tuple[DirectRGBRefiner, dict[str, Any]]:
    ckpt = torch.load(str(path), map_location="cpu", weights_only=False)
    model = build_rgb_refiner(
        str(ckpt.get("architecture", "direct")),
        width=int(ckpt.get("width", 40)),
        in_channels=int(ckpt.get("in_channels", 9)),
        residual_scale=float(ckpt.get("residual_scale", 0.5)),
    ).to(DEVICE)
    model.load_state_dict(ckpt["state_dict"])
    model.eval()
    return model, ckpt


def run_model_padded(model: DirectRGBRefiner, x: torch.Tensor, multiple: int = 4) -> torch.Tensor:
    """Pad non-grid-sized diagnostic tiles so stride-2 skip paths line up."""
    height, width = x.shape[-2:]
    pad_h = (multiple - height % multiple) % multiple
    pad_w = (multiple - width % multiple) % multiple
    if pad_h or pad_w:
        x = F.pad(x, (0, pad_w, 0, pad_h), mode="replicate")
    pred = model(x)
    return pred[..., :height, :width]


def tile_origins(length: int, tile: int, stride: int) -> list[int]:
    if length <= tile:
        return [0]
    out = list(range(0, max(1, length - tile + 1), stride))
    last = length - tile
    if out[-1] != last:
        out.append(last)
    return out


def tile_origins_with_offset(length: int, tile: int, stride: int, offset: int) -> list[int]:
    if length <= tile:
        return [0]
    last = length - tile
    offset = int(offset) % max(1, stride)
    starts = {0, last}
    pos = offset
    while pos <= last:
        starts.add(pos)
        pos += stride
    return sorted(starts)


def tile_weight(height: int, width: int) -> np.ndarray:
    wy = np.hanning(height).astype(np.float32)
    wx = np.hanning(width).astype(np.float32)
    if height <= 2:
        wy = np.ones(height, dtype=np.float32)
    if width <= 2:
        wx = np.ones(width, dtype=np.float32)
    weight = np.maximum(wy[:, None] * wx[None, :], 1e-3)
    return weight[..., None]


def source_frequency_planes(source_rgb: np.ndarray, mode: str, blur_radius: float) -> list[np.ndarray]:
    if mode == "none":
        return []
    if mode != "low_high":
        raise ValueError(f"unsupported source frequency plane mode {mode!r}")
    source = source_rgb.astype(np.float32) / 255.0
    low_rgb = np.asarray(
        Image.fromarray(source_rgb).filter(ImageFilter.GaussianBlur(radius=float(blur_radius))),
        dtype=np.float32,
    ) / 255.0
    high_rgb = source - low_rgb
    return [
        np.transpose(low_rgb, (2, 0, 1)).astype(np.float32),
        np.transpose(high_rgb, (2, 0, 1)).astype(np.float32),
    ]


def build_tile_input(
    source_rgb: np.ndarray,
    conditioning: str,
    coordinate_mode: str,
    xywh: tuple[int, int, int, int],
    full_size: tuple[int, int],
    global_stats: dict[str, float] | None = None,
    source_frequency_mode: str = "none",
    source_frequency_blur: float = 2.0,
) -> torch.Tensor:
    if coordinate_mode == "local" and source_frequency_mode == "none":
        return build_input(source_rgb, conditioning)
    if coordinate_mode not in {"global_tile", "zero_coord"}:
        raise ValueError(f"unsupported coordinate mode {coordinate_mode!r}")
    height, width = source_rgb.shape[:2]
    x0, y0, _w, _h = xywh
    full_w, full_h = full_size
    if coordinate_mode == "zero_coord":
        yy = np.zeros((height, width), dtype=np.float32)
        xx = np.zeros((height, width), dtype=np.float32)
    else:
        yy, xx = np.meshgrid(
            (np.arange(height, dtype=np.float32) + float(y0)) / max(1.0, float(full_h - 1)),
            (np.arange(width, dtype=np.float32) + float(x0)) / max(1.0, float(full_w - 1)),
            indexing="ij",
        )
    source = np.transpose(source_rgb.astype(np.float32) / 255.0, (2, 0, 1))
    key_planes = np.zeros((4, height, width), dtype=np.float32)
    if conditioning == "zero":
        pass
    elif conditioning == "content_stats":
        gray = source.mean(axis=0)
        key_planes[0].fill(float(gray.mean()))
        key_planes[1].fill(float(gray.std()))
        key_planes[2].fill(float(np.percentile(gray, 95) - np.percentile(gray, 5)))
    elif conditioning == "color_stats":
        gray = source.mean(axis=0)
        key_planes[0].fill(float(source[0].mean()))
        key_planes[1].fill(float(source[1].mean()))
        key_planes[2].fill(float(source[2].mean()))
        key_planes[3].fill(float(gray.std()))
    elif conditioning == "global_color_stats":
        if global_stats is None:
            gray = source.mean(axis=0)
            global_stats = {
                "r_mean": float(source[0].mean()),
                "g_mean": float(source[1].mean()),
                "b_mean": float(source[2].mean()),
                "gray_std": float(gray.std()),
            }
        key_planes[0].fill(float(global_stats["r_mean"]))
        key_planes[1].fill(float(global_stats["g_mean"]))
        key_planes[2].fill(float(global_stats["b_mean"]))
        key_planes[3].fill(float(global_stats["gray_std"]))
    else:
        raise ValueError(f"unsupported conditioning {conditioning!r}")
    planes = [source, np.stack([xx, yy], axis=0), key_planes]
    planes.extend(source_frequency_planes(source_rgb, source_frequency_mode, source_frequency_blur))
    arr = np.concatenate(planes, axis=0)
    return torch.from_numpy(arr[None].copy()).to(DEVICE).contiguous()


def global_rgb_stats(rgb: np.ndarray) -> dict[str, float]:
    normalized = rgb.astype(np.float32) / 255.0
    return {
        "r_mean": float(normalized[:, :, 0].mean()),
        "g_mean": float(normalized[:, :, 1].mean()),
        "b_mean": float(normalized[:, :, 2].mean()),
        "gray_std": float(normalized.mean(axis=2).std()),
    }


def apply_post_refiner(
    *,
    image_rgb: np.ndarray,
    model: DirectRGBRefiner,
    conditioning: str,
    coordinate_mode: str,
    tile_size: int,
    overlap: int,
    source_frequency_mode: str = "none",
    source_frequency_blur: float = 2.0,
) -> tuple[np.ndarray, dict[str, Any]]:
    height, width = image_rgb.shape[:2]
    stride = max(1, tile_size - overlap)
    stats = global_rgb_stats(image_rgb)
    out_acc = np.zeros((height, width, 3), dtype=np.float32)
    weight_acc = np.zeros((height, width, 1), dtype=np.float32)
    input_ms: list[float] = []
    model_ms: list[float] = []
    tile_count = 0
    for y0 in tile_origins(height, tile_size, stride):
        for x0 in tile_origins(width, tile_size, stride):
            y1 = min(height, y0 + tile_size)
            x1 = min(width, x0 + tile_size)
            tile_rgb = image_rgb[y0:y1, x0:x1]
            t0 = time.perf_counter()
            x = build_tile_input(
                tile_rgb,
                conditioning,
                coordinate_mode,
                (x0, y0, x1 - x0, y1 - y0),
                (width, height),
                stats,
                source_frequency_mode,
                source_frequency_blur,
            )
            t1 = time.perf_counter()
            with torch.no_grad():
                pred = run_model_padded(model, x).detach().cpu().numpy()[0]
            t2 = time.perf_counter()
            pred_rgb = np.clip(np.transpose(pred, (1, 2, 0)) * 255.0, 0, 255).astype(np.uint8)
            w = tile_weight(pred_rgb.shape[0], pred_rgb.shape[1])
            out_acc[y0:y1, x0:x1] += pred_rgb.astype(np.float32) * w
            weight_acc[y0:y1, x0:x1] += w
            input_ms.append((t1 - t0) * 1000.0)
            model_ms.append((t2 - t1) * 1000.0)
            tile_count += 1
    refined = np.clip(out_acc / np.maximum(weight_acc, 1e-6), 0, 255).astype(np.uint8)
    return refined, {
        "tile_count": tile_count,
        "tile_size": tile_size,
        "overlap": overlap,
        "conditioning": conditioning,
        "coordinate_mode": coordinate_mode,
        "source_frequency_planes": source_frequency_mode,
        "source_frequency_blur": source_frequency_blur,
        "input_ms_median": float(statistics.median(input_ms)) if input_ms else 0.0,
        "model_ms_median": float(statistics.median(model_ms)) if model_ms else 0.0,
        "model_ms_total": float(sum(model_ms)),
    }


def select_model(
    *,
    source_path: Path,
    base_sidecar: dict[str, Any],
    override_sidecars: list[tuple[Path, dict[str, Any]]],
    default_checkpoint: Path,
    cluster_ckpts: dict[int, Path],
    override_ckpts: dict[tuple[int | None, int], Path],
    cluster_conditioning: dict[int, str],
    override_conditioning: dict[tuple[int | None, int], str],
    default_conditioning: str,
) -> tuple[int, int | None, str, Path, str, dict[str, Any]]:
    cluster, route = route_from_sidecar(source_path, base_sidecar)
    ckpt_path = cluster_ckpts.get(cluster, default_checkpoint)
    model_key = f"cluster_{cluster}" if cluster in cluster_ckpts else "default"
    conditioning = cluster_conditioning.get(cluster, default_conditioning)
    override_cluster = None
    if override_sidecars:
        trace = []
        for override_index, (_path, override_sidecar) in enumerate(override_sidecars):
            routed_cluster, override_route = route_from_sidecar(source_path, override_sidecar)
            trace.append(
                {
                    "index": override_index,
                    "cluster": routed_cluster,
                    "route_source": override_route["route_source"],
                    "route_distance": override_route["route_distance"],
                }
            )
            override_key = (override_index, routed_cluster)
            legacy_key = (None, routed_cluster)
            matched_key = override_key if override_key in override_ckpts else legacy_key
            if matched_key in override_ckpts:
                override_cluster = routed_cluster
                ckpt_path = override_ckpts[matched_key]
                model_key = (
                    f"override_{override_index}_cluster_{override_cluster}"
                    if override_key in override_ckpts
                    else f"override_cluster_{override_cluster}"
                )
                conditioning = override_conditioning.get(
                    matched_key,
                    override_conditioning.get(legacy_key, conditioning),
                )
                route["override_route_source"] = override_route["route_source"]
                route["override_route_distance"] = override_route["route_distance"]
                route["override_router_index"] = override_index
                break
        route["override_trace"] = trace
    return cluster, override_cluster, model_key, ckpt_path, conditioning, route


def select_model_features(
    *,
    features: np.ndarray,
    base_sidecar: dict[str, Any],
    override_sidecars: list[tuple[Path, dict[str, Any]]],
    default_checkpoint: Path,
    cluster_ckpts: dict[int, Path],
    override_ckpts: dict[tuple[int | None, int], Path],
    cluster_conditioning: dict[int, str],
    override_conditioning: dict[tuple[int | None, int], str],
    default_conditioning: str,
) -> tuple[int, int | None, str, Path, str, dict[str, Any]]:
    cluster, route = route_features_from_sidecar(features, base_sidecar)
    ckpt_path = cluster_ckpts.get(cluster, default_checkpoint)
    model_key = f"cluster_{cluster}" if cluster in cluster_ckpts else "default"
    conditioning = cluster_conditioning.get(cluster, default_conditioning)
    override_cluster = None
    if override_sidecars:
        trace = []
        for override_index, (_path, override_sidecar) in enumerate(override_sidecars):
            routed_cluster, override_route = route_features_from_sidecar(features, override_sidecar)
            trace.append(
                {
                    "index": override_index,
                    "cluster": routed_cluster,
                    "route_source": override_route["route_source"],
                    "route_distance": override_route["route_distance"],
                }
            )
            override_key = (override_index, routed_cluster)
            legacy_key = (None, routed_cluster)
            matched_key = override_key if override_key in override_ckpts else legacy_key
            if matched_key in override_ckpts:
                override_cluster = routed_cluster
                ckpt_path = override_ckpts[matched_key]
                model_key = (
                    f"override_{override_index}_cluster_{override_cluster}"
                    if override_key in override_ckpts
                    else f"override_cluster_{override_cluster}"
                )
                conditioning = override_conditioning.get(
                    matched_key,
                    override_conditioning.get(legacy_key, conditioning),
                )
                route["override_route_source"] = override_route["route_source"]
                route["override_route_distance"] = override_route["route_distance"]
                route["override_router_index"] = override_index
                break
        route["override_trace"] = trace
    return cluster, override_cluster, model_key, ckpt_path, conditioning, route


def apply_spatial_override(
    *,
    regions: list[dict[str, Any]],
    conditioning: dict[str, str],
    cluster: int,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
    width: int,
    height: int,
    model_key: str,
    current_conditioning: str,
    enabled_names: set[str] | None = None,
) -> tuple[str, str, dict[str, Any]]:
    center_x = (float(x0) + float(x1)) * 0.5 / max(1.0, float(width))
    center_y = (float(y0) + float(y1)) * 0.5 / max(1.0, float(height))
    for region in regions:
        name = str(region["name"])
        if enabled_names is not None and name not in enabled_names:
            continue
        expected_cluster = region.get("cluster")
        if expected_cluster is not None and int(expected_cluster) != int(cluster):
            continue
        if region["x0"] <= center_x < region["x1"] and region["y0"] <= center_y < region["y1"]:
            key = f"spatial_{name}"
            return key, conditioning.get(name, current_conditioning), {
                "spatial_override": name,
                "spatial_center": [center_x, center_y],
                "spatial_cluster_constraint": expected_cluster,
            }
    return model_key, current_conditioning, {"spatial_override": None, "spatial_center": [center_x, center_y]}


def route_tile_role(
    *,
    args: argparse.Namespace,
    tile_rgb: np.ndarray,
    routing: dict[str, Any],
) -> tuple[int, int | None, str, str, dict[str, Any]]:
    if args.force_model_key:
        return -1, None, args.force_model_key, args.force_conditioning, {
            "route_source": "forced_diagnostic_model_key",
            "route_distance": 0.0,
            "route_feature_ms": 0.0,
            "route_select_ms": 0.0,
        }
    t0 = time.perf_counter()
    features = feature_vector_rgb(tile_rgb, max_side=int(args.route_feature_max_side))
    t1 = time.perf_counter()
    cluster, override_cluster, model_key, _ckpt, conditioning, route = select_model_features(
        features=features,
        base_sidecar=routing["base_sidecar"],
        override_sidecars=routing["override_sidecars"],
        default_checkpoint=routing["default_checkpoint"],
        cluster_ckpts=routing["cluster_ckpts"],
        override_ckpts=routing["override_ckpts"],
        cluster_conditioning=routing["cluster_conditioning"],
        override_conditioning=routing["override_conditioning"],
        default_conditioning=routing["default_conditioning"],
    )
    t2 = time.perf_counter()
    route["route_feature_ms"] = (t1 - t0) * 1000.0
    route["route_select_ms"] = (t2 - t1) * 1000.0
    return cluster, override_cluster, model_key, conditioning, route


def scene_spatial_enabled_names(role_counts: dict[str, int], thresholds: dict[str, dict[str, int]]) -> set[str] | None:
    if not thresholds:
        return None
    enabled: set[str] = set()
    for name, required in thresholds.items():
        if all(role_counts.get(role, 0) >= count for role, count in required.items()):
            enabled.add(name)
    return enabled


def tile_center(spec: tuple[int, int, int, int, str]) -> tuple[float, float]:
    x0, y0, x1, y1, _label = spec
    return (float(x0 + x1) * 0.5, float(y0 + y1) * 0.5)


def route_cache_key(spec: tuple[int, int, int, int, str]) -> tuple[int, int, int, int, str]:
    x0, y0, x1, y1, tile_label = spec
    return (x0, y0, x1, y1, tile_label)


def route_result_for_tile(
    *,
    args: argparse.Namespace,
    spec: tuple[int, int, int, int, str],
    source_rgb: np.ndarray,
    width: int,
    height: int,
    routing: dict[str, Any],
    route_cache: dict[
        tuple[int, int, int, int, str],
        tuple[int, int | None, str, str, dict[str, Any]],
    ],
) -> tuple[int, int | None, str, str, dict[str, Any]]:
    x0, y0, x1, y1, _tile_label = spec
    key = route_cache_key(spec)
    if key in route_cache:
        return route_cache[key]
    pad = max(0, int(args.model_context_padding))
    route_pad = max(pad, int(args.route_context_padding))
    rx0 = max(0, x0 - route_pad)
    ry0 = max(0, y0 - route_pad)
    rx1 = min(width, x1 + route_pad)
    ry1 = min(height, y1 + route_pad)
    route_rgb = source_rgb[ry0:ry1, rx0:rx1]
    result = route_tile_role(
        args=args,
        tile_rgb=route_rgb,
        routing=routing,
    )
    route_cache[key] = result
    return result


def build_route_smoothing_cache(
    *,
    args: argparse.Namespace,
    tile_specs: list[tuple[int, int, int, int, str]],
    source_rgb: np.ndarray,
    width: int,
    height: int,
    routing: dict[str, Any],
    route_cache: dict[
        tuple[int, int, int, int, str],
        tuple[int, int | None, str, str, dict[str, Any]],
    ],
) -> dict[tuple[int, int, int, int, str], tuple[int, int | None, str, str, dict[str, Any]]]:
    radius = float(args.route_smoothing_radius)
    if radius <= 0.0 or args.force_model_key:
        return {}
    min_fraction = float(args.route_smoothing_min_fraction)
    centers = {route_cache_key(spec): tile_center(spec) for spec in tile_specs}
    for spec in tile_specs:
        route_result_for_tile(
            args=args,
            spec=spec,
            source_rgb=source_rgb,
            width=width,
            height=height,
            routing=routing,
            route_cache=route_cache,
        )
    smoothed: dict[tuple[int, int, int, int, str], tuple[int, int | None, str, str, dict[str, Any]]] = {}
    for spec in tile_specs:
        key = route_cache_key(spec)
        cx, cy = centers[key]
        neighbor_keys = [
            other_key
            for other_key, (nx, ny) in centers.items()
            if abs(nx - cx) <= radius and abs(ny - cy) <= radius
        ]
        role_counts: dict[str, int] = {}
        for other_key in neighbor_keys:
            _cluster, _override_cluster, model_key, _conditioning, _route = route_cache[other_key]
            role_counts[model_key] = role_counts.get(model_key, 0) + 1
        if not role_counts:
            continue
        selected_model, selected_count = sorted(role_counts.items(), key=lambda item: (-item[1], item[0]))[0]
        selected_fraction = float(selected_count) / float(len(neighbor_keys))
        if selected_fraction < min_fraction:
            continue
        selected_key = next(
            other_key
            for other_key in neighbor_keys
            if route_cache[other_key][2] == selected_model
        )
        cluster, override_cluster, model_key, conditioning, route = route_cache[selected_key]
        original_cluster, original_override, original_model, original_conditioning, original_route = route_cache[key]
        route = dict(route)
        route.update(
            {
                "route_smoothing": "local_majority",
                "route_smoothing_radius": radius,
                "route_smoothing_min_fraction": min_fraction,
                "route_smoothing_neighbor_count": len(neighbor_keys),
                "route_smoothing_selected_fraction": selected_fraction,
                "route_smoothing_original_model_key": original_model,
                "route_smoothing_original_conditioning": original_conditioning,
                "route_smoothing_original_cluster": original_cluster,
                "route_smoothing_original_override_cluster": original_override,
                "route_smoothing_changed": selected_model != original_model,
                "route_smoothing_original_route_source": original_route.get("route_source"),
            }
        )
        smoothed[key] = (cluster, override_cluster, model_key, conditioning, route)
    return smoothed


def summarize_image_timing(image_receipts: list[dict[str, Any]]) -> dict[str, Any]:
    timings = [receipt["timing"] for receipt in image_receipts]

    def avg(key: str) -> float:
        return float(sum(float(timing.get(key, 0.0)) for timing in timings) / len(timings))

    def min_value(key: str) -> float:
        return float(min(float(timing.get(key, 0.0)) for timing in timings))

    def max_value(key: str) -> float:
        return float(max(float(timing.get(key, 0.0)) for timing in timings))

    runtime_avg = avg("runtime_no_ref_wall_ms")
    return {
        "image_count": len(image_receipts),
        "runtime_no_ref_wall_ms_avg": runtime_avg,
        "runtime_no_ref_wall_ms_min": min_value("runtime_no_ref_wall_ms"),
        "runtime_no_ref_wall_ms_max": max_value("runtime_no_ref_wall_ms"),
        "runtime_no_ref_fps_avg": 1000.0 / runtime_avg if runtime_avg > 0 else 0.0,
        "model_ms_total_avg": avg("model_ms_total"),
        "source_render_ms_avg": avg("source_render_ms"),
        "scoring_wall_ms_avg": avg("scoring_wall_ms"),
        "total_eval_wall_ms_avg": avg("total_eval_wall_ms"),
    }


def summarize_quality_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    if rows:
        return summarize(rows)
    return {
        "count": 0,
        "pass_count": 0,
        "pass_rate": 0.0,
        "worst_lpips": 0.0,
        "median_lpips": 0.0,
        "worst_ms_ssim": 0.0,
        "worst_y_psnr": 0.0,
        "worst_dE2000_mean": 0.0,
        "quality_scoring": "skipped",
    }


def crop_metric_image(image: np.ndarray, crop: dict[str, int], sensor_dims: list[int]) -> np.ndarray:
    box = scaled_box(crop, sensor_dims, (image.shape[1], image.shape[0]))
    pil = Image.fromarray(image).crop(box)
    if pil.size != (512, 512):
        pil = pil.resize((512, 512), Image.Resampling.LANCZOS)
    return np.asarray(pil.convert("RGB"), dtype=np.uint8)


def stitched_output_spec(name: str) -> tuple[str, str, dict[str, Any]]:
    if name == "png":
        return ".png", "PNG", {}
    if name == "png_fast":
        return ".png", "PNG", {"compress_level": 1}
    if name == "png_store":
        return ".png", "PNG", {"compress_level": 0}
    if name == "tiff_raw":
        return ".tiff", "TIFF", {"compression": "raw"}
    if name == "bmp":
        return ".bmp", "BMP", {}
    raise ValueError(f"unsupported stitched output format {name!r}")


def render_full_image(args: argparse.Namespace, image: dict[str, Any], work: Path, models: dict[str, DirectRGBRefiner], routing: dict[str, Any]) -> dict[str, Any]:
    image_wall_start = time.perf_counter()
    image_id = str(image["id"])
    manifest = json.loads(args.manifest.read_text())
    ref_dng = None if args.skip_quality_scoring else resolve_ref(image, args.ref_root)
    source_dng = resolve_source(image_id, args.source_root)
    if source_dng is None:
        raise FileNotFoundError(f"missing source DNG for {image_id}")
    ref_tiff = work / f"{image_id}_REF.tiff" if ref_dng is not None else None
    source_tiff = work / f"{image_id}_source.tiff"
    ref_render_ms = render_dng_to_tiff(ref_dng, ref_tiff) if ref_dng is not None and ref_tiff is not None else 0.0
    runtime_wall_start = time.perf_counter()
    source_render_ms = render_dng_to_tiff(source_dng, source_tiff)
    t0 = time.perf_counter()
    source_rgb = load_rgb(source_tiff)
    source_load_ms = (time.perf_counter() - t0) * 1000.0
    height, width = source_rgb.shape[:2]
    source_global_stats = global_rgb_stats(source_rgb)
    tile = int(args.tile_size)
    stride = max(1, tile - int(args.overlap))
    out_acc = np.zeros((height, width, 3), dtype=np.float32)
    weight_acc = np.zeros((height, width, 1), dtype=np.float32)
    tile_rows: list[dict[str, Any]] = []
    model_ms: list[float] = []
    model_batch_sizes: list[int] = []
    input_ms: list[float] = []
    route_ms: list[float] = []
    route_feature_ms: list[float] = []
    route_select_ms: list[float] = []
    save_tile_ms: list[float] = []
    tile_jobs: list[dict[str, Any]] = []
    route_cache: dict[
        tuple[int, int, int, int, str],
        tuple[int, int | None, str, str, dict[str, Any]],
    ] = {}
    if args.tile_mode == "manifest_crops":
        tile_specs = []
        for crop_name, crop in manifest["crops"].items():
            if crop_name.startswith("$"):
                continue
            x0, y0, x1, y1 = scaled_box(crop, image["sensor_dims"], (width, height))
            tile_specs.append((x0, y0, x1, y1, crop_name))
    else:
        tile_specs = []
        seen_tiles: set[tuple[int, int, int, int]] = set()
        for offset_x, offset_y in args.tile_offsets:
            label = f"grid_o{offset_x}_{offset_y}"
            for y0 in tile_origins_with_offset(height, tile, stride, offset_y):
                for x0 in tile_origins_with_offset(width, tile, stride, offset_x):
                    x1 = min(width, x0 + tile)
                    y1 = min(height, y0 + tile)
                    key = (x0, y0, x1, y1)
                    if key in seen_tiles:
                        continue
                    seen_tiles.add(key)
                    tile_specs.append((x0, y0, x1, y1, label))
    scene_role_counts: dict[str, int] = {}
    spatial_enabled_names: set[str] | None = None
    scene_route_ms: list[float] = []
    scene_route_feature_ms: list[float] = []
    scene_route_select_ms: list[float] = []
    route_smoothing_precompute_ms = 0.0
    if routing["spatial_scene_role_min"]:
        for x0, y0, x1, y1, _tile_label in tile_specs:
            route_pad = max(max(0, int(args.model_context_padding)), int(args.route_context_padding))
            rx0 = max(0, x0 - route_pad)
            ry0 = max(0, y0 - route_pad)
            rx1 = min(width, x1 + route_pad)
            ry1 = min(height, y1 + route_pad)
            route_rgb = source_rgb[ry0:ry1, rx0:rx1]
            t0 = time.perf_counter()
            route_result = route_tile_role(
                args=args,
                tile_rgb=route_rgb,
                routing=routing,
            )
            _cluster, _override_cluster, model_key, _conditioning, _route = route_result
            scene_route_ms.append((time.perf_counter() - t0) * 1000.0)
            scene_route_feature_ms.append(float(_route.get("route_feature_ms", 0.0)))
            scene_route_select_ms.append(float(_route.get("route_select_ms", 0.0)))
            scene_role_counts[model_key] = scene_role_counts.get(model_key, 0) + 1
            route_cache[(x0, y0, x1, y1, _tile_label)] = route_result
        spatial_enabled_names = scene_spatial_enabled_names(scene_role_counts, routing["spatial_scene_role_min"])
    t0 = time.perf_counter()
    route_smoothing_cache = build_route_smoothing_cache(
        args=args,
        tile_specs=tile_specs,
        source_rgb=source_rgb,
        width=width,
        height=height,
        routing=routing,
        route_cache=route_cache,
    )
    route_smoothing_precompute_ms = (time.perf_counter() - t0) * 1000.0
    for x0, y0, x1, y1, tile_label in tile_specs:
            pad = max(0, int(args.model_context_padding))
            route_pad = max(pad, int(args.route_context_padding))
            cx0 = max(0, x0 - pad)
            cy0 = max(0, y0 - pad)
            cx1 = min(width, x1 + pad)
            cy1 = min(height, y1 + pad)
            rx0 = max(0, x0 - route_pad)
            ry0 = max(0, y0 - route_pad)
            rx1 = min(width, x1 + route_pad)
            ry1 = min(height, y1 + route_pad)
            tile_rgb = source_rgb[y0:y1, x0:x1]
            context_rgb = source_rgb[cy0:cy1, cx0:cx1]
            route_rgb = source_rgb[ry0:ry1, rx0:rx1]
            t0 = time.perf_counter()
            route_key = (x0, y0, x1, y1, tile_label)
            if route_key in route_smoothing_cache:
                cluster, override_cluster, model_key, conditioning, route = route_smoothing_cache[route_key]
            elif route_key in route_cache:
                cluster, override_cluster, model_key, conditioning, route = route_cache[route_key]
            else:
                cluster, override_cluster, model_key, conditioning, route = route_tile_role(
                    args=args,
                    tile_rgb=route_rgb,
                    routing=routing,
                )
                route_feature_ms.append(float(route.get("route_feature_ms", 0.0)))
                route_select_ms.append(float(route.get("route_select_ms", 0.0)))
            if not args.force_model_key:
                model_key, conditioning, spatial_route = apply_spatial_override(
                    regions=routing["spatial_regions"],
                    conditioning=routing["spatial_conditioning"],
                    cluster=cluster,
                    x0=x0,
                    y0=y0,
                    x1=x1,
                    y1=y1,
                    width=width,
                    height=height,
                    model_key=model_key,
                    current_conditioning=conditioning,
                    enabled_names=spatial_enabled_names,
                )
                route.update(spatial_route)
            route_ms.append((time.perf_counter() - t0) * 1000.0 if route_key not in route_cache else 0.0)
            t0 = time.perf_counter()
            x = build_tile_input(
                context_rgb,
                conditioning,
                args.coordinate_mode,
                (cx0, cy0, cx1 - cx0, cy1 - cy0),
                (width, height),
                source_global_stats,
            )
            t1 = time.perf_counter()
            input_ms.append((t1 - t0) * 1000.0)
            tile_jobs.append(
                {
                    "x": x,
                    "x0": x0,
                    "y0": y0,
                    "x1": x1,
                    "y1": y1,
                    "cx0": cx0,
                    "cy0": cy0,
                    "rx0": rx0,
                    "ry0": ry0,
                    "rx1": rx1,
                    "ry1": ry1,
                    "tile_label": tile_label,
                    "cluster": cluster,
                    "override_cluster": override_cluster,
                    "checkpoint_role": model_key,
                    "conditioning": conditioning,
                    "route": route,
                }
            )
    model_batch_size = max(1, int(args.model_batch_size))
    grouped_jobs: dict[tuple[str, int, int, int], list[dict[str, Any]]] = {}
    for job in tile_jobs:
        x = job["x"]
        key = (str(job["checkpoint_role"]), int(x.shape[1]), int(x.shape[2]), int(x.shape[3]))
        grouped_jobs.setdefault(key, []).append(job)
    for (model_key, _channels, _height, _width), jobs in grouped_jobs.items():
        for start in range(0, len(jobs), model_batch_size):
            chunk = jobs[start:start + model_batch_size]
            batch = torch.cat([job["x"] for job in chunk], dim=0)
            t1 = time.perf_counter()
            with torch.no_grad():
                preds = run_model_padded(models[model_key], batch).detach().cpu().numpy()
            t2 = time.perf_counter()
            model_ms.append((t2 - t1) * 1000.0)
            model_batch_sizes.append(len(chunk))
            for job, pred in zip(chunk, preds):
                x0 = int(job["x0"])
                y0 = int(job["y0"])
                x1 = int(job["x1"])
                y1 = int(job["y1"])
                cx0 = int(job["cx0"])
                cy0 = int(job["cy0"])
                rx0 = int(job["rx0"])
                ry0 = int(job["ry0"])
                rx1 = int(job["rx1"])
                ry1 = int(job["ry1"])
                pred_context_rgb = np.clip(np.transpose(pred, (1, 2, 0)) * 255.0, 0, 255).astype(np.uint8)
                ox0 = x0 - cx0
                oy0 = y0 - cy0
                pred_rgb = pred_context_rgb[oy0:oy0 + (y1 - y0), ox0:ox0 + (x1 - x0)]
                valid_margin = max(0, int(args.valid_margin))
                local_x0 = 0 if x0 == 0 else min(valid_margin, max(0, pred_rgb.shape[1] - 1))
                local_y0 = 0 if y0 == 0 else min(valid_margin, max(0, pred_rgb.shape[0] - 1))
                local_x1 = pred_rgb.shape[1] if x1 >= width else max(local_x0 + 1, pred_rgb.shape[1] - valid_margin)
                local_y1 = pred_rgb.shape[0] if y1 >= height else max(local_y0 + 1, pred_rgb.shape[0] - valid_margin)
                valid_rgb = pred_rgb[local_y0:local_y1, local_x0:local_x1]
                w = tile_weight(valid_rgb.shape[0], valid_rgb.shape[1])
                out_y0 = y0 + local_y0
                out_x0 = x0 + local_x0
                out_y1 = out_y0 + valid_rgb.shape[0]
                out_x1 = out_x0 + valid_rgb.shape[1]
                out_acc[out_y0:out_y1, out_x0:out_x1] += valid_rgb.astype(np.float32) * w
                weight_acc[out_y0:out_y1, out_x0:out_x1] += w
                tile_rows.append(
                    {
                        "xywh": [x0, y0, pred_rgb.shape[1], pred_rgb.shape[0]],
                        "route_context_xywh": [rx0, ry0, rx1 - rx0, ry1 - ry0],
                        "context_xywh": [cx0, cy0, int(job["x"].shape[-1]), int(job["x"].shape[-2])],
                        "written_xywh": [out_x0, out_y0, valid_rgb.shape[1], valid_rgb.shape[0]],
                        "tile_label": job["tile_label"],
                        "cluster": job["cluster"],
                        "override_cluster": job["override_cluster"],
                        "checkpoint_role": job["checkpoint_role"],
                        "conditioning": job["conditioning"],
                        **job["route"],
                    }
                )
    stitched = np.clip(out_acc / np.maximum(weight_acc, 1e-6), 0, 255).astype(np.uint8)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    stitched_suffix, stitched_pil_format, stitched_save_kwargs = stitched_output_spec(args.stitched_output_format)
    stitched_path = args.output_dir / f"{image_id}_scene_routed_fullframe{stitched_suffix}"
    t0 = time.perf_counter()
    Image.fromarray(stitched).save(stitched_path, stitched_pil_format, **stitched_save_kwargs)
    stitch_save_ms = (time.perf_counter() - t0) * 1000.0
    scored_image = stitched
    post_refiner_receipt = None
    post_refiner_wall_ms = 0.0
    if args.post_checkpoint is not None:
        t0 = time.perf_counter()
        scored_image, post_refiner_receipt = apply_post_refiner(
            image_rgb=stitched,
            model=models["post_refiner"],
            conditioning=args.post_conditioning,
            coordinate_mode=args.post_coordinate_mode,
            tile_size=int(args.post_tile_size),
            overlap=int(args.post_overlap),
            source_frequency_mode=args.post_source_frequency_planes,
            source_frequency_blur=args.post_source_frequency_blur,
        )
        post_path = args.output_dir / f"{image_id}_scene_routed_fullframe_post_refined.png"
        Image.fromarray(scored_image).save(post_path)
        post_refiner_wall_ms = (time.perf_counter() - t0) * 1000.0
        post_refiner_receipt["png"] = post_path.name
    runtime_wall_ms = (time.perf_counter() - runtime_wall_start) * 1000.0
    scoring_wall_start = time.perf_counter()
    ref_rgb = None
    ref_load_ms = 0.0
    crop_rows: list[dict[str, Any]] = []
    if not args.skip_quality_scoring:
        if ref_tiff is None:
            raise RuntimeError("quality scoring requested without a REF TIFF")
        t0 = time.perf_counter()
        ref_rgb = load_rgb(ref_tiff)
        ref_load_ms = (time.perf_counter() - t0) * 1000.0
        for crop_name, crop in manifest["crops"].items():
            if crop_name.startswith("$"):
                continue
            ref_crop = crop_metric_image(ref_rgb, crop, image["sensor_dims"])
            out_crop = crop_metric_image(scored_image, crop, image["sensor_dims"])
            crop_png = args.output_dir / f"{image_id}_{crop_name}_scene_routed_fullframe.png"
            Image.fromarray(out_crop).save(crop_png)
            metrics = compute_visual_metrics(ref_crop, out_crop)
            metrics = {k: float(v) for k, v in metrics.items()}
            metrics["preview_pass"] = pass_preview(metrics)
            crop_rows.append(
                {
                    "image_id": image_id,
                    "crop": crop_name,
                    "png": crop_png.name,
                    **metrics,
                }
            )
    scoring_wall_ms = (time.perf_counter() - scoring_wall_start) * 1000.0
    total_eval_wall_ms = (time.perf_counter() - image_wall_start) * 1000.0
    return {
        "image_id": image_id,
        "source_dng": str(source_dng),
        "ref_dng": str(ref_dng) if ref_dng is not None else None,
        "source_render_size": [width, height],
        "ref_render_size": [ref_rgb.shape[1], ref_rgb.shape[0]] if ref_rgb is not None else None,
        "stitched_output": stitched_path.name,
        "stitched_output_format": args.stitched_output_format,
        "stitched_output_bytes": stitched_path.stat().st_size,
        "stitched_png": stitched_path.name if stitched_suffix == ".png" else None,
        "tile_count": len(tile_rows),
        "tile_size": tile,
        "overlap": int(args.overlap),
        "valid_margin": int(args.valid_margin),
        "route_context_padding": int(args.route_context_padding),
        "model_context_padding": int(args.model_context_padding),
        "timing": {
            "runtime_no_ref_wall_ms": runtime_wall_ms,
            "scoring_wall_ms": scoring_wall_ms,
            "total_eval_wall_ms": total_eval_wall_ms,
            "source_render_ms": source_render_ms,
            "source_load_ms": source_load_ms,
            "ref_render_ms": ref_render_ms,
            "ref_load_ms": ref_load_ms,
            "route_ms_median": float(statistics.median(route_ms)) if route_ms else 0.0,
            "route_ms_total": float(sum(route_ms)),
            "route_feature_ms_median": float(statistics.median(route_feature_ms)) if route_feature_ms else 0.0,
            "route_feature_ms_total": float(sum(route_feature_ms)),
            "route_select_ms_median": float(statistics.median(route_select_ms)) if route_select_ms else 0.0,
            "route_select_ms_total": float(sum(route_select_ms)),
            "route_smoothing_precompute_ms": route_smoothing_precompute_ms,
            "save_tile_ms_median": float(statistics.median(save_tile_ms)) if save_tile_ms else 0.0,
            "stitch_save_ms": stitch_save_ms,
            "input_ms_median": float(statistics.median(input_ms)) if input_ms else 0.0,
            "model_ms_median": float(statistics.median(model_ms)) if model_ms else 0.0,
            "model_ms_total": float(sum(model_ms)),
            "model_batch_size_requested": model_batch_size,
            "model_batch_count": len(model_batch_sizes),
            "model_batch_size_max": max(model_batch_sizes) if model_batch_sizes else 0,
            "model_tile_count": sum(model_batch_sizes),
            "post_refiner_wall_ms": post_refiner_wall_ms,
            "scene_route_ms_median": float(statistics.median(scene_route_ms)) if scene_route_ms else 0.0,
            "scene_route_ms_total": float(sum(scene_route_ms)),
            "scene_route_feature_ms_median": float(statistics.median(scene_route_feature_ms)) if scene_route_feature_ms else 0.0,
            "scene_route_feature_ms_total": float(sum(scene_route_feature_ms)),
            "scene_route_select_ms_median": float(statistics.median(scene_route_select_ms)) if scene_route_select_ms else 0.0,
            "scene_route_select_ms_total": float(sum(scene_route_select_ms)),
        },
        "post_refiner": post_refiner_receipt,
        "tile_roles": {
            role: sum(1 for row in tile_rows if row["checkpoint_role"] == role)
            for role in sorted({row["checkpoint_role"] for row in tile_rows})
        },
        "route_smoothing": {
            "enabled": bool(args.route_smoothing_radius > 0 and not args.force_model_key),
            "radius": float(args.route_smoothing_radius),
            "min_fraction": float(args.route_smoothing_min_fraction),
            "changed_tile_count": sum(1 for row in tile_rows if row.get("route_smoothing_changed")),
        },
        "scene_spatial": {
            "role_counts_before_spatial": scene_role_counts,
            "enabled_regions": sorted(spatial_enabled_names) if spatial_enabled_names is not None else None,
            "role_min": routing["spatial_scene_role_min"],
        },
        "tiles": tile_rows,
        "rows": crop_rows,
    }


def write_dashboard(payload: dict[str, Any], html_path: Path) -> None:
    summary = payload["summary"]["preview_runtime_policy"]
    timing = payload.get("timing_summary", {})
    rows = payload["rows"]
    css = """
body { font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; margin:18px; background:#f7f8f9; color:#222; }
.cards { display:grid; grid-template-columns:repeat(5,minmax(160px,1fr)); gap:10px; margin:14px 0; }
.card,.tile { background:white; border:1px solid #d4d8de; border-radius:6px; padding:10px; }
.grid { display:grid; grid-template-columns:repeat(3,minmax(220px,1fr)); gap:10px; }
.tile img { width:100%; display:block; border:1px solid #ddd; }
.pass { color:#096b2b; font-weight:700; }
.fail { color:#9b1c1c; font-weight:700; }
td,th { border:1px solid #ccc; padding:5px 7px; text-align:right; }
table { border-collapse:collapse; background:white; font-size:12px; }
th.left,td.left { text-align:left; }
"""
    parts = [
        "<!doctype html><meta charset='utf-8'><title>PREVIEW full-frame tiled receipt</title>",
        f"<style>{css}</style><h1>PREVIEW Full-Frame Tiled Receipt</h1>",
        "<p>Diagnostic no-REF tiled full-frame render. REF is scoring-only.</p><div class=cards>",
    ]
    cards = [
        ("Pass", f"{summary['pass_count']}/{summary['count']}"),
        ("Pass rate", f"{summary['pass_rate'] * 100:.1f}%"),
        ("Worst LPIPS", f"{summary['worst_lpips']:.4f}"),
        ("Worst Y-PSNR", f"{summary['worst_y_psnr']:.2f}"),
        ("Worst dE2000", f"{summary['worst_dE2000_mean']:.2f}"),
    ]
    if timing:
        cards.extend(
            [
                ("Runtime avg", f"{timing['runtime_no_ref_wall_ms_avg'] / 1000.0:.2f}s"),
                ("Runtime FPS", f"{timing['runtime_no_ref_fps_avg']:.3f}"),
                ("Model avg", f"{timing['model_ms_total_avg'] / 1000.0:.2f}s"),
                ("Total eval avg", f"{timing['total_eval_wall_ms_avg'] / 1000.0:.2f}s"),
            ]
        )
    for label, value in cards:
        parts.append(f"<div class=card><b>{label}</b><br>{value}</div>")
    if not rows:
        parts.append("</div><p>Quality scoring skipped for production render timing receipt.</p>")
        html_path.write_text("".join(parts))
        return
    parts.append("</div><table><thead><tr><th class=left>image</th><th class=left>crop</th><th>LPIPS</th><th>MS</th><th>Y</th><th>dE</th><th>pass</th></tr></thead><tbody>")
    for row in rows:
        cls = "pass" if row["preview_pass"] else "fail"
        parts.append(
            f"<tr><td class=left>{row['image_id']}</td><td class=left>{row['crop']}</td>"
            f"<td>{row['lpips']:.4f}</td><td>{row['ms_ssim']:.4f}</td>"
            f"<td>{row['y_psnr']:.2f}</td><td>{row['dE2000_mean']:.2f}</td>"
            f"<td class={cls}>{'PASS' if row['preview_pass'] else 'FAIL'}</td></tr>"
        )
    parts.append("</tbody></table><div class=grid>")
    for row in rows:
        parts.append(
            f"<div class=tile><b>{row['image_id']} {row['crop']}</b>"
            f"<br>LPIPS {row['lpips']:.4f} Y {row['y_psnr']:.2f} dE {row['dE2000_mean']:.2f}"
            f"<img src='{row['png']}'></div>"
        )
    parts.append("</div>")
    html_path.write_text("".join(parts))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, default=REPO / "pipelines/preview_holdout_set.json")
    ap.add_argument("--image-id", action="append", default=[])
    ap.add_argument("--ref-root", type=Path, action="append", default=[
        external_path('cnn/diverse_dngs'),
        external_path('barnsky_full_dngs'),
    ])
    ap.add_argument("--source-root", type=Path, action="append", default=[
        external_path('artifacts/upresable_holdout_clean_20260607/editable_dng'),
        external_path('artifacts/upresable/editable_dng'),
        external_path('artifacts/upresable_preview_probe_20260606/editable_dng'),
    ])
    ap.add_argument("--router-sidecar", type=Path, required=True)
    ap.add_argument("--override-router-sidecar", type=Path, action="append", default=[])
    ap.add_argument("--default-checkpoint", type=Path, required=True)
    ap.add_argument("--cluster-checkpoint", action="append", default=[])
    ap.add_argument("--override-cluster-checkpoint", action="append", default=[])
    ap.add_argument("--spatial-checkpoint", action="append", default=[], help="NAME=PATH; selected by --spatial-region after normal routing.")
    ap.add_argument("--cluster-conditioning", action="append", default=[])
    ap.add_argument("--override-cluster-conditioning", action="append", default=[])
    ap.add_argument("--spatial-conditioning", action="append", default=[], help="NAME=MODE for spatial checkpoint input conditioning.")
    ap.add_argument("--spatial-region", action="append", default=[], help="NAME=X0,Y0,X1,Y1[,CLUSTER] normalized tile-center bounds.")
    ap.add_argument("--spatial-scene-role-min", action="append", default=[], help="NAME=ROLE,COUNT; enable spatial region only when the full-frame pre-route role histogram meets this minimum.")
    ap.add_argument("--conditioning", choices=["zero", "content_stats", "color_stats", "global_color_stats"], default="zero")
    ap.add_argument("--coordinate-mode", choices=["local", "global_tile", "zero_coord"], default="local")
    ap.add_argument("--tile-size", type=int, default=768)
    ap.add_argument("--overlap", type=int, default=128)
    ap.add_argument("--tile-offset", action="append", default=[], help="Full-grid origin offset as X,Y pixels. Repeat for multi-origin stitching.")
    ap.add_argument("--valid-margin", type=int, default=0, help="Overlap-save mode: discard this many non-border pixels from each output tile before stitching.")
    ap.add_argument("--route-context-padding", type=int, default=0, help="Route each output tile using this many surrounding source pixels while keeping model input unchanged unless model context is also set.")
    ap.add_argument("--route-feature-max-side", type=int, default=512, help="Max side for runtime router feature extraction. Default preserves the frozen sidecar feature implementation.")
    ap.add_argument("--route-smoothing-radius", type=float, default=0.0, help="Diagnostic: locally smooth source-derived route roles within this pixel radius before model selection.")
    ap.add_argument("--route-smoothing-min-fraction", type=float, default=0.50, help="Minimum local-majority fraction needed to replace a tile route when route smoothing is enabled.")
    ap.add_argument("--model-context-padding", type=int, default=0, help="Run/route each output tile with this many source pixels of surrounding context, then crop back to the tile.")
    ap.add_argument("--tile-mode", choices=["full_grid", "manifest_crops"], default="full_grid")
    ap.add_argument("--force-model-key", help="Diagnostic: bypass routing and run one loaded model key on every tile.")
    ap.add_argument("--force-conditioning", choices=["zero", "content_stats", "color_stats", "global_color_stats"], default="zero")
    ap.add_argument("--post-checkpoint", type=Path, help="Optional no-REF post-refiner applied to the stitched full-frame RGB before scoring.")
    ap.add_argument("--post-conditioning", choices=["zero", "content_stats", "color_stats", "global_color_stats"], default="global_color_stats")
    ap.add_argument("--post-coordinate-mode", choices=["local", "global_tile"], default="global_tile")
    ap.add_argument("--post-tile-size", type=int, default=512)
    ap.add_argument("--post-overlap", type=int, default=128)
    ap.add_argument("--model-batch-size", type=int, default=1, help="Maximum same-checkpoint/same-shape tiles per model invocation.")
    ap.add_argument("--stitched-output-format", choices=["png", "png_fast", "png_store", "tiff_raw", "bmp"], default="png")
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--dashboard-json", type=Path, required=True)
    ap.add_argument("--dashboard-html", type=Path, required=True)
    ap.add_argument("--tmp-dir", type=Path, default=external_path('tmp'))
    ap.add_argument("--skip-quality-scoring", action="store_true", help="Measure production no-REF render timing without REF render/load, crop metrics, or crop PNGs.")
    args = ap.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.dashboard_json.parent.mkdir(parents=True, exist_ok=True)
    args.dashboard_html.parent.mkdir(parents=True, exist_ok=True)
    args.tmp_dir.mkdir(parents=True, exist_ok=True)
    args.tile_offsets = parse_tile_offsets(args.tile_offset)

    cluster_ckpts = parse_cluster_checkpoint(args.cluster_checkpoint)
    override_ckpts = parse_override_checkpoint(args.override_cluster_checkpoint)
    spatial_ckpts = parse_spatial_checkpoint(args.spatial_checkpoint)
    spatial_regions = parse_spatial_region(args.spatial_region)
    spatial_conditioning = parse_spatial_conditioning(args.spatial_conditioning)
    spatial_scene_role_min = parse_spatial_scene_role_threshold(args.spatial_scene_role_min)
    spatial_region_names = {str(r["name"]) for r in spatial_regions}
    missing_regions = sorted(set(spatial_ckpts) - spatial_region_names)
    if missing_regions:
        raise ValueError(f"spatial checkpoints missing matching --spatial-region entries: {missing_regions}")
    missing_checkpoints = sorted(spatial_region_names - set(spatial_ckpts))
    if missing_checkpoints:
        raise ValueError(f"spatial regions missing matching --spatial-checkpoint entries: {missing_checkpoints}")
    all_ckpts = {
        "default": args.default_checkpoint,
        **{f"cluster_{k}": v for k, v in cluster_ckpts.items()},
        **{
            f"override_{idx}_cluster_{cluster}" if idx is not None else f"override_cluster_{cluster}": v
            for (idx, cluster), v in override_ckpts.items()
        },
        **{f"spatial_{name}": path for name, path in spatial_ckpts.items()},
    }
    models = {key: load_model(path) for key, path in all_ckpts.items()}
    if args.post_checkpoint is not None:
        models["post_refiner"], post_ckpt = load_model_with_receipt(args.post_checkpoint)
        args.post_source_frequency_planes = str(post_ckpt.get("source_frequency_planes", "none"))
        args.post_source_frequency_blur = float(post_ckpt.get("source_frequency_blur", 2.0))
    else:
        post_ckpt = None
        args.post_source_frequency_planes = "none"
        args.post_source_frequency_blur = 2.0
    if args.force_model_key and args.force_model_key not in models:
        raise ValueError(f"--force-model-key must be one of {sorted(models)}, got {args.force_model_key!r}")
    checkpoint_receipts = {
        key: {"path": str(path), "sha256": sha256_file(path), "bytes": path.stat().st_size}
        for key, path in all_ckpts.items()
    }
    if args.post_checkpoint is not None:
        checkpoint_receipts["post_refiner"] = {
            "path": str(args.post_checkpoint),
            "sha256": sha256_file(args.post_checkpoint),
            "bytes": args.post_checkpoint.stat().st_size,
            "architecture": str(post_ckpt.get("architecture", "direct")) if post_ckpt else "unknown",
            "source_policy": str(post_ckpt.get("source_policy", "unknown")) if post_ckpt else "unknown",
            "in_channels": int(post_ckpt.get("in_channels", 9)) if post_ckpt else 9,
            "source_frequency_planes": args.post_source_frequency_planes,
            "source_frequency_blur": args.post_source_frequency_blur,
        }
    routing = {
        "base_sidecar": json.loads(args.router_sidecar.read_text()),
        "override_sidecars": [(path, json.loads(path.read_text())) for path in args.override_router_sidecar],
        "default_checkpoint": args.default_checkpoint,
        "cluster_ckpts": cluster_ckpts,
        "override_ckpts": override_ckpts,
        "cluster_conditioning": parse_cluster_conditioning(args.cluster_conditioning),
        "override_conditioning": parse_override_conditioning(args.override_cluster_conditioning),
        "default_conditioning": args.conditioning,
        "spatial_regions": spatial_regions,
        "spatial_conditioning": spatial_conditioning,
        "spatial_scene_role_min": spatial_scene_role_min,
    }
    manifest = json.loads(args.manifest.read_text())
    images = [
        image for image in manifest["images"]
        if not args.image_id or str(image["id"]) in set(args.image_id)
    ]
    if not images:
        raise RuntimeError("no manifest images selected")
    work = Path(tempfile.mkdtemp(prefix="preview_fullframe_", dir=args.tmp_dir))
    image_receipts = []
    rows = []
    try:
        for image in images:
            receipt = render_full_image(args, image, work, models, routing)
            image_receipts.append(receipt)
            rows.extend(receipt["rows"])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    payload = {
        "schema": "preview_scene_routed_fullframe_receipt.v1",
        "runtime_contract": {
            "source_policy": "scene_router_kmeans_runtime_features_tiled_fullframe",
            "forbidden_inputs": ["REF image content", "REF HF/LF fields", "winner JSON", "sample index", "crop identity key planes", "gate metrics"],
            "render_inputs": ["source RGB full frame", "runtime tile feature cluster", "normalized tile coordinates", "selected checkpoint"],
            "router_sidecar": str(args.router_sidecar),
            "override_router_sidecar": [str(path) for path in args.override_router_sidecar],
            "spatial_regions": spatial_regions,
            "spatial_conditioning": spatial_conditioning,
            "spatial_scene_role_min": spatial_scene_role_min,
            "tile_size": args.tile_size,
            "overlap": args.overlap,
            "tile_offsets": args.tile_offsets,
            "valid_margin": args.valid_margin,
            "route_context_padding": args.route_context_padding,
            "route_feature_max_side": args.route_feature_max_side,
            "route_smoothing_radius": args.route_smoothing_radius,
            "route_smoothing_min_fraction": args.route_smoothing_min_fraction,
            "model_context_padding": args.model_context_padding,
            "model_batch_size": args.model_batch_size,
            "stitched_output_format": args.stitched_output_format,
            "coordinate_mode": args.coordinate_mode,
            "post_refiner": {
                "enabled": args.post_checkpoint is not None,
                "conditioning": args.post_conditioning if args.post_checkpoint is not None else None,
                "coordinate_mode": args.post_coordinate_mode if args.post_checkpoint is not None else None,
                "tile_size": args.post_tile_size if args.post_checkpoint is not None else None,
                "overlap": args.post_overlap if args.post_checkpoint is not None else None,
                "source_frequency_planes": args.post_source_frequency_planes if args.post_checkpoint is not None else None,
                "source_frequency_blur": args.post_source_frequency_blur if args.post_checkpoint is not None else None,
                "render_inputs": ["stitched RGB full frame", "normalized pixel coordinates", "checkpoint"] if args.post_checkpoint is not None else [],
            },
            "device": str(DEVICE),
            "quality_scoring": "skipped" if args.skip_quality_scoring else "enabled",
        },
        "router_sidecar_sha256": sha256_file(args.router_sidecar),
        "override_router_sidecar_sha256": [sha256_file(path) for path in args.override_router_sidecar],
        "checkpoints": checkpoint_receipts,
        "summary": {"preview_runtime_policy": summarize_quality_rows(rows)},
        "timing_summary": summarize_image_timing(image_receipts),
        "memory": {"max_rss_mb": max_rss_mb(), **mps_memory_mb()},
        "images": image_receipts,
        "rows": rows,
    }
    args.dashboard_json.write_text(json.dumps(payload, indent=2))
    write_dashboard(payload, args.dashboard_html)
    print(json.dumps(payload["summary"]["preview_runtime_policy"], indent=2), flush=True)
    print(args.dashboard_json)
    print(args.dashboard_html)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
