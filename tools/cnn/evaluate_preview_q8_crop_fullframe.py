#!/usr/bin/env python3
"""Evaluate a q8 crop PREVIEW refiner on tiled full frames.

This is the production-shaped counterpart to ``train_preview_q8_crop_refiner``:
it loads a q8-source checkpoint, applies it to runtime q8 full-frame source
tiles, stitches the output, and scores manifest crops from that stitched image.
REF is used only for scoring.
"""
from __future__ import annotations


import argparse
import html
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
from PIL import Image




sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))
sys.path.insert(0, str(REPO / "tools/cnn"))

from build_preview_holdout_runtime_receipt import resolve_ref, scaled_box, sha256_file  # noqa: E402
from evaluate_preview_runtime_policy import summarize  # noqa: E402
from metrics import compute_visual_metrics  # noqa: E402
from preview_rgb_models import build_rgb_refiner, pass_preview  # noqa: E402
from preview_input_features import coordinate_tensor, source_multiband_tensor  # noqa: E402


Image.MAX_IMAGE_PIXELS = None
DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")


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


def load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def render_dng_to_png(dng_path: Path, png_path: Path) -> float:
    t0 = time.perf_counter()
    result = subprocess.run(
        ["sips", "-s", "format", "png", str(dng_path), "--out", str(png_path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"sips failed for {dng_path}: {result.stderr[-400:]}")
    return (time.perf_counter() - t0) * 1000.0


def load_source_receipt(path: Path) -> dict[str, Path]:
    payload = json.loads(path.read_text())
    root = path.parent
    out: dict[str, Path] = {}
    for image in payload.get("images") or []:
        stitched = root / str(image["stitched_png"])
        if not stitched.exists():
            raise FileNotFoundError(f"missing q8 source fullframe {stitched}")
        out[str(image["image_id"])] = stitched
    return out


def load_model(checkpoint: Path) -> torch.nn.Module:
    ckpt = torch.load(str(checkpoint), map_location="cpu", weights_only=False)
    model = build_rgb_refiner(
        str(ckpt.get("architecture", "direct")),
        width=int(ckpt.get("width", 40)),
        in_channels=int(ckpt.get("in_channels", 34)),
        residual_scale=float(ckpt.get("residual_scale", 0.5)),
    ).to(DEVICE)
    model.load_state_dict(ckpt["state_dict"])
    model.eval()
    return model


def selected_images(args: argparse.Namespace, manifest: dict[str, Any]) -> list[dict[str, Any]]:
    wanted = set(args.image_id)
    images = [image for image in manifest["images"] if not wanted or str(image["id"]) in wanted]
    if args.limit:
        images = images[: args.limit]
    if not images:
        raise RuntimeError("no selected images")
    return images


def tile_origins(length: int, tile: int, stride: int) -> list[int]:
    if length <= tile:
        return [0]
    out = list(range(0, max(1, length - tile + 1), stride))
    last = length - tile
    if out[-1] != last:
        out.append(last)
    return out


def tile_weight(height: int, width: int) -> np.ndarray:
    wy = np.hanning(height).astype(np.float32)
    wx = np.hanning(width).astype(np.float32)
    if height <= 2:
        wy = np.ones(height, dtype=np.float32)
    if width <= 2:
        wx = np.ones(width, dtype=np.float32)
    return np.maximum(wy[:, None] * wx[None, :], 1e-3)[..., None]


def run_model_padded(model: torch.nn.Module, x: torch.Tensor, multiple: int = 4) -> torch.Tensor:
    height, width = x.shape[-2:]
    pad_h = (multiple - height % multiple) % multiple
    pad_w = (multiple - width % multiple) % multiple
    if pad_h or pad_w:
        x = F.pad(x, (0, pad_w, 0, pad_h), mode="replicate")
    pred = model(x)
    return pred[..., :height, :width]


def build_q8_tile_input(source_rgb: np.ndarray, coordinate_mode: str, xywh: tuple[int, int, int, int], full_size: tuple[int, int]) -> torch.Tensor:
    source01 = source_rgb.astype(np.float32) / 255.0
    height, width = source_rgb.shape[:2]
    if coordinate_mode == "local":
        coords = coordinate_tensor(height, width)
    elif coordinate_mode == "global":
        x0, y0, _w, _h = xywh
        full_w, full_h = full_size
        yy, xx = torch.meshgrid(
            torch.linspace(float(y0) / max(1.0, float(full_h - 1)), float(y0 + height - 1) / max(1.0, float(full_h - 1)), height),
            torch.linspace(float(x0) / max(1.0, float(full_w - 1)), float(x0 + width - 1) / max(1.0, float(full_w - 1)), width),
            indexing="ij",
        )
        coords = torch.stack([xx * 2.0 - 1.0, yy * 2.0 - 1.0], dim=0).float()
    elif coordinate_mode == "zero":
        coords = torch.zeros((2, height, width), dtype=torch.float32)
    else:
        raise ValueError(f"unsupported coordinate mode {coordinate_mode!r}")
    mean = source01.mean(axis=(0, 1), dtype=np.float64).astype(np.float32)
    std = source01.std(axis=(0, 1), dtype=np.float64).astype(np.float32)
    stats = np.concatenate([mean, std]).astype(np.float32)
    stat_planes = np.broadcast_to(stats[:, None, None], (6, height, width)).copy()
    planes = [
        torch.from_numpy(source01).permute(2, 0, 1),
        coords,
        source_multiband_tensor(source_rgb),
        torch.from_numpy(stat_planes),
    ]
    return torch.cat(planes, dim=0).unsqueeze(0).to(DEVICE).contiguous()


@torch.no_grad()
def apply_tiled(model: torch.nn.Module, source_rgb: np.ndarray, args: argparse.Namespace) -> tuple[np.ndarray, dict[str, Any]]:
    height, width = source_rgb.shape[:2]
    stride = max(1, args.tile_size - args.overlap)
    out_acc = np.zeros((height, width, 3), dtype=np.float32)
    weight_acc = np.zeros((height, width, 1), dtype=np.float32)
    input_ms: list[float] = []
    model_ms: list[float] = []
    tile_count = 0
    for y0 in tile_origins(height, args.tile_size, stride):
        for x0 in tile_origins(width, args.tile_size, stride):
            y1 = min(height, y0 + args.tile_size)
            x1 = min(width, x0 + args.tile_size)
            tile_rgb = source_rgb[y0:y1, x0:x1]
            t0 = time.perf_counter()
            x = build_q8_tile_input(tile_rgb, args.coordinate_mode, (x0, y0, x1 - x0, y1 - y0), (width, height))
            t1 = time.perf_counter()
            pred = run_model_padded(model, x)[0].detach().cpu().numpy()
            t2 = time.perf_counter()
            pred_rgb = np.clip(np.transpose(pred, (1, 2, 0)) * 255.0, 0, 255).astype(np.uint8)
            weight = tile_weight(pred_rgb.shape[0], pred_rgb.shape[1])
            out_acc[y0:y1, x0:x1] += pred_rgb.astype(np.float32) * weight
            weight_acc[y0:y1, x0:x1] += weight
            input_ms.append((t1 - t0) * 1000.0)
            model_ms.append((t2 - t1) * 1000.0)
            tile_count += 1
    stitched = np.clip(out_acc / np.maximum(weight_acc, 1e-6), 0, 255).astype(np.uint8)
    return stitched, {
        "tile_count": tile_count,
        "tile_size": args.tile_size,
        "overlap": args.overlap,
        "coordinate_mode": args.coordinate_mode,
        "input_ms_median": float(statistics.median(input_ms)) if input_ms else 0.0,
        "model_ms_median": float(statistics.median(model_ms)) if model_ms else 0.0,
        "input_ms_total": float(sum(input_ms)),
        "model_ms_total": float(sum(model_ms)),
    }


def crop_to_512(rgb: np.ndarray, crop: dict[str, int], sensor_dims: list[int]) -> np.ndarray:
    pil = Image.fromarray(rgb).crop(scaled_box(crop, sensor_dims, (rgb.shape[1], rgb.shape[0])))
    if pil.size != (512, 512):
        pil = pil.resize((512, 512), Image.Resampling.LANCZOS)
    return np.asarray(pil.convert("RGB"), dtype=np.uint8)


def write_html(payload: dict[str, Any], path: Path) -> None:
    css = """
body { font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; margin:18px; background:#f7f8f9; color:#222; }
.cards { display:grid; grid-template-columns:repeat(4,minmax(160px,1fr)); gap:10px; margin:14px 0; }
.card,.tile { background:#fff; border:1px solid #d4d8de; border-radius:6px; padding:10px; }
table { border-collapse:collapse; background:#fff; width:100%; font-size:12px; margin:14px 0; }
td,th { border:1px solid #ccd2d9; padding:5px 7px; text-align:right; }
th.left,td.left { text-align:left; }
.pass { color:#096b2b; font-weight:700; }
.fail { color:#9b1c1c; font-weight:700; }
.grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(220px,1fr)); gap:10px; }
.tile img { width:100%; display:block; border:1px solid #ddd; }
"""
    summary = payload["summary"]["preview_q8_crop_fullframe"]
    parts = [
        "<!doctype html><meta charset='utf-8'><title>q8 Crop Full-frame PREVIEW</title>",
        f"<style>{css}</style><h1>q8 Crop Full-frame PREVIEW</h1>",
        "<p>Runtime inputs are q8 source full-frame tiles, source-derived feature planes, normalized coordinates, and checkpoint only. REF is scoring only.</p>",
        "<div class=cards>",
        f"<div class=card><b>Pass</b><br>{summary['pass_count']}/{summary['count']}</div>",
        f"<div class=card><b>Worst LPIPS</b><br>{summary['worst_lpips']:.4f}</div>",
        f"<div class=card><b>Worst dE2000</b><br>{summary['worst_dE2000_mean']:.2f}</div>",
        f"<div class=card><b>Tiles</b><br>{payload['timing']['tile_count_total']}</div>",
        "</div><table><thead><tr><th class=left>image</th><th class=left>crop</th><th>pass</th><th>LPIPS</th><th>MS</th><th>Y</th><th>dE</th></tr></thead><tbody>",
    ]
    for row in payload["rows"]:
        cls = "pass" if row["preview_pass"] else "fail"
        parts.append(
            f"<tr><td class=left>{html.escape(row['image_id'])}</td><td class=left>{html.escape(row['crop'])}</td>"
            f"<td class={cls}>{'PASS' if row['preview_pass'] else 'FAIL'}</td>"
            f"<td>{row['lpips']:.4f}</td><td>{row['ms_ssim']:.4f}</td><td>{row['y_psnr']:.2f}</td><td>{row['dE2000_mean']:.2f}</td></tr>"
        )
    parts.append("</tbody></table><div class=grid>")
    for row in sorted(payload["rows"], key=lambda item: (item["preview_pass"], -float(item["lpips"]))):
        cls = "pass" if row["preview_pass"] else "fail"
        parts.append(
            f"<div class=tile><b>{html.escape(row['image_id'])} {html.escape(row['crop'])}</b>"
            f"<br><span class={cls}>LPIPS {row['lpips']:.4f}, MS {row['ms_ssim']:.4f}, Y {row['y_psnr']:.2f}, dE {row['dE2000_mean']:.2f}</span>"
            f"<img src='{html.escape(row['png'])}'></div>"
        )
    parts.append("</div>")
    path.write_text("".join(parts))


def collect(args: argparse.Namespace) -> dict[str, Any]:
    manifest = json.loads(args.manifest.read_text())
    crops = {str(k): v for k, v in manifest["crops"].items() if not str(k).startswith("$")}
    requested_crops = set(args.crop)
    source_paths = load_source_receipt(args.source_fullframe_receipt)
    model = load_model(args.checkpoint)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="preview_q8_crop_fullframe_", dir=args.tmp_dir))
    rows: list[dict[str, Any]] = []
    image_rows: list[dict[str, Any]] = []
    render_ms: list[float] = []
    tile_timings: list[dict[str, Any]] = []
    try:
        for image in selected_images(args, manifest):
            image_id = str(image["id"])
            source_path = source_paths.get(image_id)
            if source_path is None:
                raise FileNotFoundError(f"missing q8 source fullframe for {image_id}")
            ref_png = work / f"{image_id}_ref.png"
            render_ms.append(render_dng_to_png(resolve_ref(image, args.ref_root), ref_png))
            print(f"[q8-fullframe] {image_id} apply tiled checkpoint", flush=True)
            source_rgb = load_rgb(source_path)
            ref_rgb = load_rgb(ref_png)
            t0 = time.perf_counter()
            stitched, timing = apply_tiled(model, source_rgb, args)
            timing["wall_ms"] = (time.perf_counter() - t0) * 1000.0
            tile_timings.append(timing)
            stitched_name = f"{image_id}_q8_crop_fullframe.png"
            if args.save_fullframe:
                Image.fromarray(stitched).save(args.output_dir / stitched_name)
            image_rows.append(
                {
                    "image_id": image_id,
                    "source_png": str(source_path),
                    "stitched_png": stitched_name if args.save_fullframe else None,
                    "timing": timing,
                }
            )
            for crop_name, crop in crops.items():
                if requested_crops and crop_name not in requested_crops:
                    continue
                ref_crop = crop_to_512(ref_rgb, crop, image["sensor_dims"])
                pred_crop = crop_to_512(stitched, crop, image["sensor_dims"])
                crop_png = args.output_dir / f"{image_id}_{crop_name}_q8_crop_fullframe.png"
                Image.fromarray(pred_crop).save(crop_png)
                metrics = {key: float(value) for key, value in compute_visual_metrics(ref_crop, pred_crop).items()}
                metrics["preview_pass"] = pass_preview(metrics)
                rows.append(
                    {
                        "image_id": image_id,
                        "crop": crop_name,
                        "variant": "q8_crop_fullframe_tiled",
                        "png": crop_png.name,
                        **metrics,
                    }
                )
                print(
                    f"[q8-fullframe] {image_id} {crop_name} "
                    f"{'PASS' if metrics['preview_pass'] else 'FAIL'} "
                    f"lp={metrics['lpips']:.4f} ms={metrics['ms_ssim']:.4f} "
                    f"y={metrics['y_psnr']:.2f} de={metrics['dE2000_mean']:.2f}",
                    flush=True,
                )
            ref_png.unlink(missing_ok=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    summary = summarize(rows)
    return {
        "schema": "preview_q8_crop_fullframe_receipt.v1",
        "manifest": str(args.manifest),
        "source_fullframe_receipt": str(args.source_fullframe_receipt),
        "checkpoint": str(args.checkpoint),
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "device": str(DEVICE),
        "render_contract": {
            "render_time_inputs": ["q8_source_rgb_fullframe_tiles", "normalized_xy", "source_rgb_multiscale_bands", "source_rgb_grad_lap", "source_rgb_global_mean_std", "checkpoint"],
            "forbidden_render_time_inputs": ["ref_rgb", "ref_dng", "gate_metrics", "sample_index", "crop_identity_key_planes"],
            "source_policy": "q8_source_fullframe_tiled_plus_source_derived_features_only",
            "uses_ref_at_render_time": False,
        },
        "images": image_rows,
        "timing": {
            "render_ref_ms_total": float(sum(render_ms)),
            "render_ref_ms_median": float(statistics.median(render_ms)) if render_ms else 0.0,
            "tile_count_total": int(sum(int(item["tile_count"]) for item in tile_timings)),
            "model_ms_total": float(sum(float(item["model_ms_total"]) for item in tile_timings)),
            "model_ms_median": float(statistics.median(float(item["model_ms_median"]) for item in tile_timings)) if tile_timings else 0.0,
            "input_ms_total": float(sum(float(item["input_ms_total"]) for item in tile_timings)),
            "wall_ms_total": float(sum(float(item["wall_ms"]) for item in tile_timings)),
            "max_rss_mb": max_rss_mb(),
            **mps_memory_mb(),
        },
        "summary": {"preview_q8_crop_fullframe": summary},
        "rows": rows,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, default=REPO / "pipelines/preview_holdout_set.json")
    ap.add_argument(
        "--source-fullframe-receipt",
        type=Path,
        default=external_path('artifacts/preview_runtime_policy_20260613/q8_source_fullframes_holdout28_v1/preview_codec_source_fullframes.json'),
    )
    ap.add_argument("--checkpoint", type=Path, required=True)
    ap.add_argument("--image-id", action="append", default=[])
    ap.add_argument("--crop", action="append", default=[])
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--tile-size", type=int, default=512)
    ap.add_argument("--overlap", type=int, default=0)
    ap.add_argument("--coordinate-mode", choices=["local", "global", "zero"], default="local")
    ap.add_argument("--save-fullframe", action="store_true")
    ap.add_argument(
        "--ref-root",
        type=Path,
        action="append",
        default=[
            external_path('cnn/diverse_dngs'),
            external_path('barnsky_full_dngs'),
        ],
    )
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--output-json", type=Path, required=True)
    ap.add_argument("--output-html", type=Path, required=True)
    ap.add_argument("--tmp-dir", type=Path, default=external_path('tmp'))
    args = ap.parse_args()
    args.tmp_dir.mkdir(parents=True, exist_ok=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    payload = collect(args)
    args.output_json.write_text(json.dumps(payload, indent=2, allow_nan=False) + "\n")
    write_html(payload, args.output_html)
    summary = payload["summary"]["preview_q8_crop_fullframe"]
    print(
        f"fullframe {summary['pass_count']}/{summary['count']} "
        f"LPIPS={summary['worst_lpips']:.4f} MS={summary['worst_ms_ssim']:.4f} "
        f"Y={summary['worst_y_psnr']:.2f} dE={summary['worst_dE2000_mean']:.2f}",
        flush=True,
    )
    print(args.output_json)
    print(args.output_html)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
