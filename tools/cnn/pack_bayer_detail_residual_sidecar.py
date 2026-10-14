#!/usr/bin/env python3
"""Prototype pack/unpack for same-color Bayer detail residual sidecars.

This is a Python feasibility prototype for a codec-side stream. Encoding uses
the source/clean low raw to build a sparse quantized residual; decoding uses
only the codec low raw plus the sidecar.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any

import numpy as np

from bayer_detail_residual import (
    PLANES,
    apply_detail_residual,
    deinterleave,
    parse_planes,
    read_raw,
    reinterleave,
    rmse,
    write_raw,
)


SCHEMA = "gpr.bayer_detail_residual_sidecar.v1"


def quantized_residual_planes(
    codec: np.ndarray,
    clean: np.ndarray,
    *,
    significant_detail_threshold: float,
    residual_threshold: float,
    quant_step: float,
    planes: set[int],
    max_value: int,
) -> np.ndarray:
    out, _ = apply_detail_residual(
        codec,
        clean,
        significant_detail_threshold=significant_detail_threshold,
        residual_threshold=residual_threshold,
        quant_step=quant_step,
        planes=planes,
        max_value=max_value,
    )
    return deinterleave(out).astype(np.int32) - deinterleave(codec).astype(np.int32)


def write_sidecar(path: Path, q: np.ndarray, metadata: dict[str, Any]) -> None:
    q_i16 = np.clip(q, -32768, 32767).astype("<i2", copy=False)
    nonzero = q_i16 != 0
    bitmap = np.packbits(nonzero.reshape(-1).astype(np.uint8))
    values = q_i16[nonzero].astype("<i2", copy=False)
    path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        path,
        metadata=np.asarray(json.dumps(metadata, sort_keys=True)),
        shape=np.asarray(q_i16.shape, dtype=np.int32),
        bitmap=bitmap,
        values=values,
    )


def load_sidecar(path: Path) -> tuple[np.ndarray, dict[str, Any]]:
    with np.load(path, allow_pickle=False) as z:
        metadata = json.loads(str(z["metadata"].item()))
        shape = tuple(int(v) for v in z["shape"].tolist())
        bitmap = np.unpackbits(z["bitmap"])[: int(np.prod(shape))].astype(bool)
        values = z["values"].astype("<i2", copy=False)
    q = np.zeros(int(np.prod(shape)), dtype="<i2")
    if values.size != int(np.count_nonzero(bitmap)):
        raise ValueError(f"{path}: values count does not match bitmap")
    q[bitmap] = values
    return q.reshape(shape), metadata


def encode(args: argparse.Namespace) -> int:
    started = time.perf_counter()
    codec = read_raw(args.codec_raw, args.width, args.height)
    clean = read_raw(args.clean_raw, args.width, args.height)
    planes = parse_planes(args.planes)
    q = quantized_residual_planes(
        codec,
        clean,
        significant_detail_threshold=args.significant_detail_threshold,
        residual_threshold=args.residual_threshold,
        quant_step=args.quant_step,
        planes=planes,
        max_value=args.max_value,
    )
    metadata = {
        "schema": SCHEMA,
        "width": args.width,
        "height": args.height,
        "planes": [PLANES[i] for i in sorted(planes)],
        "significant_detail_threshold": args.significant_detail_threshold,
        "residual_threshold": args.residual_threshold,
        "quant_step": args.quant_step,
        "max_value": args.max_value,
        "policy": "encoder_side_source_detail_residual_prototype",
    }
    write_sidecar(args.sidecar, q, metadata)
    elapsed = time.perf_counter() - started
    size = args.sidecar.stat().st_size
    nonzero = int(np.count_nonzero(q))
    receipt = {
        **metadata,
        "cmd": "encode",
        "codec_raw": str(args.codec_raw),
        "clean_raw": str(args.clean_raw),
        "sidecar": str(args.sidecar),
        "sidecar_bytes": size,
        "nonzero_samples": nonzero,
        "nonzero_pct": 100.0 * nonzero / float(q.size) if q.size else 0.0,
        "elapsed_s": elapsed,
    }
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2, sort_keys=True))
    return 0


def decode(args: argparse.Namespace) -> int:
    started = time.perf_counter()
    codec = read_raw(args.codec_raw, args.width, args.height)
    q, metadata = load_sidecar(args.sidecar)
    if tuple(q.shape) != tuple(deinterleave(codec).shape):
        raise ValueError(f"{args.sidecar}: sidecar shape {q.shape} does not match codec raw")
    out = np.clip(deinterleave(codec).astype(np.int32) + q.astype(np.int32), 0, int(metadata["max_value"]))
    out_raw = reinterleave(out.astype(np.uint16))
    write_raw(args.out_raw, out_raw)
    elapsed = time.perf_counter() - started
    receipt = {
        **metadata,
        "cmd": "decode",
        "codec_raw": str(args.codec_raw),
        "sidecar": str(args.sidecar),
        "out_raw": str(args.out_raw),
        "sidecar_bytes": args.sidecar.stat().st_size,
        "elapsed_s": elapsed,
    }
    if args.clean_raw:
        clean = read_raw(args.clean_raw, args.width, args.height)
        receipt["codec_clean_rmse"] = rmse(codec, clean)
        receipt["output_clean_rmse"] = rmse(out_raw, clean)
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2, sort_keys=True))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    enc = sub.add_parser("encode")
    enc.add_argument("--codec-raw", type=Path, required=True)
    enc.add_argument("--clean-raw", type=Path, required=True)
    enc.add_argument("--sidecar", type=Path, required=True)
    enc.add_argument("--width", type=int, required=True)
    enc.add_argument("--height", type=int, required=True)
    enc.add_argument("--planes", default="all")
    enc.add_argument("--significant-detail-threshold", type=float, default=2.0)
    enc.add_argument("--residual-threshold", type=float, default=1.0)
    enc.add_argument("--quant-step", type=float, default=2.0)
    enc.add_argument("--max-value", type=int, default=65535)
    enc.add_argument("--receipt", type=Path)

    dec = sub.add_parser("decode")
    dec.add_argument("--codec-raw", type=Path, required=True)
    dec.add_argument("--sidecar", type=Path, required=True)
    dec.add_argument("--out-raw", type=Path, required=True)
    dec.add_argument("--width", type=int, required=True)
    dec.add_argument("--height", type=int, required=True)
    dec.add_argument("--clean-raw", type=Path)
    dec.add_argument("--receipt", type=Path)

    args = ap.parse_args()
    if args.cmd == "encode":
        return encode(args)
    if args.cmd == "decode":
        return decode(args)
    raise ValueError(args.cmd)


if __name__ == "__main__":
    raise SystemExit(main())
