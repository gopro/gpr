#!/usr/bin/env python3
"""Bayer detail-residual math shared by the sidecar encoder and decoder."""

from __future__ import annotations

import math
import zlib
from pathlib import Path
from typing import Any

import numpy as np


SCHEMA = "gpr.bayer_detail_residual_oracle_raw.v1"
PLANES = ("r", "g1", "g2", "b")


def read_raw(path: Path, width: int, height: int) -> np.ndarray:
    arr = np.fromfile(path, dtype="<u2")
    expected = width * height
    if arr.size != expected:
        raise ValueError(f"{path} has {arr.size} samples, expected {expected}")
    return arr.reshape((height, width))


def write_raw(path: Path, raw: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    raw.astype("<u2", copy=False).tofile(path)


def deinterleave(raw: np.ndarray) -> np.ndarray:
    return np.stack(
        [
            raw[0::2, 0::2],
            raw[0::2, 1::2],
            raw[1::2, 0::2],
            raw[1::2, 1::2],
        ],
        axis=0,
    )


def reinterleave(planes: np.ndarray) -> np.ndarray:
    _, h, w = planes.shape
    out = np.empty((h * 2, w * 2), dtype=np.uint16)
    out[0::2, 0::2] = planes[0]
    out[0::2, 1::2] = planes[1]
    out[1::2, 0::2] = planes[2]
    out[1::2, 1::2] = planes[3]
    return out


def blur3_reflect(plane: np.ndarray) -> np.ndarray:
    padded = np.pad(plane.astype(np.float32), 1, mode="reflect")
    return (
        padded[:-2, :-2]
        + 2.0 * padded[:-2, 1:-1]
        + padded[:-2, 2:]
        + 2.0 * padded[1:-1, :-2]
        + 4.0 * padded[1:-1, 1:-1]
        + 2.0 * padded[1:-1, 2:]
        + padded[2:, :-2]
        + 2.0 * padded[2:, 1:-1]
        + padded[2:, 2:]
    ) * (1.0 / 16.0)


def parse_planes(value: str) -> set[int]:
    names = {item.strip().lower() for item in value.split(",") if item.strip()}
    if not names or "all" in names:
        return set(range(4))
    unknown = sorted(names - set(PLANES))
    if unknown:
        raise ValueError(f"unknown plane names: {', '.join(unknown)}")
    return {PLANES.index(name) for name in names}


def quantize_residual(residual: np.ndarray, quant_step: float) -> np.ndarray:
    if quant_step <= 0.0:
        raise ValueError("--quant-step must be positive")
    return np.rint(residual / quant_step) * quant_step


def compression_estimates(q: np.ndarray) -> dict[str, int]:
    q_i16 = np.clip(np.rint(q), -32768, 32767).astype("<i2", copy=False)
    nonzero = q_i16 != 0
    bitmap = np.packbits(nonzero.reshape(-1).astype(np.uint8))
    values = q_i16[nonzero].astype("<i2", copy=False)
    dense_zlib = zlib.compress(q_i16.tobytes(), level=6)
    bitmap_zlib = zlib.compress(bitmap.tobytes(), level=6)
    values_zlib = zlib.compress(values.tobytes(), level=6)
    return {
        "dense_i16_zlib_bytes": len(dense_zlib),
        "bitmap_zlib_bytes": len(bitmap_zlib),
        "values_i16_zlib_bytes": len(values_zlib),
        "bitmap_values_zlib_bytes": len(bitmap_zlib) + len(values_zlib),
    }


def estimate_sidecar_bits(q: np.ndarray, plane_count: int) -> dict[str, Any]:
    nonzero = q != 0.0
    nnz = int(np.count_nonzero(nonzero))
    total = int(q.size)
    if nnz:
        max_symbol = int(np.max(np.abs(q[nonzero])))
        mag_bits = max(1, int(math.ceil(math.log2(max_symbol + 1))))
    else:
        max_symbol = 0
        mag_bits = 0
    index_bits = max(1, int(math.ceil(math.log2(max(1, total)))))
    sparse_bits = nnz * (index_bits + 1 + mag_bits)
    bitmap_bits = total + nnz * (1 + mag_bits)
    return {
        "plane_count": int(plane_count),
        "total_samples": total,
        "nonzero_samples": nnz,
        "nonzero_pct": 100.0 * nnz / float(total) if total else 0.0,
        "max_abs_quantized_residual": max_symbol,
        "magnitude_bits": mag_bits,
        "index_bits": index_bits,
        "sparse_bits_estimate": int(sparse_bits),
        "bitmap_bits_estimate": int(bitmap_bits),
        "sparse_bytes_estimate": int((sparse_bits + 7) // 8),
        "bitmap_bytes_estimate": int((bitmap_bits + 7) // 8),
        **compression_estimates(q),
    }


def apply_detail_residual(
    codec: np.ndarray,
    clean: np.ndarray,
    *,
    significant_detail_threshold: float,
    residual_threshold: float,
    quant_step: float,
    planes: set[int],
    max_value: int,
) -> tuple[np.ndarray, dict[str, Any]]:
    codec_planes = deinterleave(codec)
    clean_planes = deinterleave(clean)
    out = codec_planes.astype(np.float32).copy()
    all_q = []
    rows: dict[str, Any] = {}
    for i, name in enumerate(PLANES):
        codec_detail = codec_planes[i].astype(np.float32) - blur3_reflect(codec_planes[i])
        clean_detail = clean_planes[i].astype(np.float32) - blur3_reflect(clean_planes[i])
        residual = clean_detail - codec_detail
        if i not in planes:
            q = np.zeros_like(residual, dtype=np.float32)
        else:
            mask = np.ones_like(residual, dtype=bool)
            if significant_detail_threshold > 0.0:
                mask &= np.abs(clean_detail) >= significant_detail_threshold
            if residual_threshold > 0.0:
                mask &= np.abs(residual) >= residual_threshold
            q = np.where(mask, quantize_residual(residual, quant_step), 0.0).astype(np.float32)
            out[i] += q
        all_q.append(q)
        plane_bits = estimate_sidecar_bits(q, 1)
        rows[name] = {
            **plane_bits,
            "residual_rmse_before_counts": rmse(codec_planes[i], clean_planes[i]),
            "residual_rmse_after_counts": rmse(out[i], clean_planes[i]),
        }
    stacked_q = np.stack(all_q, axis=0)
    receipt = {
        "sidecar": estimate_sidecar_bits(stacked_q, len(planes)),
        "planes": rows,
    }
    return np.clip(np.rint(reinterleave(out)), 0, max_value).astype(np.uint16), receipt


def rmse(a: np.ndarray, b: np.ndarray) -> float:
    diff = a.astype(np.float32) - b.astype(np.float32)
    return float(np.sqrt(np.mean(diff * diff)))
