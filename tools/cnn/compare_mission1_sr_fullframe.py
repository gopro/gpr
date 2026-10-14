#!/usr/bin/env python3
"""Compare full-frame Mission 1 12MP->50MP SR output against a 50MP Bayer target."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

import cv2
import numpy as np
from PIL import Image, ImageDraw


RAW_SCALE = 16383.0
DEFAULT_LO_W = 4096
DEFAULT_LO_H = 3072


def read_raw(path: Path, width: int, height: int) -> np.ndarray:
    arr = np.fromfile(path, dtype="<u2")
    expected = width * height
    if arr.size != expected:
        raise ValueError(f"{path} has {arr.size} pixels, expected {expected}")
    return arr.reshape((height, width))


def deinterleave(bayer: np.ndarray) -> np.ndarray:
    return np.stack(
        [
            bayer[0::2, 0::2],
            bayer[0::2, 1::2],
            bayer[1::2, 0::2],
            bayer[1::2, 1::2],
        ],
        axis=0,
    )


def bilinear_planes(low_planes: np.ndarray, high_width: int, high_height: int) -> np.ndarray:
    return np.stack(
        [
            cv2.resize(ch.astype(np.float32), (high_width // 2, high_height // 2), interpolation=cv2.INTER_LINEAR)
            for ch in low_planes
        ],
        axis=0,
    )


def rmse(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.sqrt(np.mean((a.astype(np.float32) - b.astype(np.float32)) ** 2)))


def mae(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.mean(np.abs(a.astype(np.float32) - b.astype(np.float32))))


def gradient_mae(a: np.ndarray, b: np.ndarray) -> float:
    af = a.astype(np.float32)
    bf = b.astype(np.float32)
    dx = np.mean(np.abs(np.diff(af, axis=-1) - np.diff(bf, axis=-1)))
    dy = np.mean(np.abs(np.diff(af, axis=-2) - np.diff(bf, axis=-2)))
    return float((dx + dy) * 0.5)


def same_cell_detail(planes: np.ndarray) -> np.ndarray:
    pf = planes.astype(np.float32)
    _, h, w = pf.shape
    if h % 2 or w % 2:
        raise ValueError("same-cell detail metric expects even CFA-plane dimensions")
    cell_mean = pf.reshape((pf.shape[0], h // 2, 2, w // 2, 2)).mean(axis=(2, 4))
    return pf - np.repeat(np.repeat(cell_mean, 2, axis=1), 2, axis=2)


def cfa_plane_mae(a: np.ndarray, b: np.ndarray) -> dict[str, float]:
    labels = ("r", "g1", "g2", "b")
    return {label: mae(a[idx], b[idx]) for idx, label in enumerate(labels)}


def psnr14(value: float) -> float:
    return 99.0 if value == 0.0 else 20.0 * math.log10(RAW_SCALE / value)


def metric_row(candidate: np.ndarray, target: np.ndarray) -> dict[str, float]:
    r = rmse(candidate, target)
    return {
        "rmse_counts": r,
        "mae_counts": mae(candidate, target),
        "gradient_mae_counts": gradient_mae(candidate, target),
        "psnr14_db": psnr14(r),
    }


def detail_metric_row(candidate: np.ndarray, target: np.ndarray) -> dict[str, Any]:
    candidate_detail = same_cell_detail(candidate)
    target_detail = same_cell_detail(target)
    detail_mae = mae(candidate_detail, target_detail)
    return {
        "same_cell_detail_mae_counts": detail_mae,
        "same_cell_fine_detail_mae_counts": gradient_mae(candidate_detail, target_detail),
        "cfa_plane_detail_mae_counts": cfa_plane_mae(candidate_detail, target_detail),
    }


def improvement_pct(baseline: float, model: float) -> float:
    return 0.0 if baseline == 0.0 else 100.0 * (baseline - model) / baseline


def luma(planes: np.ndarray) -> np.ndarray:
    return planes.astype(np.float32).mean(axis=0)


def scale_crop(crop: np.ndarray, lo: float, hi: float) -> Image.Image:
    arr = np.clip((crop - lo) / max(hi - lo, 1.0), 0.0, 1.0)
    return Image.fromarray((arr * 255.0 + 0.5).astype(np.uint8), "L").convert("RGB")


def error_crop(candidate: np.ndarray, target: np.ndarray, scale: float) -> Image.Image:
    err = np.abs(candidate - target)
    arr = np.clip(err / max(scale, 1.0), 0.0, 1.0)
    return Image.fromarray((arr * 255.0 + 0.5).astype(np.uint8), "L").convert("RGB")


def write_contact(out: Path, target: np.ndarray, bilinear: np.ndarray, model: np.ndarray) -> None:
    target_l = luma(target)
    bilinear_l = luma(bilinear)
    model_l = luma(model)
    crops = [
        ("upper_left", 160, 160),
        ("center", target_l.shape[1] // 2 - 192, target_l.shape[0] // 2 - 192),
        ("lower_detail", target_l.shape[1] - 640, target_l.shape[0] - 640),
    ]
    panels = [("target", target_l), ("bilinear", bilinear_l), ("model", model_l), ("model error", None)]
    size = 384
    pad = 10
    label_h = 34
    sheet = Image.new(
        "RGB",
        (len(panels) * (size + pad) + pad, len(crops) * (size + label_h + pad) + pad),
        (24, 24, 24),
    )
    draw = ImageDraw.Draw(sheet)
    for row, (name, x, y) in enumerate(crops):
        stack = [target_l[y : y + size, x : x + size], bilinear_l[y : y + size, x : x + size], model_l[y : y + size, x : x + size]]
        lo, hi = np.percentile(np.concatenate([s.reshape(-1) for s in stack]), [0.5, 99.5])
        for col, (title, img) in enumerate(panels):
            x0 = pad + col * (size + pad)
            y0 = pad + row * (size + label_h + pad)
            draw.text((x0, y0), f"{name} - {title}", fill=(235, 235, 235))
            if img is None:
                crop_img = error_crop(model_l[y : y + size, x : x + size], target_l[y : y + size, x : x + size], scale=96.0)
            else:
                crop_img = scale_crop(img[y : y + size, x : x + size], lo, hi)
            sheet.paste(crop_img, (x0, y0 + label_h))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out, quality=92)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--low-raw", type=Path, required=True)
    ap.add_argument("--sr-raw", type=Path, required=True)
    ap.add_argument("--target-raw", type=Path, required=True)
    ap.add_argument("--low-width", type=int, default=DEFAULT_LO_W)
    ap.add_argument("--low-height", type=int, default=DEFAULT_LO_H)
    ap.add_argument("--high-width", type=int, help="defaults to 2x --low-width")
    ap.add_argument("--high-height", type=int, help="defaults to 2x --low-height")
    ap.add_argument("--out-json", type=Path, required=True)
    ap.add_argument("--contact-sheet", type=Path, required=True)
    args = ap.parse_args()

    high_width = args.high_width or args.low_width * 2
    high_height = args.high_height or args.low_height * 2
    if high_width != args.low_width * 2 or high_height != args.low_height * 2:
        raise ValueError("this comparator expects exactly 2x high dimensions")

    low = deinterleave(read_raw(args.low_raw, args.low_width, args.low_height))
    sr = deinterleave(read_raw(args.sr_raw, high_width, high_height))
    target = deinterleave(read_raw(args.target_raw, high_width, high_height))
    bilinear = bilinear_planes(low, high_width, high_height)
    sr_f = sr.astype(np.float32)
    target_f = target.astype(np.float32)

    baseline = metric_row(bilinear, target_f)
    model = metric_row(sr_f, target_f)
    baseline_detail = detail_metric_row(bilinear, target_f)
    model_detail = detail_metric_row(sr_f, target_f)
    cfa_baseline_mean = float(np.mean(list(baseline_detail["cfa_plane_detail_mae_counts"].values())))
    cfa_model_mean = float(np.mean(list(model_detail["cfa_plane_detail_mae_counts"].values())))
    payload: dict[str, Any] = {
        "schema": "mission1_sr_fullframe_compare.v1",
        "low_raw": str(args.low_raw),
        "sr_raw": str(args.sr_raw),
        "target_raw": str(args.target_raw),
        "low_width": args.low_width,
        "low_height": args.low_height,
        "high_width": high_width,
        "high_height": high_height,
        "baseline_bilinear": baseline,
        "model": model,
        "baseline_same_cell_detail": baseline_detail,
        "model_same_cell_detail": model_detail,
        "improvement_pct": {
            "rmse": improvement_pct(baseline["rmse_counts"], model["rmse_counts"]),
            "mae": improvement_pct(baseline["mae_counts"], model["mae_counts"]),
            "gradient_mae": improvement_pct(baseline["gradient_mae_counts"], model["gradient_mae_counts"]),
            "same_cell_detail_mae": improvement_pct(
                baseline_detail["same_cell_detail_mae_counts"],
                model_detail["same_cell_detail_mae_counts"],
            ),
            "same_cell_fine_detail_mae": improvement_pct(
                baseline_detail["same_cell_fine_detail_mae_counts"],
                model_detail["same_cell_fine_detail_mae_counts"],
            ),
            "cfa_plane_detail_mae": improvement_pct(cfa_baseline_mean, cfa_model_mean),
        },
        "contact_sheet": str(args.contact_sheet),
    }
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    write_contact(args.contact_sheet, target_f, bilinear, sr_f)
    print(json.dumps(payload, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
