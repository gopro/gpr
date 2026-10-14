#!/usr/bin/env python3
"""Build PREVIEW holdout RGB crop pairs for runtime routed evaluation.

This creates source/REF crop PNGs from full-image renders. It does not train
or score a model. REF crops are rendered from the manifest source DNGs; source
crops are rendered from existing UPRESABLE editable DNG outputs when present.
Rows with missing source images are recorded in the receipt and skipped.
"""
from __future__ import annotations


import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

from PIL import Image



import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO / "pipelines/preview_holdout_set.json"
DEFAULT_SOURCE_ROOTS = [
    external_path('artifacts/upresable/editable_dng'),
    external_path('artifacts/upresable_preview_probe_20260606/editable_dng'),
]
DEFAULT_REF_ROOTS = [
    external_path('cnn/diverse_dngs'),
    external_path('barnsky_full_dngs'),
]
DEFAULT_OUT_DIR = external_path('artifacts/preview_runtime_policy_20260606/holdout_runtime_crops')


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


def resolve_source(image_id: str, roots: list[Path]) -> Path | None:
    for root in roots:
        path = root / f"{image_id}.dng"
        if path.exists():
            return path
    return None


def resolve_ref(image: dict[str, Any], roots: list[Path]) -> Path:
    image_id = str(image["id"])
    for root in roots:
        path = root / f"{image_id}.dng"
        if path.exists():
            return path
    path = Path(image["path"]).expanduser()
    return path if path.is_absolute() else external_path(str(path))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def scaled_box(crop: dict[str, int], sensor_dims: list[int], rendered_size: tuple[int, int]) -> tuple[int, int, int, int]:
    sensor_w, sensor_h = int(sensor_dims[0]), int(sensor_dims[1])
    render_w, render_h = rendered_size
    sx = render_w / sensor_w
    sy = render_h / sensor_h
    x = int(round(int(crop["x"]) * sx))
    y = int(round(int(crop["y"]) * sy))
    w = int(round(int(crop["w"]) * sx))
    h = int(round(int(crop["h"]) * sy))
    x = min(max(0, x), max(0, render_w - w))
    y = min(max(0, y), max(0, render_h - h))
    return x, y, x + w, y + h


def save_crop(render_path: Path, out_path: Path, crop: dict[str, int], sensor_dims: list[int]) -> dict[str, Any]:
    with Image.open(render_path) as image:
        rgb = image.convert("RGB")
        box = scaled_box(crop, sensor_dims, rgb.size)
        cropped = rgb.crop(box)
        if cropped.size != (512, 512):
            cropped = cropped.resize((512, 512), Image.Resampling.LANCZOS)
        cropped.save(out_path)
        return {
            "render_size": list(rgb.size),
            "crop_box_render": list(box),
        }


def build(args: argparse.Namespace) -> dict[str, Any]:
    manifest = json.loads(args.manifest.read_text())
    args.out_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="preview_holdout_render_", dir=args.tmp_dir))
    rows: list[dict[str, Any]] = []
    missing: list[dict[str, Any]] = []
    render_ms: list[float] = []
    try:
        for image in manifest["images"]:
            image_id = image["id"]
            ref_dng = resolve_ref(image, args.ref_root)
            source_dng = resolve_source(image_id, args.source_root)
            if not ref_dng.exists():
                missing.append({"image_id": image_id, "reason": "missing_ref_dng", "path": str(ref_dng)})
                continue
            if source_dng is None:
                missing.append({"image_id": image_id, "reason": "missing_upresable_editable_dng"})
                continue
            if source_dng.resolve() == ref_dng.resolve():
                missing.append({"image_id": image_id, "reason": "source_is_ref_path", "path": str(source_dng)})
                continue
            ref_sha = sha256_file(ref_dng)
            source_sha = sha256_file(source_dng)
            if source_sha == ref_sha:
                missing.append({"image_id": image_id, "reason": "source_is_ref_hash", "path": str(source_dng)})
                continue

            ref_tiff = work / f"{image_id}_REF.tiff"
            source_tiff = work / f"{image_id}_upresable_preview.tiff"
            render_ms.append(render_dng_to_tiff(ref_dng, ref_tiff))
            render_ms.append(render_dng_to_tiff(source_dng, source_tiff))

            for crop_name, crop in manifest["crops"].items():
                if crop_name.startswith("$"):
                    continue
                ref_png = args.out_dir / f"{image_id}_{crop_name}_REF.png"
                source_png = args.out_dir / f"{image_id}_{crop_name}_upresable_preview.png"
                ref_meta = save_crop(ref_tiff, ref_png, crop, image["sensor_dims"])
                source_meta = save_crop(source_tiff, source_png, crop, image["sensor_dims"])
                rows.append(
                    {
                        "image_id": image_id,
                        "crop": crop_name,
                        "source_label": f"{args.out_dir.name}:upresable_preview",
                        "source_png": str(source_png),
                        "ref_png": str(ref_png),
                        "ref_dng": str(ref_dng),
                        "manifest_ref_path": image["path"],
                        "source_dng": str(source_dng),
                        "ref_dng_sha256": ref_sha,
                        "source_dng_sha256": source_sha,
                        "strata": image.get("strata", []),
                        "ref_render": ref_meta,
                        "source_render": source_meta,
                    }
                )
            if not args.keep_renders:
                ref_tiff.unlink(missing_ok=True)
                source_tiff.unlink(missing_ok=True)
    finally:
        if not args.keep_renders:
            shutil.rmtree(work, ignore_errors=True)

    timing = {
        "render_count": len(render_ms),
        "render_ms_median": sorted(render_ms)[len(render_ms) // 2] if render_ms else 0.0,
        "render_ms_total": sum(render_ms),
    }
    return {
        "schema": "preview_holdout_runtime_source_receipt.v1",
        "manifest": str(args.manifest),
        "source_roots": [str(p) for p in args.source_root],
        "out_dir": str(args.out_dir),
        "rows": rows,
        "missing": missing,
        "summary": {
            "images_in_manifest": len(manifest["images"]),
            "images_with_source": len({r["image_id"] for r in rows}),
            "rows": len(rows),
            "missing_images": len(missing),
        },
        "timing": timing,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--ref-root", type=Path, action="append")
    parser.add_argument("--source-root", type=Path, action="append")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--out-json", type=Path, required=True)
    parser.add_argument("--tmp-dir", type=Path, default=external_path('tmp'))
    parser.add_argument("--keep-renders", action="store_true")
    args = parser.parse_args()
    if args.ref_root is None:
        args.ref_root = DEFAULT_REF_ROOTS
    if args.source_root is None:
        args.source_root = DEFAULT_SOURCE_ROOTS
    args.tmp_dir.mkdir(parents=True, exist_ok=True)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    payload = build(args)
    args.out_json.write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps(payload["summary"], indent=2), flush=True)
    print(args.out_json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
