#!/usr/bin/env python3
"""Package a retained Mission 1 8K SR Bayer raw into editable/review outputs."""
from __future__ import annotations


import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any

import numpy as np
import rawpy



import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import REPO, external_path

RAW_SCALE = 16383.0


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: list[str], log: Path) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    text = proc.stdout + proc.stderr
    log.write_text(text, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"command failed ({proc.returncode}): {' '.join(cmd)}\n{text[-2000:]}")
    return text


def run_raw_to_gpr_with_fallback(
    *,
    gpr_tools: Path,
    source_raw: Path,
    scratch_parent: Path,
    editable_gpr: Path,
    width: int,
    height: int,
    pixel_format: str,
    quality: int,
    out_dir: Path,
) -> tuple[str, str]:
    """Wrap SR raw as GPR, retrying direct-from-artifact if scratch wrapping crashes.

    Some 8K SR payloads have shown a path-sensitive SDK crash when the input and
    output live in a temporary scratch directory. The direct retained-artifact
    path is the production-relevant one, so keep it as an explicit fallback and
    record which route succeeded in the receipt.
    """
    scratch_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="srpkg_", dir=scratch_parent) as scratch_name:
        scratch = Path(scratch_name)
        scratch_raw = scratch / "in.raw"
        scratch_gpr = scratch / "out.gpr"
        shutil.copy2(source_raw, scratch_raw)
        try:
            log = run([
                str(gpr_tools),
                "-i",
                str(scratch_raw),
                "-w",
                str(width),
                "-h",
                str(height),
                "-x",
                pixel_format,
                "-o",
                str(scratch_gpr),
                "-q",
                str(quality),
            ], out_dir / "raw_to_sdk_wrapped_gpr.log")
            shutil.copy2(scratch_gpr, editable_gpr)
            return log, "scratch_copy"
        except RuntimeError as scratch_error:
            log = run([
                str(gpr_tools),
                "-i",
                str(source_raw),
                "-w",
                str(width),
                "-h",
                str(height),
                "-x",
                pixel_format,
                "-o",
                str(editable_gpr),
                "-q",
                str(quality),
            ], out_dir / "raw_to_sdk_wrapped_gpr_direct_fallback.log")
            fallback_log = (
                "scratch_copy_failed_then_direct_fallback_succeeded\n"
                f"scratch_error_tail:\n{str(scratch_error)[-1200:]}\n"
                f"direct_log_tail:\n{log[-1200:]}"
            )
            (out_dir / "raw_to_sdk_wrapped_gpr.log").write_text(fallback_log, encoding="utf-8")
            return fallback_log, "direct_fallback_after_scratch_failure"


def rawpy_shape(path: Path) -> list[int]:
    dng = rawpy.imread(str(path))
    try:
        return [int(dng.raw_image.shape[0]), int(dng.raw_image.shape[1])]
    finally:
        dng.close()


def raw_roundtrip_identical(dng: Path, out_raw: Path, gpr_tools: Path, log: Path) -> bool:
    run([str(gpr_tools), "-i", str(dng), "-o", str(out_raw)], log)
    return out_raw.read_bytes() == dng.with_suffix(".source.raw").read_bytes()


def metrics(src_raw: Path, dec_raw: Path) -> dict[str, Any]:
    src = np.fromfile(src_raw, dtype="<u2").astype(np.float64)
    dec = np.fromfile(dec_raw, dtype="<u2").astype(np.float64)
    if src.shape != dec.shape:
        raise ValueError(f"shape mismatch: {src.shape} != {dec.shape}")
    diff = src - dec
    rmse = float(np.sqrt(np.mean(diff * diff)))
    mae = float(np.mean(np.abs(diff)))
    psnr = 99.0 if rmse == 0.0 else 20.0 * math.log10(RAW_SCALE / rmse)
    return {
        "pixels": int(src.size),
        "rmse_dn": rmse,
        "mae_dn": mae,
        "psnr14_db": psnr,
        "max_abs_dn": int(np.max(np.abs(diff))) if src.size else 0,
    }


def ffprobe(path: Path) -> dict[str, Any]:
    out = subprocess.check_output([
        "ffprobe",
        "-v",
        "error",
        "-show_streams",
        "-show_format",
        "-of",
        "json",
        str(path),
    ])
    return json.loads(out)


def rel(path: Path, external_root: Path) -> str:
    try:
        return "artifacts/" + path.resolve().relative_to((external_root / "artifacts").resolve()).as_posix()
    except ValueError:
        return str(path)


def file_record(path: Path, external_root: Path) -> dict[str, Any]:
    return {
        "path": rel(path, external_root),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sr-raw", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--external-root", type=Path, default=external_path(''))
    ap.add_argument("--gpr-tools", type=Path, default=REPO / "build-local/source/app/gpr_tools/gpr_tools")
    ap.add_argument("--gpr2prores", type=Path, default=REPO / "tools/gpr2prores/gpr2prores")
    ap.add_argument("--width", type=int, default=8192)
    ap.add_argument("--height", type=int, default=6144)
    ap.add_argument("--pixel-format", default="rggb14")
    ap.add_argument("--gpr-quality", type=int, default=3)
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    source_raw = args.out_dir / args.sr_raw.name.replace(".raw", ".source.raw")
    source_raw.write_bytes(args.sr_raw.read_bytes())

    generic_dng = args.out_dir / "frame_000000_sr8k_generic.dng"
    editable_gpr = args.out_dir / "frame_000000_sr8k_sdk_wrapped.gpr"
    gpr_raw = args.out_dir / "frame_000000_sr8k_sdk_wrapped_from_gpr.raw"
    gpr_dng = args.out_dir / "frame_000000_sr8k_sdk_wrapped_from_gpr.dng"
    dng_raw = args.out_dir / "frame_000000_sr8k_generic_roundtrip.raw"
    prores = args.out_dir / "frame_000000_sr8k_review_2k_prores.mov"
    prores_twoframe = args.out_dir / "frame_000000_000001_sr8k_review_2k_prores.mov"
    prores_twoframe_dir = args.out_dir / "prores_fps_frames"

    raw_to_dng_log = run([
        str(args.gpr_tools),
        "-i",
        str(source_raw),
        "-w",
        str(args.width),
        "-h",
        str(args.height),
        "-x",
        args.pixel_format,
        "-o",
        str(generic_dng),
        "-q",
        "3",
    ], args.out_dir / "raw_to_generic_dng.log")
    scratch_parent = Path(os.environ.get("GPR_TMPDIR") or os.environ.get("TMPDIR") or args.out_dir)
    raw_to_gpr_log, raw_to_gpr_mode = run_raw_to_gpr_with_fallback(
        gpr_tools=args.gpr_tools,
        source_raw=source_raw,
        scratch_parent=scratch_parent,
        editable_gpr=editable_gpr,
        width=args.width,
        height=args.height,
        pixel_format=args.pixel_format,
        quality=args.gpr_quality,
        out_dir=args.out_dir,
    )
    gpr_to_raw_log = run([str(args.gpr_tools), "-i", str(editable_gpr), "-o", str(gpr_raw)], args.out_dir / "sdk_wrapped_gpr_to_raw.log")
    gpr_to_dng_log = run([str(args.gpr_tools), "-i", str(editable_gpr), "-o", str(gpr_dng)], args.out_dir / "sdk_wrapped_gpr_to_dng.log")
    dng_to_raw_log = run([str(args.gpr_tools), "-i", str(generic_dng), "-o", str(dng_raw)], args.out_dir / "dng_to_raw.log")
    prores_log = run([
        str(args.gpr2prores),
        "--max-frames",
        "1",
        "--fps",
        "24",
        "--no-codec",
        "--no-cnn",
        "--demosaic",
        "core-image",
        "--out-resolution",
        "2k",
        str(generic_dng),
        str(prores),
    ], args.out_dir / "sr_dng_to_prores.log")
    prores_twoframe_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(generic_dng, prores_twoframe_dir / "frame_000000.dng")
    shutil.copy2(generic_dng, prores_twoframe_dir / "frame_000001.dng")
    prores_twoframe_log = run([
        str(args.gpr2prores),
        "--max-frames",
        "2",
        "--fps",
        "24",
        "--no-codec",
        "--no-cnn",
        "--demosaic",
        "core-image",
        "--out-resolution",
        "2k",
        str(prores_twoframe_dir),
        str(prores_twoframe),
    ], args.out_dir / "sr_dng_to_prores_twoframe.log")
    shutil.rmtree(prores_twoframe_dir, ignore_errors=True)

    receipt = {
        "schema": "mission1_native12_gvid_to_8k_sr_packaging.v2",
        "sr_raw": {
            **file_record(source_raw, args.external_root),
            "width": args.width,
            "height": args.height,
        },
        "editable_dng": {
            **file_record(generic_dng, args.external_root),
            "rawpy_open_shape": rawpy_shape(generic_dng),
            "raw_roundtrip_byte_identical": source_raw.read_bytes() == dng_raw.read_bytes(),
        },
        "editable_gpr": {
            **file_record(editable_gpr, args.external_root),
            "quality": args.gpr_quality,
            "raw_to_gpr_mode": raw_to_gpr_mode,
            "readback_metrics": metrics(source_raw, gpr_raw),
            "gpr_to_dng_path": rel(gpr_dng, args.external_root),
            "gpr_to_dng_sha256": sha256_file(gpr_dng),
            "gpr_to_dng_rawpy_open_shape": rawpy_shape(gpr_dng),
            "raw_to_gpr_log_tail": raw_to_gpr_log[-1000:],
            "gpr_to_raw_log_tail": gpr_to_raw_log[-1000:],
            "gpr_to_dng_log_tail": gpr_to_dng_log[-1000:],
        },
        "prores_review": {
            **file_record(prores, args.external_root),
            "ffprobe": ffprobe(prores),
            "render_log_tail": prores_log[-1000:],
        },
        "prores_fps_review": {
            **file_record(prores_twoframe, args.external_root),
            "ffprobe": ffprobe(prores_twoframe),
            "render_log_tail": prores_twoframe_log[-1000:],
        },
        "logs": {
            "raw_to_dng_log_tail": raw_to_dng_log[-1000:],
            "dng_to_raw_log_tail": dng_to_raw_log[-1000:],
        },
    }
    out = args.out_dir / "packaging_receipt.json"
    out.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"receipt": str(out), "editable_gpr_psnr14_db": receipt["editable_gpr"]["readback_metrics"]["psnr14_db"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
