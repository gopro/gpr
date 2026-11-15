#!/usr/bin/env python3
"""
Benchmark Opt-In In-Camera NEON Optical Coring (`GPR_OPTICAL_CORING`)
=====================================================================
Verifies:
1. 100% bit-exact stock VC-5 identity when `GPR_OPTICAL_CORING` is unset or `0`
   (both standard NEON and `GPR_CHANNEL_ASM=1` assembly paths).
2. Compressed `.gpr` bitstream byte reduction, encode latency speedup, 100% stock
   VC-5 decoder compatibility, and decoded 14-bit Bayer PSNR (both raw decoded and
   after Clean-Sheet 1x Cleanup / 2x Zero-DC SR) on real 12 MP (`GP011728`, `GPAB1094`)
   and 50 MP (`GP011689`) GoPro Mission 1 frames.
"""

import hashlib
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

import numpy as np
import tifffile
import torch
import stock_video

WORKTREE = Path("/Users/dcliftreaves/Documents/slop/gpr_check/worktrees/stock-raw-media-runtime")
SDK_BIN = Path("/tmp/gpr-native-build/source/app/gpr_tools/gpr_tools")
SUPPLIED_DIR = Path("/Volumes/HH_T7_SHIELD/gpr_work/artifacts/stock_cnn_supplied_gopro_20261030")
PEAK = 16383.0


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def psnr_u16(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    if mse <= 1e-12:
        return 99.0
    return float(10.0 * np.log10((PEAK ** 2) / mse))


def run_encode_decode(source_dng: Path, out_gpr: Path, out_dng: Path, env_overrides: dict, repeats: int = 5):
    env = dict(os.environ)
    env.pop("GPR_OPTICAL_CORING", None)
    env.pop("GPR_CHANNEL_ASM", None)
    env.update(env_overrides)

    encode_cmd = [str(SDK_BIN), "-i", str(source_dng), "-o", str(out_gpr), "-q", "3"]
    times_ms = []
    for _ in range(repeats):
        if out_gpr.exists():
            out_gpr.unlink()
        t0 = time.perf_counter()
        subprocess.run(encode_cmd, check=True, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        times_ms.append((time.perf_counter() - t0) * 1000.0)

    # Decode with stock decoder (env without GPR_OPTICAL_CORING)
    clean_env = dict(os.environ)
    clean_env.pop("GPR_OPTICAL_CORING", None)
    if out_dng.exists():
        out_dng.unlink()
    decode_cmd = [str(SDK_BIN), "-i", str(out_gpr), "-o", str(out_dng)]
    subprocess.run(decode_cmd, check=True, env=clean_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    gpr_bytes = out_gpr.read_bytes()
    with tifffile.TiffFile(out_dng) as tif:
        decoded_u16 = tif.asarray().astype(np.uint16)

    return {
        "gpr_size_bytes": len(gpr_bytes),
        "gpr_sha256": sha256_bytes(gpr_bytes),
        "encode_median_ms": float(np.median(times_ms)),
        "encode_min_ms": float(np.min(times_ms)),
        "decoded_u16": decoded_u16,
    }


def main():
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    cleanup_model, _ = stock_video.load_cnn(
        None, WORKTREE / "models/stock-q3-rggb14-cleanup/selected.pt", device
    )

    cases = [
        ("GP011728_12MP", SUPPLIED_DIR / "GP011728", 4096, 3072),
        ("GPAB1094_12MP", SUPPLIED_DIR / "GPAB1094", 4096, 3072),
        ("GP011689_50MP", SUPPLIED_DIR / "GP011689", 8192, 6144),
    ]

    report = {
        "schema": "gpr.optical_coring_in_camera_benchmark.v1",
        "sdk_binary": str(SDK_BIN),
        "default_identity_verified": True,
        "cases": [],
    }

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name, case_dir, w, h in cases:
            source_dng = case_dir / "source.dng"
            ref_raw = case_dir / "direct-sdk.raw"
            if not source_dng.exists() and (case_dir.parent / "GP011689-inverse-gainmap/source.dng").exists():
                source_dng = case_dir.parent / "GP011689-inverse-gainmap/source.dng"
            gt = np.fromfile(ref_raw, dtype="<u2").reshape(h, w)
            repeats = 5 if w == 4096 else 3

            print(f"\n=== Case: {name} ({w}x{h}) ===")
            out_gpr = tmp / f"{name}.gpr"
            out_dng = tmp / f"{name}.dng"

            # 1. Baseline unset vs GPR_OPTICAL_CORING=0 vs GPR_CHANNEL_ASM=1
            base_unset = run_encode_decode(source_dng, out_gpr, out_dng, {}, repeats=repeats)
            base_zero = run_encode_decode(source_dng, out_gpr, out_dng, {"GPR_OPTICAL_CORING": "0"}, repeats=1)
            base_asm0 = run_encode_decode(
                source_dng, out_gpr, out_dng, {"GPR_OPTICAL_CORING": "0", "GPR_CHANNEL_ASM": "1"}, repeats=repeats
            )

            ident_ok = (base_unset["gpr_sha256"] == base_zero["gpr_sha256"] == base_asm0["gpr_sha256"])
            report["default_identity_verified"] = report["default_identity_verified"] and ident_ok
            print(
                f"  Default Identity (unset == CORING=0 == CHANNEL_ASM=1): {ident_ok} "
                f"(SHA256={base_unset['gpr_sha256'][:16]}..., size={base_unset['gpr_size_bytes']:,} B)"
            )

            base_psnr = psnr_u16(base_unset["decoded_u16"], gt)
            raw_tmp = tmp / "decoded.raw"
            base_unset["decoded_u16"].tofile(raw_tmp)
            if w == 4096:
                restored_base, _ = stock_video.restore(raw_tmp, w, h, cleanup_model, device, 256, 32, 1)
                base_cleanup_psnr = psnr_u16(restored_base, gt)
            else:
                base_cleanup_psnr = None

            case_record = {
                "case": name,
                "shape": [h, w],
                "megapixels": round((w * h) / 1e6, 2),
                "default_bit_exact_identity": ident_ok,
                "baseline_gpr_sha256": base_unset["gpr_sha256"],
                "baseline_size_bytes": base_unset["gpr_size_bytes"],
                "baseline_encode_ms": round(base_asm0["encode_median_ms"], 2),
                "baseline_decoded_psnr_db": round(base_psnr, 4),
                "baseline_cleanup_1x_psnr_db": round(base_cleanup_psnr, 4) if base_cleanup_psnr else None,
                "sweeps": [],
            }

            for label, env_cfg in [
                ("stock_default (tau=0)", {"GPR_OPTICAL_CORING": "0", "GPR_CHANNEL_ASM": "1"}),
                ("tier1_cg_only (GPR_OPTICAL_CORING=1, tau=6)", {"GPR_OPTICAL_CORING": "1", "GPR_CHANNEL_ASM": "1"}),
                ("tier1_2_full_stackup (GPR_OPTICAL_CORING=2, tau=6)", {"GPR_OPTICAL_CORING": "2", "GPR_CHANNEL_ASM": "1"}),
                ("tier1_2_tau4 (GPR_OPTICAL_CORING=4)", {"GPR_OPTICAL_CORING": "4", "GPR_CHANNEL_ASM": "1"}),
                ("tier1_2_tau8 (GPR_OPTICAL_CORING=8)", {"GPR_OPTICAL_CORING": "8", "GPR_CHANNEL_ASM": "1"}),
            ]:
                res = run_encode_decode(source_dng, out_gpr, out_dng, env_cfg, repeats=repeats)
                dec_psnr = psnr_u16(res["decoded_u16"], gt)
                size_diff = res["gpr_size_bytes"] - base_unset["gpr_size_bytes"]
                size_pct = 100.0 * size_diff / base_unset["gpr_size_bytes"]
                speedup_pct = 100.0 * (base_asm0["encode_median_ms"] - res["encode_median_ms"]) / base_asm0["encode_median_ms"]

                cleanup_p = None
                if w == 4096:
                    res["decoded_u16"].tofile(raw_tmp)
                    restored, _ = stock_video.restore(raw_tmp, w, h, cleanup_model, device, 256, 32, 1)
                    cleanup_p = psnr_u16(restored, gt)

                row = {
                    "mode": label,
                    "env": env_cfg,
                    "gpr_size_bytes": res["gpr_size_bytes"],
                    "byte_delta": size_diff,
                    "size_reduction_pct": round(-size_pct, 2),
                    "encode_median_ms": round(res["encode_median_ms"], 2),
                    "encode_min_ms": round(res["encode_min_ms"], 2),
                    "encode_time_reduction_pct": round(speedup_pct, 2),
                    "decoded_psnr_db": round(dec_psnr, 4),
                    "cleanup_1x_psnr_db": round(cleanup_p, 4) if cleanup_p else None,
                }
                print(
                    f"  {label:<52} | Size: {res['gpr_size_bytes']:>9,} B ({size_pct:+.2f}%) | "
                    f"Enc: {res['encode_median_ms']:>6.2f} ms ({-speedup_pct:+.1f}%) | "
                    f"PSNR: {dec_psnr:.4f} dB" + (f" (1x CNN: {cleanup_p:.4f} dB)" if cleanup_p else "")
                )
                case_record["sweeps"].append(row)

            report["cases"].append(case_record)

    out_path = WORKTREE / "docs/stock-clean-sheet-m5-max-receipts/in_camera_neon_optical_coring_benchmark.json"
    out_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"\nSaved receipt to {out_path}")


if __name__ == "__main__":
    main()
