#!/usr/bin/env python3
"""Evaluate a hard-routed multi-expert PREVIEW refiner.

Routing is supplied by a scene-router audit generated from runtime source
features. REF is used only for metric labels in the receipt.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import resource
import statistics
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np
import torch
from PIL import Image


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))
sys.path.insert(0, str(REPO / "tools/cnn"))

from preview_scene_features import FEATURE_NAMES, discover_sources, feature_vector  # noqa: E402
from evaluate_preview_runtime_policy import build_input, load_rgb, summarize, write_html  # noqa: E402
from metrics import compute_visual_metrics  # noqa: E402
from preview_rgb_models import DirectRGBRefiner, pass_preview  # noqa: E402


DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


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


def parse_cluster_checkpoint(values: list[str]) -> dict[int, Path]:
    out: dict[int, Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--cluster-checkpoint must be CLUSTER=PATH, got {value!r}")
        left, right = value.split("=", 1)
        out[int(left)] = Path(right)
    return out


def parse_override_checkpoint(values: list[str]) -> dict[tuple[int | None, int], Path]:
    out: dict[tuple[int | None, int], Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--override-cluster-checkpoint must be [INDEX:]CLUSTER=PATH, got {value!r}")
        left, right = value.split("=", 1)
        if ":" in left:
            index, cluster = left.split(":", 1)
            key = (int(index), int(cluster))
        else:
            key = (None, int(left))
        out[key] = Path(right)
    return out


def parse_cluster_conditioning(values: list[str]) -> dict[int, str]:
    out: dict[int, str] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--cluster-conditioning must be CLUSTER=MODE, got {value!r}")
        left, right = value.split("=", 1)
        if right not in {"zero", "content_stats", "color_stats", "global_color_stats"}:
            raise ValueError(f"unsupported conditioning mode for cluster {left}: {right!r}")
        out[int(left)] = right
    return out


def parse_override_conditioning(values: list[str]) -> dict[tuple[int | None, int], str]:
    out: dict[tuple[int | None, int], str] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--override-cluster-conditioning must be [INDEX:]CLUSTER=MODE, got {value!r}")
        left, right = value.split("=", 1)
        if right not in {"zero", "content_stats", "color_stats", "global_color_stats"}:
            raise ValueError(f"unsupported override conditioning mode for cluster {left}: {right!r}")
        if ":" in left:
            index, cluster = left.split(":", 1)
            key = (int(index), int(cluster))
        else:
            key = (None, int(left))
        out[key] = right
    return out


def parse_int_list(values: list[str]) -> set[int]:
    out: set[int] = set()
    for value in values:
        for part in value.split(","):
            part = part.strip()
            if part:
                out.add(int(part))
    return out


def load_model(path: Path) -> DirectRGBRefiner:
    ckpt = torch.load(str(path), map_location="cpu", weights_only=False)
    model = DirectRGBRefiner(
        width=int(ckpt.get("width", 40)),
        residual_scale=float(ckpt.get("residual_scale", 0.5)),
    ).to(DEVICE)
    model.load_state_dict(ckpt["state_dict"])
    model.eval()
    return model


def resolve_source(row: dict[str, Any], source_map: dict[tuple[str, str, str], Path]) -> Path:
    path = Path(str(row.get("source_png_resolved") or row.get("source_png") or ""))
    if path.exists():
        return path
    key = (row["image_id"], row["crop"], row["source_label"])
    path = source_map.get(key, path)
    if not path.exists():
        raise FileNotFoundError(f"missing source for {key}: {path}")
    return path


def resolve_ref(row: dict[str, Any], source_map: dict[tuple[str, str, str], Path]) -> Path:
    path = Path(str(row.get("ref_png") or ""))
    if path.exists():
        return path
    # Current receipts use the same root for REF and source rows.
    root_label = str(row["source_label"]).split(":", 1)[0]
    key = (row["image_id"], row["crop"], f"{root_label}:REF")
    path = source_map.get(key, path)
    if not path.exists():
        raise FileNotFoundError(f"missing REF for {key}: {path}")
    return path


def route_features_from_sidecar(features: np.ndarray, sidecar: dict[str, Any]) -> tuple[int, dict[str, Any]]:
    router = sidecar.get("router") or {}
    if router.get("type") != "zscore_nearest_center":
        raise ValueError(f"unsupported router sidecar type: {router.get('type')!r}")
    names = router.get("feature_names") or []
    if names != FEATURE_NAMES:
        raise ValueError("router sidecar feature_names do not match evaluator feature extractor")
    mean = np.array([float((router.get("feature_mean") or {})[name]) for name in FEATURE_NAMES], dtype=np.float64)
    std = np.array([float((router.get("feature_std") or {})[name]) for name in FEATURE_NAMES], dtype=np.float64)
    centers_z = np.array(
        [
            [float(center[name]) for name in FEATURE_NAMES]
            for center in (router.get("centers_z") or [])
        ],
        dtype=np.float64,
    )
    if centers_z.ndim != 2 or centers_z.shape[0] == 0 or centers_z.shape[1] != len(FEATURE_NAMES):
        raise ValueError("router sidecar has invalid centers_z")
    std = np.where(std < 1e-6, 1.0, std)
    z = (features - mean) / std
    distances = ((centers_z - z[None, :]) ** 2).sum(axis=1)
    cluster = int(distances.argmin())
    return cluster, {
        "route_source": "frozen_sidecar_nearest_center",
        "route_distance": float(distances[cluster]),
        "features": {name: float(features[idx]) for idx, name in enumerate(FEATURE_NAMES)},
    }


def route_from_sidecar(source_path: Path, sidecar: dict[str, Any]) -> tuple[int, dict[str, Any]]:
    return route_features_from_sidecar(feature_vector(source_path), sidecar)


def center_crop(arr: np.ndarray, size: int) -> np.ndarray:
    if size <= 0:
        return arr
    height, width = arr.shape[:2]
    if size > height or size > width:
        raise ValueError(f"center crop {size} exceeds image shape {arr.shape}")
    y0 = (height - size) // 2
    x0 = (width - size) // 2
    return arr[y0:y0 + size, x0:x0 + size]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--router-audit", type=Path, required=True)
    ap.add_argument("--router-sidecar", type=Path)
    ap.add_argument("--override-router-sidecar", type=Path, action="append", default=[])
    ap.add_argument("--source-root", type=Path, action="append", required=True)
    ap.add_argument("--default-checkpoint", type=Path, required=True)
    ap.add_argument("--cluster-checkpoint", action="append", default=[], help="CLUSTER=PATH")
    ap.add_argument("--override-cluster-checkpoint", action="append", default=[], help="[INDEX:]CLUSTER=PATH")
    ap.add_argument("--bypass-cluster", action="append", default=[], help="Cluster id or comma-list to pass source through unchanged")
    ap.add_argument(
        "--cluster-conditioning",
        action="append",
        default=[],
        help="Optional CLUSTER=MODE override. MODE is zero, content_stats, or color_stats.",
    )
    ap.add_argument(
        "--override-cluster-conditioning",
        action="append",
        default=[],
        help="Optional override-router [INDEX:]CLUSTER=MODE override. MODE is zero, content_stats, or color_stats.",
    )
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--dashboard-json", type=Path, required=True)
    ap.add_argument("--dashboard-html", type=Path, required=True)
    ap.add_argument("--conditioning", choices=["zero", "content_stats", "color_stats"], default="zero")
    ap.add_argument("--timing-iters", type=int, default=5)
    ap.add_argument(
        "--metric-center-size",
        type=int,
        default=0,
        help="If set, compute metrics on the centered square of this size. "
             "Used for larger-context diagnostic renders.",
    )
    args = ap.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.dashboard_json.parent.mkdir(parents=True, exist_ok=True)
    args.dashboard_html.parent.mkdir(parents=True, exist_ok=True)

    audit = json.loads(args.router_audit.read_text())
    sidecar = json.loads(args.router_sidecar.read_text()) if args.router_sidecar else None
    override_sidecars = [
        (path, json.loads(path.read_text()))
        for path in (args.override_router_sidecar or [])
    ]
    source_map = discover_sources(args.source_root)
    cluster_ckpts = parse_cluster_checkpoint(args.cluster_checkpoint)
    override_cluster_ckpts = parse_override_checkpoint(args.override_cluster_checkpoint)
    cluster_conditioning = parse_cluster_conditioning(args.cluster_conditioning)
    override_cluster_conditioning = parse_override_conditioning(args.override_cluster_conditioning)
    bypass_clusters = parse_int_list(args.bypass_cluster)
    all_ckpts = {
        "default": args.default_checkpoint,
        **{f"cluster_{k}": v for k, v in cluster_ckpts.items()},
        **{
            f"override_{idx}_cluster_{cluster}" if idx is not None else f"override_cluster_{cluster}": v
            for (idx, cluster), v in override_cluster_ckpts.items()
        },
    }
    models: dict[str, DirectRGBRefiner] = {}
    checkpoint_receipts: dict[str, dict[str, Any]] = {}
    model_load_ms: list[float] = []
    for key, path in all_ckpts.items():
        t0 = time.perf_counter()
        models[key] = load_model(path)
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        model_load_ms.append(elapsed_ms)
        checkpoint_receipts[key] = {
            "path": str(path),
            "sha256": sha256_file(path),
            "bytes": path.stat().st_size,
            "load_ms": elapsed_ms,
        }

    rows: list[dict[str, Any]] = []
    input_ms: list[float] = []
    model_ms: list[float] = []
    save_ms: list[float] = []
    metric_ms: list[float] = []
    for row in audit.get("rows") or []:
        source_path = resolve_source(row, source_map)
        if sidecar:
            cluster, route = route_from_sidecar(source_path, sidecar)
        else:
            cluster = int(row["cluster"])
            route = {"route_source": "router_audit_row_cluster"}
        override_cluster = None
        ckpt_path = cluster_ckpts.get(cluster, args.default_checkpoint)
        model_key = f"cluster_{cluster}" if cluster in cluster_ckpts else "default"
        if override_sidecars:
            override_trace = []
            for override_index, (_override_path, override_sidecar) in enumerate(override_sidecars):
                routed_cluster, override_route = route_from_sidecar(source_path, override_sidecar)
                override_trace.append(
                    {
                        "index": override_index,
                        "cluster": routed_cluster,
                        "route_source": override_route["route_source"],
                        "route_distance": override_route["route_distance"],
                    }
                )
                override_key = (override_index, routed_cluster)
                legacy_key = (None, routed_cluster)
                matched_key = override_key if override_key in override_cluster_ckpts else legacy_key
                if matched_key in override_cluster_ckpts:
                    override_cluster = routed_cluster
                    route["override_route_source"] = override_route["route_source"]
                    route["override_route_distance"] = override_route["route_distance"]
                    route["override_router_index"] = override_index
                    ckpt_path = override_cluster_ckpts[matched_key]
                    model_key = (
                        f"override_{override_index}_cluster_{override_cluster}"
                        if override_key in override_cluster_ckpts
                        else f"override_cluster_{override_cluster}"
                    )
                    break
            route["override_trace"] = override_trace
            if "override_route_source" not in route and override_trace:
                route["override_route_source"] = override_trace[-1]["route_source"]
                route["override_route_distance"] = override_trace[-1]["route_distance"]
        ref_path = resolve_ref(row, source_map)
        source = load_rgb(source_path)
        ref = load_rgb(ref_path)
        rgb = None
        sample_input_ms: list[float] = []
        sample_model_ms: list[float] = []
        if cluster in bypass_clusters:
            model_key = "bypass_source"
            ckpt_path = Path("")
            sample_conditioning = cluster_conditioning.get(cluster, args.conditioning)
            rgb = source.copy()
            sample_input_ms.append(0.0)
            sample_model_ms.append(0.0)
        else:
            model = models[model_key]
            override_key = (
                (int(route["override_router_index"]), override_cluster)
                if override_cluster is not None and "override_router_index" in route
                else (None, override_cluster)
            )
            if override_cluster is not None and override_key in override_cluster_ckpts:
                sample_conditioning = override_cluster_conditioning.get(
                    override_key,
                    override_cluster_conditioning.get(
                        (None, override_cluster),
                        cluster_conditioning.get(cluster, args.conditioning),
                    ),
                )
            elif override_cluster is not None and (None, override_cluster) in override_cluster_ckpts:
                sample_conditioning = override_cluster_conditioning.get(
                    (None, override_cluster),
                    cluster_conditioning.get(cluster, args.conditioning),
                )
            else:
                sample_conditioning = cluster_conditioning.get(cluster, args.conditioning)
            for _ in range(max(1, args.timing_iters)):
                t0 = time.perf_counter()
                x = build_input(source, sample_conditioning)
                t1 = time.perf_counter()
                with torch.no_grad():
                    pred = model(x).detach().cpu().numpy()[0]
                t2 = time.perf_counter()
                rgb = np.clip(np.transpose(pred, (1, 2, 0)) * 255, 0, 255).astype(np.uint8)
                sample_input_ms.append((t1 - t0) * 1000.0)
                sample_model_ms.append((t2 - t1) * 1000.0)
        assert rgb is not None
        png = args.output_dir / f"{row['image_id']}_{row['crop']}_scene_routed.png"
        t0 = time.perf_counter()
        Image.fromarray(rgb).save(png)
        save_ms.append((time.perf_counter() - t0) * 1000.0)
        t0 = time.perf_counter()
        metric_ref = center_crop(ref, args.metric_center_size)
        metric_rgb = center_crop(rgb, args.metric_center_size)
        metrics = compute_visual_metrics(metric_ref, metric_rgb)
        metric_ms.append((time.perf_counter() - t0) * 1000.0)
        metrics["preview_pass"] = pass_preview(metrics)
        input_ms.extend(sample_input_ms)
        model_ms.extend(sample_model_ms)
        rows.append(
            {
                "image_id": row["image_id"],
                "crop": row["crop"],
                "cluster": cluster,
                "override_cluster": override_cluster,
                **route,
                "checkpoint_role": model_key,
                "checkpoint": str(ckpt_path) if ckpt_path else None,
                "conditioning": sample_conditioning,
                "source_label": row["source_label"],
                "source_png": str(source_path),
                "ref_png": str(ref_path),
                "png": png.name,
                **metrics,
            }
        )
        print(
            f"{row['image_id']} {row['crop']} c{cluster} {model_key} "
            f"{'PASS' if metrics['preview_pass'] else 'FAIL'} "
            f"lp={metrics['lpips']:.4f} ms={metrics['ms_ssim']:.4f} "
            f"y={metrics['y_psnr']:.2f} de={metrics['dE2000_mean']:.2f}",
            flush=True,
        )

    payload = {
        "schema": "preview_scene_routed_receipt.v1",
        "router_audit": str(args.router_audit),
        "runtime_contract": {
            "router": "hard cluster route from runtime source features",
            "source_policy": "scene_router_kmeans_runtime_features",
            "conditioning": args.conditioning,
            "cluster_conditioning": {str(k): v for k, v in sorted(cluster_conditioning.items())},
            "override_cluster_conditioning": {
                (f"{idx}:{cluster}" if idx is not None else str(cluster)): v
                for (idx, cluster), v in sorted(override_cluster_conditioning.items())
            },
            "forbidden_inputs": ["REF image content", "REF HF/LF fields", "winner JSON", "sample index", "crop identity key planes", "gate metrics"],
            "render_inputs": ["source RGB frame/crop", "runtime feature cluster", "selected checkpoint"],
            "device": str(DEVICE),
            "router_sidecar": str(args.router_sidecar) if args.router_sidecar else None,
            "override_router_sidecar": [str(path) for path, _sidecar in override_sidecars],
            "router_assignment": "frozen_sidecar_nearest_center" if args.router_sidecar else "router_audit_row_cluster",
            "override_router_assignment": "frozen_sidecar_nearest_center" if override_sidecars else None,
            "bypass_clusters": sorted(bypass_clusters),
            "model_loading_policy": "preload_all_configured_experts",
            "metric_center_size": args.metric_center_size,
        },
        "router_sidecar_sha256": sha256_file(args.router_sidecar) if args.router_sidecar else None,
        "override_router_sidecar_sha256": [sha256_file(path) for path, _sidecar in override_sidecars],
        "checkpoints": checkpoint_receipts,
        "checkpoint_sha256": "multiple",
        "summary": {"preview_runtime_policy": summarize(rows)},
        "timing": {
            "model_load_ms_total": float(sum(model_load_ms)),
            "model_load_ms_max": float(max(model_load_ms) if model_load_ms else 0.0),
            "timing_iters_per_crop": max(1, args.timing_iters),
            "input_ms_per_crop_median": float(statistics.median(input_ms)),
            "input_ms_per_crop_p95": float(np.percentile(input_ms, 95)),
            "model_ms_per_crop_median": float(statistics.median(model_ms)),
            "model_ms_per_crop_p95": float(np.percentile(model_ms, 95)),
            "save_png_ms_per_crop_median": float(statistics.median(save_ms)),
            "metric_ms_per_crop_median": float(statistics.median(metric_ms)),
        },
        "memory": {"max_rss_mb": max_rss_mb(), **mps_memory_mb()},
        "rows": rows,
    }
    args.dashboard_json.write_text(json.dumps(payload, indent=2))
    write_html(payload, args.dashboard_html)
    print(json.dumps(payload["summary"]["preview_runtime_policy"], indent=2), flush=True)
    print(args.dashboard_json)
    print(args.dashboard_html)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
