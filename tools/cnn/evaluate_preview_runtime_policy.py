#!/usr/bin/env python3
"""Evaluate a deterministic runtime policy for the no-REF PREVIEW candidate.

The direct RGB non-REF checkpoint originally cleared the crop dashboard with
dashboard-shaped inputs: per-row source winners and sample-index key planes.
This receipt runner removes those inputs. It chooses render sources from a
fixed policy, feeds only source RGB plus coordinates by default, and records
quality, timing, and memory for the actual checkpoint invocation. The runner
accepts arbitrary source dimensions; the current default artifact set contains
crop proxies rather than full-image display sources.
"""
from __future__ import annotations


import argparse
import hashlib
import html
import json
import resource
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import torch
from PIL import Image




sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))
sys.path.insert(0, str(REPO / "tools/cnn"))

from metrics import compute_visual_metrics  # noqa: E402
from preview_rgb_models import DirectRGBRefiner, parse_crop_png, pass_preview  # noqa: E402


DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
DEFAULT_ARTIFACT_ROOT = external_path('artifacts')
DEFAULT_CHECKPOINT = (
    DEFAULT_ARTIFACT_ROOT
    / "display_rgb_direct_lpips_nonref_20260606"
    / "display_rgb_direct_lpips_nonref.pt"
)
DEFAULT_SOURCE_ROOTS = [
    DEFAULT_ARTIFACT_ROOT / "upresable_preview_probe_20260606" / "crops",
    DEFAULT_ARTIFACT_ROOT / "display_learned_atlas_20260606",
    DEFAULT_ARTIFACT_ROOT / "display_rgb_refiner_20260606",
]
RUNTIME_PRIORITY_V1 = [
    "crops:upresable_preview",
    "display_learned_atlas_20260606:learned_atlas",
]


@dataclass(frozen=True)
class RuntimeSample:
    image_id: str
    crop: str
    ref_path: Path
    source_path: Path
    source_label: str


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def discover(source_roots: list[Path]) -> tuple[dict[tuple[str, str], Path], dict[tuple[tuple[str, str], str], Path]]:
    refs: dict[tuple[str, str], Path] = {}
    sources: dict[tuple[tuple[str, str], str], Path] = {}
    for root in source_roots:
        for path in sorted(root.glob("Z8Z_*_*.png")):
            parsed = parse_crop_png(path)
            if parsed is None:
                continue
            image_id, crop, variant = parsed
            key = (image_id, crop)
            if variant == "REF":
                refs.setdefault(key, path)
            else:
                sources[(key, f"{root.name}:{variant}")] = path
    return refs, sources


def choose_source(key: tuple[str, str], sources: dict[tuple[tuple[str, str], str], Path], policy: str) -> tuple[str, Path]:
    if policy == "runtime_priority_v1":
        labels = RUNTIME_PRIORITY_V1
    elif policy == "fixed_upresable":
        labels = ["crops:upresable_preview"]
    elif policy == "fixed_learned_atlas":
        labels = ["display_learned_atlas_20260606:learned_atlas"]
    else:
        raise ValueError(f"unsupported policy {policy!r}")
    for label in labels:
        path = sources.get((key, label))
        if path is not None:
            return label, path
    raise KeyError(f"no source for {key} under {policy}: tried {labels}")


def build_samples(args: argparse.Namespace) -> list[RuntimeSample]:
    refs, sources = discover(args.source_root)
    out: list[RuntimeSample] = []
    for key in sorted(refs):
        if args.image_id and key[0] not in args.image_id:
            continue
        label, source = choose_source(key, sources, args.policy)
        out.append(RuntimeSample(key[0], key[1], refs[key], source, label))
    if not out:
        raise RuntimeError("no runtime samples discovered")
    return out


def build_input(source_rgb: np.ndarray, conditioning: str) -> torch.Tensor:
    height, width = source_rgb.shape[:2]
    yy, xx = np.meshgrid(
        np.linspace(0, 1, height, dtype=np.float32),
        np.linspace(0, 1, width, dtype=np.float32),
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
    else:
        raise ValueError(f"unsupported conditioning {conditioning!r}")
    arr = np.concatenate([source, np.stack([xx, yy], axis=0), key_planes], axis=0)
    return torch.from_numpy(arr[None].copy()).to(DEVICE).contiguous()


def render_one(model: DirectRGBRefiner, source_rgb: np.ndarray, conditioning: str) -> tuple[np.ndarray, dict[str, float]]:
    t0 = time.perf_counter()
    x = build_input(source_rgb, conditioning)
    t1 = time.perf_counter()
    with torch.no_grad():
        pred = model(x).detach().cpu().numpy()[0]
    t2 = time.perf_counter()
    rgb = np.clip(np.transpose(pred, (1, 2, 0)) * 255, 0, 255).astype(np.uint8)
    return rgb, {"input_ms": (t1 - t0) * 1000.0, "model_ms": (t2 - t1) * 1000.0}


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


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    pass_count = sum(1 for r in rows if r["preview_pass"])
    return {
        "count": len(rows),
        "pass_count": pass_count,
        "pass_rate": pass_count / max(1, len(rows)),
        "worst_lpips": max(float(r["lpips"]) for r in rows),
        "median_lpips": float(statistics.median(float(r["lpips"]) for r in rows)),
        "worst_ms_ssim": min(float(r["ms_ssim"]) for r in rows),
        "worst_y_psnr": min(float(r["y_psnr"]) for r in rows),
        "worst_dE2000_mean": max(float(r["dE2000_mean"]) for r in rows),
    }


def write_html(payload: dict[str, Any], html_path: Path) -> None:
    rows = payload["rows"]
    summary = payload["summary"]["preview_runtime_policy"]
    css = """
body { font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; margin:18px; background:#f6f6f2; color:#222; }
.cards { display:grid; grid-template-columns:repeat(5,minmax(180px,1fr)); gap:10px; margin:14px 0; }
.card,.tile { background:white; border:1px solid #ccc; border-radius:6px; padding:10px; }
.grid { display:grid; grid-template-columns:repeat(4,minmax(220px,1fr)); gap:10px; }
.tile img { width:100%; display:block; border:1px solid #ddd; }
.cap { font-size:11px; line-height:1.35; color:#444; }
.pass { color:#096b2b; font-weight:700; }
.fail { color:#9b1c1c; font-weight:700; }
table { border-collapse:collapse; background:white; font-size:12px; }
td,th { border:1px solid #ccc; padding:5px 7px; text-align:right; }
th.left,td.left { text-align:left; }
code { font-size:11px; }
"""
    cards = [
        ("Pass", f"{summary['pass_count']}/{summary['count']}"),
        ("Pass rate", f"{summary['pass_rate'] * 100:.1f}%"),
        ("Worst LPIPS", f"{summary['worst_lpips']:.4f}"),
        ("Worst dE2000", f"{summary['worst_dE2000_mean']:.2f}"),
        ("Model ms/crop median", f"{payload['timing']['model_ms_per_crop_median']:.2f}"),
    ]
    parts = [
        "<!doctype html><meta charset='utf-8'><title>PREVIEW Runtime Policy</title>",
        f"<style>{css}</style><h1>PREVIEW Runtime Policy Receipt</h1>",
        "<p>Deterministic no-REF runtime policy. REF is used only for scoring this receipt.</p>",
        "<div class=cards>",
    ]
    for label, value in cards:
        parts.append(f"<div class=card><b>{html.escape(label)}</b><br>{html.escape(value)}</div>")
    parts.append("</div>")
    parts.append("<table><tr><th class=left>policy</th><th class=left>conditioning</th><th class=left>device</th><th>RSS MB</th><th>checkpoint</th></tr>")
    parts.append(
        "<tr>"
        f"<td class=left>{html.escape(payload['runtime_contract']['source_policy'])}</td>"
        f"<td class=left>{html.escape(payload['runtime_contract']['conditioning'])}</td>"
        f"<td class=left>{html.escape(payload['runtime_contract']['device'])}</td>"
        f"<td>{payload['memory']['max_rss_mb']:.1f}</td>"
        f"<td class=left><code>{html.escape(payload['checkpoint_sha256'][:16])}</code></td>"
        "</tr></table>"
    )
    parts.append("<div class=grid>")
    for row in rows:
        klass = "pass" if row["preview_pass"] else "fail"
        parts.append(
            "<div class=tile>"
            f"<img src='{html.escape(row['png'])}'>"
            f"<div class=cap><b>{html.escape(row['image_id'])} {html.escape(row['crop'])}</b><br>"
            f"{html.escape(row['source_label'])}<br>"
            f"<span class={klass}>LPIPS {row['lpips']:.4f}, MS {row['ms_ssim']:.4f}, "
            f"Y {row['y_psnr']:.2f}, dE {row['dE2000_mean']:.2f}</span></div></div>"
        )
    parts.append("</div>")
    html_path.write_text("\n".join(parts))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--checkpoint", type=Path, default=DEFAULT_CHECKPOINT)
    ap.add_argument("--source-root", type=Path, action="append", default=DEFAULT_SOURCE_ROOTS)
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--dashboard-json", type=Path, required=True)
    ap.add_argument("--dashboard-html", type=Path, required=True)
    ap.add_argument("--policy", choices=["runtime_priority_v1", "fixed_upresable", "fixed_learned_atlas"], default="runtime_priority_v1")
    ap.add_argument("--conditioning", choices=["zero", "content_stats", "color_stats"], default="zero")
    ap.add_argument("--image-id", action="append", help="optional image id filter")
    ap.add_argument("--timing-iters", type=int, default=5)
    args = ap.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.dashboard_json.parent.mkdir(parents=True, exist_ok=True)
    args.dashboard_html.parent.mkdir(parents=True, exist_ok=True)

    ckpt = torch.load(str(args.checkpoint), map_location="cpu", weights_only=False)
    model = DirectRGBRefiner(
        width=int(ckpt.get("width", 40)),
        residual_scale=float(ckpt.get("residual_scale", 0.5)),
    ).to(DEVICE)
    model.load_state_dict(ckpt["state_dict"])
    model.eval()

    samples = build_samples(args)
    rows: list[dict[str, Any]] = []
    input_ms: list[float] = []
    model_ms: list[float] = []
    metric_ms: list[float] = []
    save_ms: list[float] = []
    for sample in samples:
        source_rgb = load_rgb(sample.source_path)
        ref_rgb = load_rgb(sample.ref_path)
        # Warm once per sample; then record repeat timings for the same render path.
        rgb, _ = render_one(model, source_rgb, args.conditioning)
        sample_input_ms: list[float] = []
        sample_model_ms: list[float] = []
        for _ in range(max(1, args.timing_iters)):
            rgb, timing = render_one(model, source_rgb, args.conditioning)
            sample_input_ms.append(timing["input_ms"])
            sample_model_ms.append(timing["model_ms"])
        png = args.output_dir / f"{sample.image_id}_{sample.crop}_{args.policy}_{args.conditioning}.png"
        t0 = time.perf_counter()
        Image.fromarray(rgb).save(png)
        save_ms.append((time.perf_counter() - t0) * 1000.0)
        t0 = time.perf_counter()
        metrics = compute_visual_metrics(ref_rgb, rgb)
        metric_ms.append((time.perf_counter() - t0) * 1000.0)
        metrics["preview_pass"] = pass_preview(metrics)
        input_ms.extend(sample_input_ms)
        model_ms.extend(sample_model_ms)
        rows.append({
            "image_id": sample.image_id,
            "crop": sample.crop,
            "source_label": sample.source_label,
            "source_png": str(sample.source_path),
            "ref_png": str(sample.ref_path),
            "png": png.name,
            **metrics,
        })
        print(
            f"{sample.image_id} {sample.crop} {'PASS' if metrics['preview_pass'] else 'FAIL'} "
            f"{sample.source_label} lp={metrics['lpips']:.4f} ms={metrics['ms_ssim']:.4f} "
            f"y={metrics['y_psnr']:.2f} de={metrics['dE2000_mean']:.2f}",
            flush=True,
        )

    payload = {
        "schema": "preview_runtime_policy_receipt.v1",
        "checkpoint": str(args.checkpoint),
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "summary": {"preview_runtime_policy": summarize(rows)},
        "runtime_contract": {
            "source_policy": args.policy,
            "policy_definition": (
                "runtime_priority_v1 tries upresable_preview first, then learned_atlas; "
                "fixed policies use the named source for every frame."
            ),
            "conditioning": args.conditioning,
            "forbidden_inputs": ["REF image content", "REF HF/LF fields", "winner JSON", "sample index", "crop identity key planes"],
            "render_inputs": ["source RGB frame/crop", "normalized pixel coordinates", "checkpoint"],
            "full_image_status": "runner accepts arbitrary source dimensions; this receipt uses available 16 crop proxies because full-image display sources are not present in the artifact set",
            "device": str(DEVICE),
        },
        "timing": {
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
