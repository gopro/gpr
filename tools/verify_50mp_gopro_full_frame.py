#!/usr/bin/env python3
"""
Full-Frame 50 MP (8192 x 6144) GoPro Mission 1 Verification Suite
=================================================================
Tests the Clean-Sheet Zero-DC Neural Models (`F_ane` 2x SR, `1x -> 2x` Cascade,
and `Mission1OpticalHybridW96`) + Analytical Optical Stackup on:
  1. `GP011689` (8192x6144) Supplied GoPro 50 MP Stock-Q3 Case (`decoded.raw` -> `direct-sdk.raw`)
  2. All 4 Full-Frame 50 MP GoPro Sample Streams (`GP010114`, `GP010130`, `GP010136`, `GP011689`)
  3. All 10 Uncompressed DNG-Decoded 50 MP GoPro Mission 1 Frames (`GP017346` .. `GP017604`)
"""

import json
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

WORKTREE = Path("/Users/dcliftreaves/Documents/slop/gpr_check/worktrees/stock-raw-media-runtime")
sys.path.insert(0, str(WORKTREE / "tools"))
sys.path.insert(0, "/Users/dcliftreaves/Documents/slop/gpr_check/analysis")
sys.path.insert(0, "/Volumes/OWC_8TB/gpr_work/artifacts/m5_max_cnn_optimization_20261113/track2_w96")
import stock_cnn_models
import mission1_sr_models as m1_models
from finalize_full_throttle_results import Mission1OpticalHybridW96

PEAK = 16383.0
ARTIFACT_DIR = Path("/Volumes/OWC_8TB/gpr_work/artifacts/m5_max_clean_sheet_20261113")


def psnr_db(pred: torch.Tensor, target: torch.Tensor) -> float:
    mse = float(torch.mean((pred.float() - target.float()) ** 2).item())
    if mse <= 1e-12:
        return 99.0
    return float(-10.0 * np.log10(mse))


def pack_bayer_planes(raw_hw: np.ndarray) -> torch.Tensor:
    """Convert (H, W) uint16 RGGB to (1, 4, H/2, W/2) float32 in [0, 1]."""
    f = raw_hw.astype(np.float32) / PEAK
    planes = np.stack([f[0::2, 0::2], f[0::2, 1::2], f[1::2, 0::2], f[1::2, 1::2]], axis=0)
    return torch.from_numpy(planes).unsqueeze(0)


def load_f_ane_checkpoint(ckpt_path: Path, device: torch.device):
    ckpt = torch.load(ckpt_path, map_location="cpu", weights_only=False)
    variant = ckpt["variant"]
    scale = float(ckpt["residual_scale"])
    out_scale = 2 if variant in ("F_ane", "bibo_2x") else 1
    backbone = stock_cnn_models.build(variant)
    backbone.load_state_dict(ckpt["backbone_state"], strict=True)
    backbone.to(device).eval()
    return backbone, scale, out_scale


@torch.no_grad()
def run_tiled_sr(x_low: torch.Tensor, backbone, scale: float, device: torch.device, tile: int = 512, halo: int = 32) -> torch.Tensor:
    """Tiled 2x SR inference on (1, 4, H_low, W_low) -> (1, 4, 2*H_low, 2*W_low)."""
    _, c, h, w = x_low.shape
    out = torch.empty((1, c, h * 2, w * 2), dtype=torch.float32, device="cpu")
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            y1, x1 = min(y + tile, h), min(x + tile, w)
            ya, xa = max(0, y - halo), max(0, x - halo)
            yb, xb = min(h, y1 + halo), min(w, x1 + halo)
            patch = x_low[:, :, ya:yb, xa:xb].to(device)
            base = F.interpolate(patch, scale_factor=2, mode="bicubic", align_corners=False).clamp(0.0, 1.0)
            pred = (base + scale * backbone(patch)).clamp(0.0, 1.0)
            cy0, cy1 = (y - ya) * 2, (y1 - ya) * 2
            cx0, cx1 = (x - xa) * 2, (x1 - xa) * 2
            out[:, :, y * 2 : y1 * 2, x * 2 : x1 * 2] = pred[:, :, cy0:cy1, cx0:cx1].cpu()
    return out


@torch.no_grad()
def run_tiled_w96(x_low: torch.Tensor, model: torch.nn.Module, device: torch.device, black_counts: float = 512.0, white_counts: float = 16383.0, tile: int = 512, halo: int = 32) -> torch.Tensor:
    _, c, h, w = x_low.shape
    out = torch.empty((1, c, h * 2, w * 2), dtype=torch.float32, device="cpu")
    # Build full-frame normalized radial coordinates in [-1, 1]
    yy = torch.linspace(-1.0, 1.0, h, dtype=torch.float32).view(1, 1, h, 1).expand(1, 1, h, w)
    xx = torch.linspace(-1.0, 1.0, w, dtype=torch.float32).view(1, 1, 1, w).expand(1, 1, h, w)
    coords = torch.cat([xx, yy], dim=1)
    # Exact Mission 1 canonicalization from dataset.json:
    # gain = (15892 - 1008) / (white - black), offset = (1008 - black * gain) / 16383
    gain = (15892.0 - 1008.0) / (white_counts - black_counts)
    offset = (1008.0 - black_counts * gain) / PEAK
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            y1, x1 = min(y + tile, h), min(x + tile, w)
            ya, xa = max(0, y - halo), max(0, x - halo)
            yb, xb = min(h, y1 + halo), min(w, x1 + halo)
            patch_x = x_low[:, :, ya:yb, xa:xb].to(device)
            patch_co = coords[:, :, ya:yb, xa:xb].to(device)
            inp = torch.cat([(patch_x * gain + offset).clamp(0.0, 1.0), patch_co], dim=1)
            pred = ((model(inp) - offset) / gain).clamp(0.0, 1.0)
            cy0, cy1 = (y - ya) * 2, (y1 - ya) * 2
            cx0, cx1 = (x - xa) * 2, (x1 - xa) * 2
            out[:, :, y * 2 : y1 * 2, x * 2 : x1 * 2] = pred[:, :, cy0:cy1, cx0:cx1].cpu()
    return out


@torch.no_grad()
def run_tiled_1x(x_in: torch.Tensor, backbone, scale: float, device: torch.device, tile: int = 512, halo: int = 32) -> torch.Tensor:
    _, c, h, w = x_in.shape
    out = torch.empty((1, c, h, w), dtype=torch.float32, device="cpu")
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            y1, x1 = min(y + tile, h), min(x + tile, w)
            ya, xa = max(0, y - halo), max(0, x - halo)
            yb, xb = min(h, y1 + halo), min(w, x1 + halo)
            patch = x_in[:, :, ya:yb, xa:xb].to(device)
            pred = (patch + scale * backbone(patch)).clamp(0.0, 1.0)
            cy0, cy1 = (y - ya), (y1 - ya)
            cx0, cx1 = (x - xa), (x1 - xa)
            out[:, :, y:y1, x:x1] = pred[:, :, cy0:cy1, cx0:cx1].cpu()
    return out


def compute_detailed_metrics(pred_4ch: torch.Tensor, target_4ch: torch.Tensor) -> dict:
    """Compute full-frame PSNR, RMSE counts, bias counts, detail/shadow/highlight deciles, and per-site PSNR."""
    diff = (pred_4ch - target_4ch).squeeze(0).numpy() * PEAK
    ref = target_4ch.squeeze(0).numpy() * PEAK
    mse = float(np.mean(diff ** 2))
    psnr = float(20.0 * np.log10(PEAK) - 10.0 * np.log10(max(mse, 1e-12)))
    bias = float(np.mean(diff))

    # Detail top decile (on green luminance gradient)
    grad = np.abs(ref[:, 1:, :-1] - ref[:, :-1, :-1]) + np.abs(ref[:, :-1, 1:] - ref[:, :-1, :-1])
    thresh = float(np.percentile(grad, 90))
    detail_mse = float(np.mean(diff[:, :-1, :-1][grad >= thresh] ** 2))
    detail_psnr = float(20.0 * np.log10(PEAK) - 10.0 * np.log10(max(detail_mse, 1e-12)))

    low_t, high_t = np.percentile(ref, (10, 90))
    shadow_mse = float(np.mean(diff[ref <= low_t] ** 2))
    shadow_psnr = float(20.0 * np.log10(PEAK) - 10.0 * np.log10(max(shadow_mse, 1e-12)))
    high_mse = float(np.mean(diff[ref >= high_t] ** 2))
    high_psnr = float(20.0 * np.log10(PEAK) - 10.0 * np.log10(max(high_mse, 1e-12)))

    site_psnr = {}
    for idx, name in enumerate(("R", "G1", "G2", "B")):
        s_mse = float(np.mean(diff[idx] ** 2))
        site_psnr[name] = float(20.0 * np.log10(PEAK) - 10.0 * np.log10(max(s_mse, 1e-12)))

    return {
        "psnr_db": psnr,
        "rmse_counts": float(np.sqrt(mse)),
        "bias_counts": bias,
        "detail_top_decile_psnr_db": detail_psnr,
        "shadow_bottom_decile_psnr_db": shadow_psnr,
        "highlight_top_decile_psnr_db": high_psnr,
        "site_psnr_db": site_psnr,
    }


def main():
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    print("=" * 85)
    print(f"  FULL-FRAME 50 MP (8192 x 6144) GOPRO MISSION 1 VERIFICATION SUITE ({device})")
    print("=" * 85)

    # Load models
    cleanup_bb, cleanup_scale, _ = load_f_ane_checkpoint(ARTIFACT_DIR / "clean_sheet_cleanup_1x.pt", device)
    sr_bb, sr_scale, _ = load_f_ane_checkpoint(ARTIFACT_DIR / "clean_sheet_sr_2x.pt", device)
    old_sr_bb, old_sr_scale, _ = load_f_ane_checkpoint(
        WORKTREE / "models/stock-q3-rggb14-sr-diagnostic/selected.pt", device
    )

    w96_ckpt = torch.load(ARTIFACT_DIR / "clean_sheet_wavelet_w96.pt", map_location="cpu", weights_only=False)
    base_w96 = m1_models.make_model_from_config(w96_ckpt["config"])
    w96_model = Mission1OpticalHybridW96(base_w96, torch.zeros(4, 6))
    w96_model.load_state_dict(w96_ckpt["hybrid_state"], strict=True)
    w96_model.to(device).eval()

    results = {"device": str(device), "gopro_50mp_cases": [], "mission1_dng_50mp_cases": []}

    # -------------------------------------------------------------------------
    # PART 1: Supplied GoPro 50 MP Benchmark Case (GP011689: Stock Q3 12MP -> 50MP)
    # -------------------------------------------------------------------------
    gp11689_ref_path = Path("/Volumes/HH_T7_SHIELD/gpr_work/artifacts/stock_cnn_supplied_gopro_20261030/GP011689/direct-sdk.raw")
    gp11689_low_path = Path("/Volumes/HH_T7_SHIELD/gpr_work/artifacts/stock_cnn_supplied_gopro_20261030/GP011689-inverse-gainmap/decoded.raw")

    if gp11689_ref_path.exists() and gp11689_low_path.exists():
        print("\n[Part 1] Supplied GoPro 50 MP Stock-Q3 Benchmark: GP011689 (4096x3072 -> 8192x6144)")
        ref_hw = np.fromfile(gp11689_ref_path, dtype="<u2").reshape(6144, 8192)
        low_hw = np.fromfile(gp11689_low_path, dtype="<u2").reshape(3072, 4096)
        y_true = pack_bayer_planes(ref_hw)
        x_low = pack_bayer_planes(low_hw)

        # Notice: in GP011689-inverse-gainmap, the baseline creates 2x by nearest/box or bicubic:
        bicubic_pred = F.interpolate(x_low, scale_factor=2, mode="bicubic", align_corners=False).clamp(0.0, 1.0)
        old_pred = run_tiled_sr(x_low, old_sr_bb, old_sr_scale, device)
        t0 = time.perf_counter()
        new_sr_pred = run_tiled_sr(x_low, sr_bb, sr_scale, device)
        dt_sr = time.perf_counter() - t0

        # Also test unit scale (eta = 1.0 -> scale = 0.0003125)
        new_sr_unit_pred = run_tiled_sr(x_low, sr_bb, sr_scale / 0.65, device)
        # Cascade 1x -> 2x
        x_clean = run_tiled_1x(x_low, cleanup_bb, cleanup_scale, device)
        cascade_pred = run_tiled_sr(x_clean, sr_bb, sr_scale / 0.65, device)
        # W96
        w96_pred = run_tiled_w96(x_low, w96_model, device)

        m_bic = compute_detailed_metrics(bicubic_pred, y_true)
        m_old = compute_detailed_metrics(old_pred, y_true)
        m_sr = compute_detailed_metrics(new_sr_pred, y_true)
        m_sr_unit = compute_detailed_metrics(new_sr_unit_pred, y_true)
        m_casc = compute_detailed_metrics(cascade_pred, y_true)
        m_w96 = compute_detailed_metrics(w96_pred, y_true)

        print(f"  - Bicubic Baseline:            PSNR = {m_bic['psnr_db']:.4f} dB (Bias = {m_bic['bias_counts']:+.2f} cnts)")
        print(f"  - Old Unprojected SR (10c96f): PSNR = {m_old['psnr_db']:.4f} dB ({m_old['psnr_db'] - m_bic['psnr_db']:+.4f} dB, Bias = {m_old['bias_counts']:+.2f} cnts)")
        print(f"  - Clean-Sheet 2x SR (eta=.65): PSNR = {m_sr['psnr_db']:.4f} dB ({m_sr['psnr_db'] - m_bic['psnr_db']:+.4f} dB vs bicubic, {m_sr['psnr_db'] - m_old['psnr_db']:+.4f} dB vs old SR, Bias = {m_sr['bias_counts']:+.2f} cnts, {dt_sr*1000:.0f} ms)")
        print(f"  - Clean-Sheet 2x SR (eta=1.0): PSNR = {m_sr_unit['psnr_db']:.4f} dB ({m_sr_unit['psnr_db'] - m_bic['psnr_db']:+.4f} dB vs bicubic, {m_sr_unit['psnr_db'] - m_old['psnr_db']:+.4f} dB vs old SR)")
        print(f"  - Clean-Sheet 1x->2x Cascade:  PSNR = {m_casc['psnr_db']:.4f} dB ({m_casc['psnr_db'] - m_bic['psnr_db']:+.4f} dB vs bicubic, {m_casc['psnr_db'] - m_old['psnr_db']:+.4f} dB vs old SR)")
        print(f"  - Mission1OpticalHybridW96:    PSNR = {m_w96['psnr_db']:.4f} dB ({m_w96['psnr_db'] - m_bic['psnr_db']:+.4f} dB vs bicubic, {m_w96['psnr_db'] - m_old['psnr_db']:+.4f} dB vs old SR)")

        results["gp011689_supplied_benchmark"] = {
            "bicubic": m_bic,
            "old_unprojected_sr": m_old,
            "clean_sheet_sr_eta065": m_sr,
            "clean_sheet_sr_eta100": m_sr_unit,
            "clean_sheet_cascade": m_casc,
            "mission1_hybrid_w96": m_w96,
            "inference_ms": dt_sr * 1000.0,
        }

    # -------------------------------------------------------------------------
    # PART 2: All 4 Full-Frame 50 MP GoPro Sample Streams (GP010114, GP010130, GP010136, GP011689)
    # -------------------------------------------------------------------------
    print("\n[Part 2] 4 Full-Frame 50 MP (8192x6144) GoPro Sample Streams (Box-2x2 12MP -> 50MP):")
    sample_dir = Path("/Users/dcliftreaves/Documents/slop/gpr_check/analysis/user_sample_streams")
    for stem in ("GP010114", "GP010130", "GP010136", "GP011689"):
        raw_path = sample_dir / f"{stem}.raw"
        if not raw_path.exists():
            continue
        ref_hw = np.fromfile(raw_path, dtype="<u2").reshape(6144, 8192)
        y_true = pack_bayer_planes(ref_hw)
        # Same-color 2x2 box average to 12 MP (4096x3072)
        x_low = F.avg_pool2d(y_true, kernel_size=2, stride=2)

        bic_pred = F.interpolate(x_low, scale_factor=2, mode="bicubic", align_corners=False).clamp(0.0, 1.0)
        old_pred = run_tiled_sr(x_low, old_sr_bb, old_sr_scale, device)
        sr_pred = run_tiled_sr(x_low, sr_bb, sr_scale / 0.65, device)
        w96_pred = run_tiled_w96(x_low, w96_model, device)

        p_bic = psnr_db(bic_pred, y_true)
        p_old = psnr_db(old_pred, y_true)
        p_sr = psnr_db(sr_pred, y_true)
        p_w96 = psnr_db(w96_pred, y_true)

        print(f"  - {stem} (8192x6144): Bicubic={p_bic:.3f} dB | OldSR={p_old:.3f} dB ({p_old-p_bic:+.3f}) | "
              f"ZeroDC_F_ane={p_sr:.3f} dB ({p_sr-p_bic:+.3f} vs bic, {p_sr-p_old:+.3f} vs old) | "
              f"HybridW96={p_w96:.3f} dB ({p_w96-p_bic:+.3f} vs bic)")
        results["gopro_50mp_cases"].append({
            "stem": stem,
            "bicubic_psnr_db": p_bic,
            "old_sr_psnr_db": p_old,
            "clean_sheet_f_ane_psnr_db": p_sr,
            "gain_vs_bicubic_db": p_sr - p_bic,
            "gain_vs_old_sr_db": p_sr - p_old,
            "hybrid_w96_psnr_db": p_w96,
            "w96_gain_vs_bicubic_db": p_w96 - p_bic,
        })

    # -------------------------------------------------------------------------
    # PART 3: All 10 Uncompressed DNG-Decoded 50 MP GoPro Mission 1 Frames
    # -------------------------------------------------------------------------
    print("\n[Part 3] 10 Uncompressed DNG-Decoded 50 MP (8192x6144) GoPro Mission 1 Frames (GP017346..GP017604):")
    dng50_dir = Path("/Volumes/HH_T7_SHIELD/gpr_work/artifacts/mission1_dngraw_to_12mp_gvid_review_20260621/dng_raw50")
    stems_10 = ["GP017346", "GP017349", "GP017352", "GP017389", "GP017396",
                "GP017451", "GP017497", "GP017503", "GP017508", "GP017604"]

    for stem in stems_10:
        ref_path = dng50_dir / f"{stem}_50mp_from_dng.raw"
        if not ref_path.exists():
            continue
        ref_hw = np.fromfile(ref_path, dtype="<u2").reshape(6144, 8192)
        y_true = pack_bayer_planes(ref_hw)
        # Same-color 2x2 box average to 12 MP (4096x3072) matching Mission 1 12 MP readout
        x_low = F.avg_pool2d(y_true, kernel_size=2, stride=2)

        bic_pred = F.interpolate(x_low, scale_factor=2, mode="bicubic", align_corners=False).clamp(0.0, 1.0)
        old_pred = run_tiled_sr(x_low, old_sr_bb, old_sr_scale, device)
        sr_pred = run_tiled_sr(x_low, sr_bb, sr_scale, device)
        sr_unit_pred = run_tiled_sr(x_low, sr_bb, sr_scale / 0.65, device)
        x_clean = run_tiled_1x(x_low, cleanup_bb, cleanup_scale, device)
        casc_pred = run_tiled_sr(x_clean, sr_bb, sr_scale / 0.65, device)
        w96_pred = run_tiled_w96(x_low, w96_model, device)

        p_bic = psnr_db(bic_pred, y_true)
        p_old = psnr_db(old_pred, y_true)
        p_sr = psnr_db(sr_pred, y_true)
        p_sr_u = psnr_db(sr_unit_pred, y_true)
        p_casc = psnr_db(casc_pred, y_true)
        p_w96 = psnr_db(w96_pred, y_true)

        print(f"  - {stem} (50MP DNG): Bic={p_bic:.3f} | OldSR={p_old:.3f} ({p_old-p_bic:+.3f}) | "
              f"ZeroDC_SR={p_sr_u:.3f} ({p_sr_u-p_bic:+.3f} vs bic, {p_sr_u-p_old:+.3f} vs old) | "
              f"Cascade={p_casc:.3f} ({p_casc-p_bic:+.3f}) | W96={p_w96:.3f} ({p_w96-p_bic:+.3f})")
        results["mission1_dng_50mp_cases"].append({
            "stem": stem,
            "bicubic_psnr_db": p_bic,
            "old_unprojected_sr_psnr_db": p_old,
            "clean_sheet_sr_eta065_psnr_db": p_sr,
            "clean_sheet_sr_eta100_psnr_db": p_sr_u,
            "clean_sheet_cascade_psnr_db": p_casc,
            "hybrid_w96_psnr_db": p_w96,
            "sr_gain_vs_bicubic_db": p_sr_u - p_bic,
            "sr_gain_vs_old_sr_db": p_sr_u - p_old,
            "w96_gain_vs_bicubic_db": p_w96 - p_bic,
            "w96_gain_vs_old_sr_db": p_w96 - p_old,
        })

    if results["mission1_dng_50mp_cases"]:
        cases = results["mission1_dng_50mp_cases"]
        mean_bic = float(np.mean([c["bicubic_psnr_db"] for c in cases]))
        mean_old = float(np.mean([c["old_unprojected_sr_psnr_db"] for c in cases]))
        mean_sr = float(np.mean([c["clean_sheet_sr_eta100_psnr_db"] for c in cases]))
        mean_casc = float(np.mean([c["clean_sheet_cascade_psnr_db"] for c in cases]))
        mean_w96 = float(np.mean([c["hybrid_w96_psnr_db"] for c in cases]))
        pass_sr_vs_bic = sum(1 for c in cases if c["clean_sheet_sr_eta100_psnr_db"] > c["bicubic_psnr_db"])
        pass_sr_vs_old = sum(1 for c in cases if c["clean_sheet_sr_eta100_psnr_db"] > c["old_unprojected_sr_psnr_db"])
        pass_w96_vs_bic = sum(1 for c in cases if c["hybrid_w96_psnr_db"] > c["bicubic_psnr_db"])
        print("\n" + "=" * 85)
        print(f"  10-FRAME FULL 50 MP GOPRO MISSION 1 DNG SUMMARY:")
        print(f"  - Mean Bicubic Baseline:            {mean_bic:.4f} dB")
        print(f"  - Mean Old Unprojected SR (10c96f): {mean_old:.4f} dB ({mean_old - mean_bic:+.4f} dB vs bicubic)")
        print(f"  - Mean Clean-Sheet Zero-DC 2x SR:   {mean_sr:.4f} dB ({mean_sr - mean_bic:+.4f} dB vs bicubic, {mean_sr - mean_old:+.4f} dB vs old SR) [{pass_sr_vs_bic}/10 > bic, {pass_sr_vs_old}/10 > old]")
        print(f"  - Mean Clean-Sheet 1x->2x Cascade:  {mean_casc:.4f} dB ({mean_casc - mean_bic:+.4f} dB vs bicubic, {mean_casc - mean_old:+.4f} dB vs old SR)")
        print(f"  - Mean Mission1OpticalHybridW96:    {mean_w96:.4f} dB ({mean_w96 - mean_bic:+.4f} dB vs bicubic, {mean_w96 - mean_old:+.4f} dB vs old SR) [{pass_w96_vs_bic}/10 > bic]")
        print("=" * 85)
        results["mission1_dng_50mp_summary"] = {
            "count": len(cases),
            "mean_bicubic_psnr_db": mean_bic,
            "mean_old_sr_psnr_db": mean_old,
            "mean_clean_sheet_sr_psnr_db": mean_sr,
            "mean_clean_sheet_cascade_psnr_db": mean_casc,
            "mean_hybrid_w96_psnr_db": mean_w96,
            "pass_sr_vs_bicubic": f"{pass_sr_vs_bic}/{len(cases)}",
            "pass_sr_vs_old_sr": f"{pass_sr_vs_old}/{len(cases)}",
            "pass_w96_vs_bicubic": f"{pass_w96_vs_bic}/{len(cases)}",
        }

    out_path = ARTIFACT_DIR / "gopro_50mp_full_frame_verification.json"
    out_path.write_text(json.dumps(results, indent=2) + "\n")
    print(f"\nSaved full 50 MP GoPro verification receipt to {out_path}")


if __name__ == "__main__":
    main()
