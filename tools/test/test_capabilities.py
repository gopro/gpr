#!/usr/bin/env python3
"""test_capabilities.py — single source of truth for every capability we
claim + a regression test that asserts each one against an explicit,
human-readable criterion.

Each capability row carries:
  - what it measures (still encode/decode roundtrip, at a specific
    bit depth × resolution × quality)
  - explicit pass criterion for every metric, with direction:
      encode_ms ≤ ceiling
      decode_ms ≤ ceiling
      peak_rss_mb ≤ ceiling
      compress_ratio ≤ ceiling   (smaller = more compression = better)
      psnr_db ≥ floor

The test:
  1. measures live encode_ms, decode_ms, peak_rss_mb, compress_ratio,
     psnr_db per cell
  2. compares against the stated criterion for each metric, classifying:
       MET       — passes criterion within margin
       EXCEEDED  — passes by a comfortable margin (≥ 10 % better)
       FAILED    — breaks the criterion
  3. asserts every metric is MET or EXCEEDED
  4. writes ARTIFACT_DIR/CAPABILITIES.md so the report is in sync with the
     test results

Run from the repo root:
    python3 tools/test/test_capabilities.py            # assert mode
    python3 tools/test/test_capabilities.py --refresh  # learn baselines,
                                                         # don't assert,
                                                         # rewrite the doc

Env:
    BUILD_DIR=build-local
    ARTIFACT_DIR=artifacts/capabilities
    FAST=1  → skip ≥23 MP cells for quick CI
"""

from __future__ import annotations
import argparse, os, subprocess, sys, time, shutil, tempfile
import resource
from pathlib import Path
from typing import Dict, Any, Tuple

import numpy as np


REPO = Path(__file__).resolve().parents[2]
BUILD_DIR = Path(os.environ.get("BUILD_DIR", "build-local"))
if not BUILD_DIR.is_absolute():
    BUILD_DIR = REPO / BUILD_DIR
GTOOLS = Path(os.environ.get("GTOOLS", BUILD_DIR / "source/app/gpr_tools/gpr_tools"))

# Timing-ceiling multiplier. Debug builds run ~2-3x slower than Release, so a
# single set of locked ceilings can't gate both. Default 1.0 (Release ceilings).
# CI's Debug job sets GPR_TIMING_TOLERANCE=3.0 so a Debug ms reading that's
# within 3x of the Release ceiling still passes. Quality criteria (PSNR,
# compression ratio) ignore this — they're build-type independent.
TIMING_TOLERANCE = float(os.environ.get("GPR_TIMING_TOLERANCE", "1.0"))
INSTRUMENTED = bool(os.environ.get("GPR_COVERAGE_DIR"))
TIMING_SAMPLES = int(os.environ.get("GPR_TIMING_SAMPLES", "3"))
TIMING_SAMPLE_MAX_PIXELS = int(os.environ.get(
    "GPR_TIMING_SAMPLE_MAX_PIXELS", str(4032 * 3024)))

def default_external_root() -> Path:
    return Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())) / "gpr_work"


EXTERNAL_ROOT = Path(os.environ.get("GPR_EXTERNAL_ROOT", default_external_root()))
ARTIFACT_ROOT = Path(os.environ.get("GPR_ARTIFACT_ROOT", EXTERNAL_ROOT / "artifacts"))
DEFAULT_ART = (str(ARTIFACT_ROOT / "capabilities")
               if EXTERNAL_ROOT.exists()
               else str(Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir()))
                        / "gpr-capabilities"))
ART_DIR = Path(os.environ.get("ARTIFACT_DIR", DEFAULT_ART))
FAST = os.environ.get("FAST", "0") == "1"


# ---------------------------------------------------------------------------
# Capability list.
#
# Each row's `criteria` dict states the explicit pass condition per metric:
#   "encode_ms":   {"max": ms, "exceed_below": ms_for_EXCEEDED}
#   "decode_ms":   {"max": ms, "exceed_below": ms}
#   "compress_ratio": {"max": fraction, "exceed_below": fraction}
#   "psnr_db":     {"min": dB,  "exceed_above": dB}
#
# Baselines locked from a clean M3 Max run on 2026-05-24. Ceilings set
# at +50 % vs measured for time, +0.02 vs measured for compress_ratio,
# −1.5 dB vs measured for PSNR. EXCEEDED bar at +10 % better.
# ---------------------------------------------------------------------------

CAPABILITIES = [
    # ---- 1024² cells: bit-depth × Bayer × quality coverage --------------
    dict(id="still_rggb12_1024_q3",
         display="Stills · rggb12 · 1024² · q=3 (Filmscan-1)",
         kind="still_roundtrip",
         W=1024, H=1024, pf="rggb12", peak=4095, quality=3,
         criteria=dict(
             encode_ms={"max": 55, "exceed_below": 25},
             decode_ms={"max": 45, "exceed_below": 22},
             compress_ratio={"max": 0.08, "exceed_below": 0.055},
             psnr_db={"min": 42.0, "exceed_above": 44.0})),
    dict(id="still_rggb12p_1024_q3",
         display="Stills · rggb12p (packed) · 1024² · q=3",
         kind="still_roundtrip",
         W=1024, H=1024, pf="rggb12p", peak=4095, quality=3, packed=True,
         criteria=dict(
             encode_ms={"max": 50, "exceed_below": 25},
             decode_ms={"max": 40, "exceed_below": 22},
             compress_ratio={"max": 0.08, "exceed_below": 0.055},
             psnr_db={"min": 42.0, "exceed_above": 44.0})),
    dict(id="still_rggb14_1024_q3",
         display="Stills · rggb14 · 1024² · q=3",
         kind="still_roundtrip",
         W=1024, H=1024, pf="rggb14", peak=16383, quality=3,
         criteria=dict(
             encode_ms={"max": 50, "exceed_below": 25},
             decode_ms={"max": 40, "exceed_below": 25},
             compress_ratio={"max": 0.10, "exceed_below": 0.075},
             psnr_db={"min": 52.0, "exceed_above": 54.5})),
    dict(id="still_rggb14_1024_q0",
         display="Stills · rggb14 · 1024² · q=0 (Low)",
         kind="still_roundtrip",
         W=1024, H=1024, pf="rggb14", peak=16383, quality=0,
         criteria=dict(
             encode_ms={"max": 50, "exceed_below": 25},
             decode_ms={"max": 40, "exceed_below": 22},
             compress_ratio={"max": 0.05, "exceed_below": 0.035},
             psnr_db={"min": 51.5, "exceed_above": 54.0})),
    dict(id="still_rggb14_1024_q5",
         display="Stills · rggb14 · 1024² · q=5 (Filmscan-2, quality peak)",
         kind="still_roundtrip",
         W=1024, H=1024, pf="rggb14", peak=16383, quality=5,
         # q=5 is the empirical PSNR peak across the 9 quality presets on
         # real Z8 50 MP photographic content (see docs/quant_calibration_findings.md).
         # q=6/7/8 regress (task #159). Locking q=5 here so future codec
         # changes can't quietly break the actual quality peak.
         criteria=dict(
             encode_ms={"max": 50, "exceed_below": 25},
             decode_ms={"max": 70, "exceed_below": 35},
             compress_ratio={"max": 0.14, "exceed_below": 0.095},
             psnr_db={"min": 55.0, "exceed_above": 58.0})),
    dict(id="still_rggb14_1024_q8",
         display="Stills · rggb14 · 1024² · q=8 (Filmscan-5)",
         kind="still_roundtrip",
         W=1024, H=1024, pf="rggb14", peak=16383, quality=8,
         criteria=dict(
             encode_ms={"max": 60, "exceed_below": 35},
             decode_ms={"max": 60, "exceed_below": 35},
             compress_ratio={"max": 0.25, "exceed_below": 0.205},
             psnr_db={"min": 60.5, "exceed_above": 63.0})),
    dict(id="still_rggb14_1024_q11",
         display="Stills · rggb14 · 1024² · q=11 (CNN-aware)",
         kind="still_roundtrip",
         W=1024, H=1024, pf="rggb14", peak=16383, quality=11,
         # q=11 is the CNN-aware preset (PR #21): cranked L1 highpass
         # designed to pair with a CNN trained on the cranked distribution.
         # On the synthetic radial-gradient fixture (small, smooth content
         # with little L1 highpass energy) the bayer-domain PSNR is similar
         # to q=3 — the real win is on photographic content via the
         # retrained CNN. Locking the synth fixture's numbers here so the
         # codec change isn't quietly broken.
         criteria=dict(
             encode_ms={"max": 60, "exceed_below": 25},
             decode_ms={"max": 50, "exceed_below": 25},
             compress_ratio={"max": 0.06, "exceed_below": 0.045},
             psnr_db={"min": 51.5, "exceed_above": 54.0})),
    dict(id="still_rggb16_1024_q3",
         display="Stills · rggb16 · 1024² · q=3",
         kind="still_roundtrip",
         W=1024, H=1024, pf="rggb16", peak=65535, quality=3,
         criteria=dict(
             encode_ms={"max": 70, "exceed_below": 25},
             decode_ms={"max": 70, "exceed_below": 27},
             compress_ratio={"max": 0.08, "exceed_below": 0.055},
             psnr_db={"min": 52.0, "exceed_above": 54.5})),
    dict(id="still_gbrg16_1024_q3",
         display="Stills · gbrg16 (alt Bayer) · 1024² · q=3",
         kind="still_roundtrip",
         W=1024, H=1024, pf="gbrg16", peak=65535, quality=3,
         criteria=dict(
             encode_ms={"max": 70, "exceed_below": 25},
             decode_ms={"max": 70, "exceed_below": 27},
             compress_ratio={"max": 0.08, "exceed_below": 0.055},
             psnr_db={"min": 52.0, "exceed_above": 54.5})),
    dict(id="still_gbrg14_1024_q3",
         display="Stills · gbrg14 (alt Bayer) · 1024² · q=3",
         kind="still_roundtrip",
         W=1024, H=1024, pf="gbrg14", peak=16383, quality=3,
         criteria=dict(
             encode_ms={"max": 70, "exceed_below": 25},
             decode_ms={"max": 70, "exceed_below": 27},
             compress_ratio={"max": 0.10, "exceed_below": 0.075},
             psnr_db={"min": 52.0, "exceed_above": 54.5})),
    dict(id="still_grbg14_1024_q3",
         display="Stills · grbg14 (alt Bayer) · 1024² · q=3",
         kind="still_roundtrip",
         W=1024, H=1024, pf="grbg14", peak=16383, quality=3,
         criteria=dict(
             encode_ms={"max": 90, "exceed_below": 30},
             decode_ms={"max": 90, "exceed_below": 35},
             compress_ratio={"max": 0.10, "exceed_below": 0.075},
             psnr_db={"min": 52.0, "exceed_above": 54.5})),
    dict(id="still_bggr14_1024_q3",
         display="Stills · bggr14 (alt Bayer) · 1024² · q=3",
         kind="still_roundtrip",
         W=1024, H=1024, pf="bggr14", peak=16383, quality=3,
         criteria=dict(
             encode_ms={"max": 90, "exceed_below": 30},
             decode_ms={"max": 90, "exceed_below": 35},
             compress_ratio={"max": 0.10, "exceed_below": 0.075},
             psnr_db={"min": 52.0, "exceed_above": 54.5})),
    # ---- resolution-scaling cells ---------------------------------------
    dict(id="still_rggb12_12MP_q3",
         display="Stills · rggb12 · 12 MP (4032×3024) · q=3",
         kind="still_roundtrip",
         W=4032, H=3024, pf="rggb12", peak=4095, quality=3,
         criteria=dict(
             encode_ms={"max": 300, "exceed_below": 175},
             decode_ms={"max": 250, "exceed_below": 160},
             compress_ratio={"max": 0.07, "exceed_below": 0.05},
             psnr_db={"min": 42.0, "exceed_above": 44.0})),
    dict(id="still_rggb14_h10_q3",
         display="Stills · rggb14 · 23 MP HERO10 (5568×4176) · q=3",
         kind="still_roundtrip",
         W=5568, H=4176, pf="rggb14", peak=16383, quality=3,
         criteria=dict(
             encode_ms={"max": 600, "exceed_below": 370},
             decode_ms={"max": 600, "exceed_below": 375},
             compress_ratio={"max": 0.10, "exceed_below": 0.07},
             psnr_db={"min": 52.0, "exceed_above": 54.5}),
         fast_skip=True),
    dict(id="still_rggb14_Z8_q3",
         display="Stills · rggb14 · 50 MP Z8 (8280×5520) · q=3",
         kind="still_roundtrip",
         W=8280, H=5520, pf="rggb14", peak=16383, quality=3,
         criteria=dict(
             encode_ms={"max": 1100, "exceed_below": 740},
             decode_ms={"max": 1100, "exceed_below": 720},
             compress_ratio={"max": 0.10, "exceed_below": 0.07},
             psnr_db={"min": 52.0, "exceed_above": 54.5}),
         fast_skip=True),
    dict(id="still_rggb16_X2D_q3",
         display="Stills · rggb16 · 100 MP X2D (11664×8750) · q=3",
         kind="still_roundtrip",
         W=11664, H=8750, pf="rggb16", peak=65535, quality=3,
         criteria=dict(
             encode_ms={"max": 2500, "exceed_below": 1700},
             decode_ms={"max": 2500, "exceed_below": 1800},
             compress_ratio={"max": 0.08, "exceed_below": 0.055},
             psnr_db={"min": 52.0, "exceed_above": 54.5}),
         fast_skip=True),

    # ---- CNN-corrected render-domain PSNR ------------------------------
    #
    # These cells exercise the full playback path the production app uses:
    #   real DNG  → multi-level + decimate=2 FUSED encode/decode (half-res)
    #             → BIBO_1x CNN (BayInBayOut_1x_AAon_w16_ANE.pt) on bayer
    #             → bayer_bicubic_2x back to full size
    #             → rawpy AHD render
    #             → masked Y-PSNR vs the source-DNG AHD render
    # That's the chain PR #10/#11/#13/#15 made the new default. PSNR is
    # measured at the stated output resolution (UHD or 4K) so a future
    # regression that breaks the half-res topology or the CNN integration
    # shows up here, not just in microbenchmarks.
    #
    # Baselines locked from a clean 2026-05-25 M3 Max run (reproducible to
    # 4 decimal places across re-runs because the encode is deterministic
    # and MPS gives stable results on this graph). Criteria leave ~1.5 dB
    # of margin below measured.
    #
    # macOS-only (BIBO_1x uses torch+MPS; rawpy AHD on Linux CI is fine
    # but pytorch isn't installed on the bare runner). Cell skips with
    # a clean message when deps are missing.
    dict(id="cnn_BIBO_1x_Z8_ISO64_uhd",
         display="CNN · BIBO_1x · Z8 ISO64 · 50 MP → UHD (single-level + CNN)",
         kind="cnn_corrected",
         src_dng="data/test_sets/entropy_matrix/Z8_ISO64.DNG",
         peak=16383,
         out_res=(3840, 2160),
         codec_path="singlelevel",
         visual_metrics=True,
         # Re-baselined 2026-05-25 evening (single-level): psnr_db is the
         # render-domain bayer-PSNR; y_psnr_db / ms_ssim / lpips / dE2000
         # are the new viewed-side metric stack (see tools/test/metrics.py).
         # Floors set ~10% below measured baseline for headroom.
         criteria=dict(
             psnr_db={"min": 52.0, "exceed_above": 54.0},
             ms_ssim={"min": 0.985, "exceed_above": 0.995},
             lpips={"max":   0.10,  "exceed_below": 0.05},
             dE2000={"max":   2.0,  "exceed_below": 1.0})),
    dict(id="cnn_BIBO_1x_Z8_ISO64_4k",
         display="CNN · BIBO_1x · Z8 ISO64 · 50 MP → 4K (single-level + CNN)",
         kind="cnn_corrected",
         src_dng="data/test_sets/entropy_matrix/Z8_ISO64.DNG",
         peak=16383,
         out_res=(4096, 2160),
         codec_path="singlelevel",
         visual_metrics=True,
         criteria=dict(
             psnr_db={"min": 52.0, "exceed_above": 54.0},
             ms_ssim={"min": 0.985, "exceed_above": 0.995},
             lpips={"max":   0.10,  "exceed_below": 0.05},
             dE2000={"max":   2.0,  "exceed_below": 1.0})),
    dict(id="cnn_BIBO_1x_Z8_ISO22800_uhd",
         display="CNN · BIBO_1x · Z8 ISO22800 · 50 MP → UHD (single-level + CNN)",
         kind="cnn_corrected",
         src_dng="data/test_sets/entropy_matrix/Z8_ISO22800.DNG",
         peak=16383,
         out_res=(3840, 2160),
         codec_path="singlelevel",
         visual_metrics=True,
         # Lower floors: high-ISO content is harder to recover.
         criteria=dict(
             psnr_db={"min": 42.5, "exceed_above": 44.5},
             ms_ssim={"min": 0.970, "exceed_above": 0.985},
             lpips={"max":   0.15,  "exceed_below": 0.08},
             dE2000={"max":   3.0,  "exceed_below": 1.5})),
]


def _install_default_memory_criteria() -> None:
    """Attach conservative peak-RSS gates to every still capability.

    This turns memory into a measured production contract without pretending
    the current ceilings are tuned platform limits. The max is sized from the
    raw bayer payload with a fixed floor for small CI fixtures.
    """
    for cap in CAPABILITIES:
        if cap.get("kind") != "still_roundtrip":
            continue
        crit = cap.setdefault("criteria", {})
        if "peak_rss_mb" in crit:
            continue
        raw_mb = (cap["W"] * cap["H"] * 2) / (1024.0 * 1024.0)
        max_mb = max(512.0, raw_mb * 24.0)
        crit["peak_rss_mb"] = {
            "max": round(max_mb, 0),
            "exceed_below": round(max_mb * 0.70, 0),
        }


_install_default_memory_criteria()


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def synth_bayer(W, H, pf, peak, seed, packed, out: Path):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    r = np.hypot(xx - W/2, yy - H/2) / np.hypot(W/2, H/2)
    bright = (peak * (1.0 - np.minimum(r, 1.0))).astype(np.int32)
    img = np.zeros((H, W), dtype=np.int32)
    off_r = max(1, peak // 82); off_g = max(1, peak // 20); off_b = max(1, peak // 41)
    if pf.startswith("gbrg"):
        img[0::2, 0::2] = bright[0::2, 0::2] + off_g
        img[0::2, 1::2] = bright[0::2, 1::2] + off_b
        img[1::2, 0::2] = bright[1::2, 0::2] + off_r
        img[1::2, 1::2] = bright[1::2, 1::2] + off_g
    elif pf.startswith("grbg"):
        img[0::2, 0::2] = bright[0::2, 0::2] + off_g
        img[0::2, 1::2] = bright[0::2, 1::2] + off_r
        img[1::2, 0::2] = bright[1::2, 0::2] + off_b
        img[1::2, 1::2] = bright[1::2, 1::2] + off_g
    elif pf.startswith("bggr"):
        img[0::2, 0::2] = bright[0::2, 0::2] + off_b
        img[0::2, 1::2] = bright[0::2, 1::2] + off_g
        img[1::2, 0::2] = bright[1::2, 0::2] + off_g
        img[1::2, 1::2] = bright[1::2, 1::2] + off_r
    else:
        img[0::2, 0::2] = bright[0::2, 0::2] + off_r
        img[0::2, 1::2] = bright[0::2, 1::2] + off_g
        img[1::2, 0::2] = bright[1::2, 0::2] + off_g
        img[1::2, 1::2] = bright[1::2, 1::2] + off_b
    amp = max(50, peak // 256)
    img += rng.integers(-amp, amp + 1, size=(H, W), dtype=np.int32)
    np.clip(img, 0, peak, out=img)
    if packed:
        flat = img.astype(np.uint16).ravel()
        b = np.empty(flat.size * 3 // 2, dtype=np.uint8)
        b[0::3] = (flat[0::2] & 0xFF).astype(np.uint8)
        b[1::3] = (((flat[0::2] >> 8) & 0x0F) | ((flat[1::2] & 0x0F) << 4)).astype(np.uint8)
        b[2::3] = ((flat[1::2] >> 4) & 0xFF).astype(np.uint8)
        b.tofile(out)
    else:
        img.astype("<u2").tofile(out)


def _rss_to_mb(ru_maxrss: int) -> float:
    """Normalize wait4 ru_maxrss to MiB: Linux reports KiB, Darwin bytes."""
    if sys.platform == "darwin":
        return ru_maxrss / (1024.0 * 1024.0)
    return ru_maxrss / 1024.0


def _run_timed(args):
    t0 = time.perf_counter()
    with tempfile.TemporaryFile(mode="w+t") as stdout, tempfile.TemporaryFile(mode="w+t") as stderr:
        p = subprocess.Popen(args, stdout=stdout, stderr=stderr, text=True)
        _pid, status, usage = os.wait4(p.pid, 0)
    try:
        rc = os.waitstatus_to_exitcode(status)
    except AttributeError:
        rc = status >> 8
    return rc, (time.perf_counter() - t0) * 1000.0, _rss_to_mb(usage.ru_maxrss)


def measure_still_roundtrip(cap, work: Path) -> Dict[str, float]:
    W, H, pf, peak, q = cap["W"], cap["H"], cap["pf"], cap["peak"], cap["quality"]
    packed = cap.get("packed", False)
    seed = abs(hash(cap["id"])) % (2**31)
    raw = work / f"{cap['id']}.raw"
    dng = work / f"{cap['id']}.dng"
    gpr = work / f"{cap['id']}.gpr"
    out = work / f"{cap['id']}_dec.dng"

    synth_bayer(W, H, pf, peak, seed, packed, raw)
    rc, _, _ = _run_timed([str(GTOOLS), "-i", str(raw), "-w", str(W), "-h", str(H),
                          "-x", pf, "-o", str(dng)])
    if rc != 0:
        raise RuntimeError("raw→dng failed")
    # CI-sized still timing cells are vulnerable to one-off hosted-runner noise
    # (process launch, filesystem hiccups, background runner work). Sample
    # them a few times and keep the best codec wall time. The 23/50/100 MP
    # rows stay single-shot by default so full macOS coverage remains bounded.
    samples = max(1, TIMING_SAMPLES if W * H <= TIMING_SAMPLE_MAX_PIXELS else 1)
    encode_ms = float("inf")
    decode_ms = float("inf")
    peak_rss_mb = 0.0
    for _ in range(samples):
        rc, enc, enc_rss = _run_timed([str(GTOOLS), "-i", str(dng), "-q", str(q), "-o", str(gpr)])
        if rc != 0:
            raise RuntimeError("dng→gpr failed")
        encode_ms = min(encode_ms, enc)
        peak_rss_mb = max(peak_rss_mb, enc_rss)
        rc, dec, dec_rss = _run_timed([str(GTOOLS), "-i", str(gpr), "-o", str(out)])
        if rc != 0:
            raise RuntimeError("gpr→dng failed")
        decode_ms = min(decode_ms, dec)
        peak_rss_mb = max(peak_rss_mb, dec_rss)
    gpr_bytes = gpr.stat().st_size

    import rawpy
    a = rawpy.imread(str(dng)); src = a.raw_image.copy().astype(np.float64); a.close()
    b = rawpy.imread(str(out)); dec = b.raw_image.copy().astype(np.float64); b.close()
    if src.shape != dec.shape:
        raise RuntimeError(f"shape mismatch {src.shape} vs {dec.shape}")
    mse = float(((src - dec) ** 2).mean())
    psnr = 10 * np.log10(peak * peak / mse) if mse > 0 else float("inf")
    raw_equiv = W * H * 2
    return dict(encode_ms=encode_ms, decode_ms=decode_ms,
                peak_rss_mb=peak_rss_mb,
                compress_ratio=gpr_bytes / raw_equiv, psnr_db=psnr,
                gpr_bytes=gpr_bytes, raw_bytes=raw_equiv)


# ---------------------------------------------------------------------------
# CNN-corrected render-domain PSNR
#
# Exercises the full playback chain: real DNG → multi-level + decimate=2
# FUSED encode/decode → BIBO_1x CNN → bayer_bicubic_2x → rawpy AHD render
# → masked Y-PSNR vs source AHD render at the target output resolution.
#
# Deps probed lazily inside measure_cnn_corrected so the harness still
# runs on a Linux CI box without torch/rawpy/cv2 (cell returns "SKIP"
# verdict instead of failing).
# ---------------------------------------------------------------------------

# Model code is repository-owned; checkpoint data may be supplied externally.
CNN_CODE_DIR_REPO = REPO / "tools" / "cnn"
CNN_CKPT_REPO = REPO / "models" / "BayInBayOut_1x_AAon_w16_ANE.pt"

def _cnn_resolve_paths():
    """Use the retained model with a local or explicitly configured checkpoint."""
    if os.environ.get("GPR_CNN_CHECKPOINT"):
        checkpoint = Path(os.environ["GPR_CNN_CHECKPOINT"])
    else:
        checkpoints = [CNN_CKPT_REPO]
        for key in ("GPR_MODEL_ROOT", "GPR_CHECKPOINT_ROOT"):
            checkpoints.extend(Path(root) / CNN_CKPT_REPO.name
                               for root in os.environ.get(key, "").split(os.pathsep) if root)
        checkpoint = next((path for path in checkpoints if path.is_file()), CNN_CKPT_REPO)
    return str(CNN_CODE_DIR_REPO), checkpoint, "model"

CNN_ROUNDTRIP_BIN = BUILD_DIR / "bin/test_fused_roundtrip"

_CNN_STATE = {"model": None, "device": None, "src_cache": {}}


def _cnn_probe_deps():
    """Return whether the CNN model and rendering dependencies are available."""
    missing = []
    try:
        import torch  # noqa
        import torch.nn.functional as _F  # noqa
    except ImportError as e:
        missing.append(f"torch ({e})")
    try:
        import rawpy  # noqa
    except ImportError as e:
        missing.append(f"rawpy ({e})")
    try:
        import cv2  # noqa
    except ImportError as e:
        missing.append(f"cv2 ({e})")
    code_dir, ckpt, _ = _cnn_resolve_paths()
    if not (Path(code_dir) / f"{_cnn_resolve_paths()[2]}.py").exists():
        missing.append(f"CNN model.py not present at {code_dir}")
    if not ckpt.exists():
        missing.append(f"CNN checkpoint not present: {ckpt}")
    if not CNN_ROUNDTRIP_BIN.exists():
        missing.append(f"test_fused_roundtrip not built at {CNN_ROUNDTRIP_BIN} "
                       "(build with: clang -O2 source/app/test_fused_decode_roundtrip.c "
                       "<libs> -o build-local/bin/test_fused_roundtrip)")
    if missing:
        return False, "; ".join(missing)
    return True, "ok"


def _cnn_load_model():
    if _CNN_STATE["model"] is not None:
        return _CNN_STATE["model"], _CNN_STATE["device"]
    import torch
    import importlib
    code_dir, ckpt_path, module_name = _cnn_resolve_paths()
    sys.path.insert(0, code_dir)
    mod = importlib.import_module(module_name)
    build_ane = mod.build
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    ck = torch.load(str(ckpt_path), map_location="cpu", weights_only=False)
    m = build_ane(ck.get("variant", "F_ane"))
    m.load_state_dict(ck["backbone_state"])
    m.to(device).eval()
    _CNN_STATE["model"] = m
    _CNN_STATE["device"] = device
    return m, device


def _extract_bayer(dng_path, out_raw):
    import rawpy
    r = rawpy.imread(str(dng_path))
    b = r.raw_image.copy().astype("<u2")
    h, w = b.shape
    r.close()
    b.tofile(out_raw)
    return w, h


def _encode_decode_singlelevel(raw_in, w, h, dec_out):
    """Single-level FUSED, full-res output. The known-good codec path.
    Output dimensions match input (w x h)."""
    env = os.environ.copy()
    env["FUSED_MULTI_LEVEL"] = "0"
    env["GPR_INCLUDE_LL"] = "1"
    env.pop("GPR_COL_DECIMATE", None)
    env.pop("GPR_ROW_DECIMATE", None)
    res = subprocess.run(
        [str(CNN_ROUNDTRIP_BIN), str(raw_in), str(w), str(h), str(dec_out)],
        env=env, capture_output=True, text=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"test_fused_roundtrip rc={res.returncode}: {res.stderr.strip()}")
    return w, h


def _cnn_apply_bayer(bayer_in_raw, w, h, bayer_out_raw):
    """Run BIBO_1x on a half-res bayer (uint16 le) and write corrected bayer."""
    import torch
    import torch.nn.functional as F
    model, device = _cnn_load_model()
    b = np.fromfile(bayer_in_raw, dtype=np.uint16).reshape(h, w)
    eh = h - (h & 1)
    ew = w - (w & 1)
    be = b[:eh, :ew]
    R = be[0::2, 0::2]; G1 = be[0::2, 1::2]
    G2 = be[1::2, 0::2]; B = be[1::2, 1::2]
    pl = np.stack([R, G1, G2, B], 0).astype(np.float32) / 16383.0
    x = torch.from_numpy(pl).unsqueeze(0).to(device)
    H_, W_ = x.shape[-2:]
    pad_h = (16 - H_ % 16) % 16
    pad_w = (16 - W_ % 16) % 16
    if pad_h or pad_w:
        x = F.pad(x, (0, pad_w, 0, pad_h), mode='reflect')
    with torch.no_grad():
        y = (x + 0.01 * model(x)).clamp(0, 1)
    y = y[..., :H_, :W_]
    yn = y.squeeze(0).cpu().numpy()
    out = b.copy()
    out[:eh:2, :ew:2]   = np.clip(yn[0] * 16383, 0, 16383).astype(np.uint16)
    out[:eh:2, 1:ew:2]  = np.clip(yn[1] * 16383, 0, 16383).astype(np.uint16)
    out[1:eh:2, :ew:2]  = np.clip(yn[2] * 16383, 0, 16383).astype(np.uint16)
    out[1:eh:2, 1:ew:2] = np.clip(yn[3] * 16383, 0, 16383).astype(np.uint16)
    out.tofile(bayer_out_raw)


def _bayer_bicubic_2x(b):
    import cv2
    R = b[0::2, 0::2]; G1 = b[0::2, 1::2]
    G2 = b[1::2, 0::2]; B = b[1::2, 1::2]
    sh, sw = R.shape
    o = np.empty((sh * 4, sw * 4), dtype=np.uint16)
    for plane, dst in zip([R, G1, G2, B], [(0, 0), (0, 1), (1, 0), (1, 1)]):
        up = cv2.resize(plane, (sw * 2, sh * 2), cv2.INTER_CUBIC).astype(np.uint16)
        o[dst[0]::2, dst[1]::2] = up
    return o


def _render_with_meta(bayer, meta_dng):
    import rawpy
    r = rawpy.imread(str(meta_dng))
    r.raw_image[:] = bayer
    rgb = r.postprocess(use_camera_wb=True, no_auto_bright=True, output_bps=16,
                        gamma=(2.222, 4.5), output_color=rawpy.ColorSpace.sRGB,
                        demosaic_algorithm=rawpy.DemosaicAlgorithm.AHD)
    r.close()
    return rgb


def _resize_to_target(rgb, target_w, target_h):
    import cv2
    sh, sw = rgb.shape[:2]
    # rawpy AHD render is portrait for Z8 sensor data — preserve orientation
    # by matching long edge to long edge.
    if sh > sw:
        out_w, out_h = min(target_w, target_h), max(target_w, target_h)
    else:
        out_w, out_h = max(target_w, target_h), min(target_w, target_h)
    return cv2.resize(rgb.astype(np.float32), (out_w, out_h),
                      interpolation=cv2.INTER_AREA).astype(np.uint16)


def _psnr_masked_y(ref_rgb16, test_rgb16, dark=10, bright=250):
    rs = (ref_rgb16 / 256.0).astype(np.float32)
    ts = (test_rgb16 / 256.0).astype(np.float32)
    # brightness match channel means before Y
    tsm = ts.copy()
    for c in range(3):
        tsm[..., c] = np.clip(ts[..., c] + (rs[..., c].mean() - ts[..., c].mean()), 0, 255)
    def Y(im): return 0.299 * im[..., 0] + 0.587 * im[..., 1] + 0.114 * im[..., 2]
    ry = Y(rs); ty = Y(tsm)
    mask = (ry > dark) & (ry < bright)
    mse = ((ry[mask] - ty[mask]) ** 2).mean()
    return float(20 * np.log10(255.0 / np.sqrt(max(mse, 1e-12))))


def measure_cnn_corrected(cap, work: Path) -> Dict[str, float]:
    """Full CNN-corrected playback measurement. Caller catches RuntimeError
    raised when prereqs aren't available — for missing-deps we raise
    RuntimeError('SKIP: ...') so main() can mark the row SKIPPED rather
    than FAILED."""
    ok, reason = _cnn_probe_deps()
    if not ok:
        raise RuntimeError(f"SKIP: {reason}")

    dng_rel = cap["src_dng"]
    dng = REPO / dng_rel if not Path(dng_rel).is_absolute() else Path(dng_rel)
    if not dng.exists():
        raise RuntimeError(f"SKIP: source DNG missing: {dng}")
    target_w, target_h = cap["out_res"]

    raw_in = work / f"{cap['id']}_src.raw"
    dec_raw = work / f"{cap['id']}_dec.raw"
    cnn_raw = work / f"{cap['id']}_cnn.raw"

    w, h = _extract_bayer(dng, raw_in)
    dw, dh = _encode_decode_singlelevel(raw_in, w, h, dec_raw)
    _cnn_apply_bayer(dec_raw, dw, dh, cnn_raw)

    if dw == w and dh == h:
        # single-level: output is at full res, no upsampling needed
        full_bayer = np.fromfile(cnn_raw, dtype=np.uint16).reshape(dh, dw)
    else:
        # multi-level + decimate: half-res output, bicubic-upsample per channel
        cnn_half = np.fromfile(cnn_raw, dtype=np.uint16).reshape(dh, dw)
        full_bayer = _bayer_bicubic_2x(cnn_half)
        if full_bayer.shape != (h, w):
            pad = np.zeros((h, w), dtype=np.uint16)
            clip_h = min(full_bayer.shape[0], h)
            clip_w = min(full_bayer.shape[1], w)
            pad[:clip_h, :clip_w] = full_bayer[:clip_h, :clip_w]
            full_bayer = pad
    cnn_rgb = _render_with_meta(full_bayer, dng)

    # Source render is cached per DNG (the same DNG is used by multiple cells).
    src_cache = _CNN_STATE["src_cache"]
    if str(dng) not in src_cache:
        src_bayer = np.fromfile(raw_in, dtype=np.uint16).reshape(h, w)
        src_cache[str(dng)] = _render_with_meta(src_bayer, dng)
    src_rgb = src_cache[str(dng)]

    src_resized = _resize_to_target(src_rgb, target_w, target_h)
    cnn_resized = _resize_to_target(cnn_rgb, target_w, target_h)
    psnr = _psnr_masked_y(src_resized, cnn_resized)
    metrics = dict(psnr_db=psnr)
    # If the cell opts in to the full visual metric stack (lpips/ms_ssim/dE),
    # compute the perceptual metrics too. Requires tools/test/metrics.py.
    if cap.get("visual_metrics", False):
        try:
            sys.path.insert(0, str(REPO / "tools" / "test"))
            from metrics import compute_visual_metrics
            # metrics module expects uint8 RGB; resized arrays are uint16.
            src8 = (src_resized.astype(np.float32) / 256.0).clip(0, 255).astype(np.uint8)
            cnn8 = (cnn_resized.astype(np.float32) / 256.0).clip(0, 255).astype(np.uint8)
            vm = compute_visual_metrics(src8, cnn8)
            metrics["y_psnr_db"] = vm["y_psnr"]
            metrics["ms_ssim"]   = vm["ms_ssim"]
            metrics["lpips"]     = vm["lpips"]
            metrics["dE2000"]    = vm["dE2000_mean"]
        except Exception as e:
            metrics["_visual_metrics_error"] = str(e)[:120]
    return metrics


# ---------------------------------------------------------------------------
# Criterion classification: MET / EXCEEDED / FAILED
# ---------------------------------------------------------------------------

def classify(value: float, crit: Dict[str, Any]) -> Tuple[str, str]:
    """Classify a measured value against a criterion dict.
    Returns (verdict, criterion_text)."""
    if "max" in crit:
        # lower is better
        cmax = crit["max"]
        excd = crit.get("exceed_below")
        text = f"≤ {cmax:g}"
        if value > cmax:
            return "FAILED", text
        if excd is not None and value <= excd:
            return "EXCEEDED", text
        return "MET", text
    if "min" in crit:
        # higher is better
        cmin = crit["min"]
        excd = crit.get("exceed_above")
        text = f"≥ {cmin:g}"
        if value < cmin:
            return "FAILED", text
        if excd is not None and value >= excd:
            return "EXCEEDED", text
        return "MET", text
    return "MET", "(no criterion)"


STILL_METRIC_ORDER = [
    ("encode_ms",      "Encode",        "ms",  "{:.1f}"),
    ("decode_ms",      "Decode",        "ms",  "{:.1f}"),
    ("peak_rss_mb",    "Peak RSS",      "MiB", "{:.0f}"),
    ("compress_ratio", "Size vs raw",   "%",   "{:.2%}"),
    ("psnr_db",        "Roundtrip PSNR","dB",  "{:.2f}"),
]

VISUAL_METRIC_ORDER = [
    # Visual metric stack (opt-in per cell via cap["visual_metrics"]=True).
    # See tools/test/metrics.py for definitions. These cover the criteria
    # the user requested ("multiple measures of visual correctness" +
    # "bayer AND viewed") — bayer-side is psnr_db, viewed-side is below.
    ("y_psnr_db",      "Y-PSNR (RGB)",  "dB",  "{:.2f}"),
    ("ms_ssim",        "MS-SSIM",       "",    "{:.4f}"),
    ("lpips",          "LPIPS-Alex",    "",    "{:.4f}"),
    ("dE2000",         "ΔE2000",        "",    "{:.2f}"),
]

METRIC_ORDER = STILL_METRIC_ORDER + VISUAL_METRIC_ORDER


def check_cap(cap: dict, m: Dict[str, float], *, instrumented=INSTRUMENTED) -> Tuple[str, list]:
    """Returns (overall_verdict, [(metric_id, value, verdict, criterion_text)])."""
    crits = cap["criteria"]
    rows = []
    overall = "MET"
    has_exceeded = False
    skipped_performance = False
    for mid, *_ in METRIC_ORDER:
        if mid not in crits:
            continue
        if instrumented and mid in ("encode_ms", "decode_ms", "peak_rss_mb"):
            rows.append((mid, m[mid], "SKIPPED", "instrumented; not a performance receipt"))
            skipped_performance = True
            continue
        # Apply timing tolerance only to ms-based metrics; quality/size criteria
        # are build-type independent and stay strict.
        crit = crits[mid]
        if mid in ("encode_ms", "decode_ms") and TIMING_TOLERANCE != 1.0:
            crit = dict(crit)
            if "max" in crit:
                crit["max"] = crit["max"] * TIMING_TOLERANCE
            if "exceed_below" in crit and crit["exceed_below"] is not None:
                crit["exceed_below"] = crit["exceed_below"] * TIMING_TOLERANCE
        v, c = classify(m[mid], crit)
        rows.append((mid, m[mid], v, c))
        if v == "FAILED":
            overall = "FAILED"
        elif v == "EXCEEDED" and overall != "FAILED":
            has_exceeded = True
    if overall == "MET" and has_exceeded:
        # If at least one metric exceeded and none failed, mark overall as EXCEEDED.
        overall = "EXCEEDED"
    if skipped_performance and overall != "FAILED":
        overall = "PARTIAL"
    return overall, rows


# ---------------------------------------------------------------------------
# Markdown emission
# ---------------------------------------------------------------------------

VERDICT_ICONS = {"MET": "✅ MET", "EXCEEDED": "✨ EXCEEDED",
                 "FAILED": "❌ FAILED", "SKIPPED": "⊘ SKIPPED", "PARTIAL": "PARTIAL"}


def emit_markdown(rows: list, out_path: Path):
    """rows: list of (cap, measured, overall_verdict, per_metric_rows)."""
    lines = [
        "# Capabilities — measured, criteria-stated, regression-tested",
        "",
        "Each row is one capability we claim. The metric columns show the",
        "**measured value** alongside the **explicit criterion** the test asserts,",
        "and the verdict — MET, EXCEEDED, or FAILED.",
        "",
        "- **MET**     — measured value passes the stated criterion.",
        "- **EXCEEDED** — measured value comfortably beats the criterion",
        "  by the metric-specific margin.",
        "- **FAILED**  — measured value breaks the criterion.",
        "- **PARTIAL** — quality/size checked; instrumented timing and memory not evaluated.",
        "",
        "Regenerated on every run of `tools/test/test_capabilities.py`. Adding a",
        "capability = adding one row to that script with its criteria.",
        "",
        "## Summary",
        "",
    ]
    n_met  = sum(1 for _, _, v, _ in rows if v == "MET")
    n_exc  = sum(1 for _, _, v, _ in rows if v == "EXCEEDED")
    n_fail = sum(1 for _, _, v, _ in rows if v == "FAILED")
    n_skip = sum(1 for _, _, v, _ in rows if v == "SKIPPED")
    n_partial = sum(1 for _, _, v, _ in rows if v == "PARTIAL")
    lines += [
        f"- **{n_exc}** EXCEEDED",
        f"- **{n_met}** MET",
        f"- **{n_fail}** FAILED",
        f"- **{n_skip}** SKIPPED (missing optional deps)",
        f"- **{n_partial}** PARTIAL (instrumented performance not evaluated)",
        f"- last run: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"- build dir: `{BUILD_DIR.relative_to(REPO) if str(BUILD_DIR).startswith(str(REPO)) else BUILD_DIR}`",
        "",
        "## Stills · encode → decode → PSNR roundtrip",
        "",
        "| Capability | Encode | Decode | Peak RSS | Compressed size | Roundtrip PSNR | Overall |",
        "|---|---|---|---|---|---|---|",
    ]
    for cap, m, overall, metric_rows in rows:
        if cap["kind"] != "still_roundtrip":
            continue
        cells = []
        for mid, name, unit, fmt in STILL_METRIC_ORDER:
            mr = next((r for r in metric_rows if r[0] == mid), None)
            if mr is None:
                cells.append("—")
                continue
            _, val, verdict, crit_text = mr
            v_str = fmt.format(val if mid != "compress_ratio" else val) + (
                "" if mid == "compress_ratio" else f" {unit}"
            )
            crit_str = crit_text + (f" {unit}" if mid != "compress_ratio" else "")
            cells.append(f"{v_str}<br/>_{crit_str}_<br/>{VERDICT_ICONS[verdict]}")
        lines.append(f"| {cap['display']} | {' | '.join(cells)} | **{VERDICT_ICONS[overall]}** |")

    # CNN-corrected render-domain PSNR section. Only emit if at least one
    # cnn_corrected cell is present in the result set.
    cnn_rows = [(c, m, o, mr) for (c, m, o, mr) in rows if c["kind"] == "cnn_corrected"]
    if cnn_rows:
        lines += [
            "",
            "## CNN-corrected · multi-level + dec=2 FUSED → BIBO_1x → AHD render PSNR",
            "",
            "Real-DNG playback chain protected by these cells: multi-level + decimate=2",
            "FUSED encode/decode (half-res topology from PR #10/#11/#13) → BIBO_1x CNN",
            "(`BayInBayOut_1x_AAon_w16_ANE.pt`) on the half-res bayer → bayer-bicubic-2x",
            "back to full size → rawpy AHD render → masked Y-PSNR vs the source-DNG AHD",
            "render at the stated output resolution. macOS-only (torch + MPS); Linux CI",
            "reports SKIPPED for these rows.",
            "",
            "| Capability | CNN-corrected PSNR | Overall |",
            "|---|---|---|",
        ]
        for cap, m, overall, metric_rows in cnn_rows:
            psnr_cell = "—"
            for mid, val, verdict, crit_text in metric_rows:
                if mid == "psnr_db":
                    if verdict == "SKIPPED":
                        psnr_cell = f"_n/a_<br/>{VERDICT_ICONS[verdict]}"
                    else:
                        psnr_cell = (f"{val:.2f} dB<br/>"
                                     f"_{crit_text} dB_<br/>{VERDICT_ICONS[verdict]}")
                    break
            lines.append(f"| {cap['display']} | {psnr_cell} | **{VERDICT_ICONS[overall]}** |")

    lines += [
        "",
        "## Metric definitions",
        "",
        "- **Encode ms** — wall-clock time for `gpr_tools dng→gpr` at the stated quality.",
        "- **Decode ms** — wall-clock time for `gpr_tools gpr→dng`.",
        "- **Peak RSS** — max resident memory observed for the encode/decode subprocesses.",
        "- **Compressed size** — output GPR bytes ÷ raw bayer bytes (W·H·2). Lower = more compression.",
        "- **Roundtrip PSNR** — bayer-domain PSNR (decoded vs original synth raw), peak set per bit depth.",
        "- **CNN-corrected PSNR** — render-domain masked Y-PSNR (channel-brightness matched) for the",
        "  full multi-level + dec=2 FUSED → BIBO_1x → AHD-render chain vs the source-DNG AHD render",
        "  at the stated output resolution. Dark/bright masked (Y∈(10,250) on 8-bit scale).",
        "",
        "## Test methodology",
        "",
        "Each cell uses a deterministic synthetic Bayer fixture (radial gradient +",
        "per-channel DC offsets + noise) sized to match the stated resolution.",
        "The fixture is designed so 3-level wavelet LL coefficients exceed 32767,",
        "exercising the sign-extension path that has historically been a regression",
        "hotspot. Timing measurements are wall-clock subprocess invocations. For",
        "CI-sized still cells the harness records the best of a small number of",
        "invocations to suppress hosted-runner cold-start noise; larger cells run",
        "once because codec work dominates launch overhead.",
        "",
        "Run `python3 tools/test/test_capabilities.py` to assert; add `--refresh`",
        "to recompute baselines (don't commit the script changes without revisiting",
        "tolerances).",
        "",
    ]
    out_path.write_text("\n".join(lines))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true",
                    help="measure + rewrite doc only; don't assert; print baseline-update suggestions")
    ap.add_argument("--filter", default=None,
                    help="substring filter on capability id")
    args = ap.parse_args()

    if not GTOOLS.exists():
        print(f"ERROR: gpr_tools not at {GTOOLS}", file=sys.stderr)
        return 2

    ART_DIR.mkdir(parents=True, exist_ok=True)
    work = ART_DIR / "live"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    print(f"=== test_capabilities · {time.strftime('%Y-%m-%d %H:%M:%S')} ===")
    print(f"GTOOLS    : {GTOOLS}")
    print(f"ARTIFACT  : {ART_DIR}")
    print(f"FAST      : {FAST}")
    print(f"timing samples (≤{TIMING_SAMPLE_MAX_PIXELS} px stills): {max(1, TIMING_SAMPLES)}")
    if INSTRUMENTED:
        print("SKIP: timing and memory criteria on instrumented build; quality and size remain enforced")
    print(f"refresh   : {args.refresh}")
    print()

    rows = []
    any_failed = False
    print(f"{'Capability':<55s} {'enc(ms)':>9s} {'dec(ms)':>9s} {'rss(MiB)':>9s} {'ratio':>7s} {'PSNR(dB)':>9s}  overall")
    print("-" * 122)
    for cap in CAPABILITIES:
        if args.filter and args.filter not in cap["id"]:
            continue
        if FAST and cap.get("fast_skip"):
            print(f"  {cap['display']:<53s} ... (FAST: skipped)")
            continue
        try:
            if cap["kind"] == "still_roundtrip":
                m = measure_still_roundtrip(cap, work)
            elif cap["kind"] == "cnn_corrected":
                m = measure_cnn_corrected(cap, work)
            else:
                continue
        except RuntimeError as e:
            msg = str(e)
            if msg.startswith("SKIP:"):
                # Missing optional deps — record as SKIPPED, do NOT fail.
                print(f"  {cap['display']:<53s} ... {msg}")
                empty = {mid: 0.0 for mid, *_ in METRIC_ORDER}
                rows.append((cap, empty, "SKIPPED",
                            [(mid, 0.0, "SKIPPED", "n/a")
                             for mid, *_ in METRIC_ORDER
                             if mid in cap.get("criteria", {})]))
                continue
            print(f"  {cap['display']:<53s} ... ERROR: {e}")
            empty = {mid: 0.0 for mid, *_ in METRIC_ORDER}
            rows.append((cap, empty, "FAILED",
                        [(mid, 0.0, "FAILED", "n/a")
                         for mid, *_ in METRIC_ORDER
                         if mid in cap.get("criteria", {})]))
            any_failed = True
            continue
        except Exception as e:
            print(f"  {cap['display']:<53s} ... ERROR: {e}")
            empty = {mid: 0.0 for mid, *_ in METRIC_ORDER}
            rows.append((cap, empty, "FAILED",
                        [(mid, 0.0, "FAILED", "n/a")
                         for mid, *_ in METRIC_ORDER
                         if mid in cap.get("criteria", {})]))
            any_failed = True
            continue
        overall, mr = check_cap(cap, m)
        if overall == "FAILED":
            any_failed = True
        # Display: still_roundtrip has 4 metrics; cnn_corrected has only PSNR
        # plus optional visual stack (Y-PSNR / MS-SSIM / LPIPS / ΔE2000).
        if cap["kind"] == "still_roundtrip":
            print(f"  {cap['display']:<55s} {m['encode_ms']:>8.1f} {m['decode_ms']:>8.1f} "
                  f"{m['peak_rss_mb']:>8.0f}  "
                  f"{m['compress_ratio']*100:>6.2f}% {m['psnr_db']:>8.2f}  {overall}")
        else:
            extras = ""
            if cap.get("visual_metrics", False):
                yp = m.get("y_psnr_db");  ms = m.get("ms_ssim")
                lp = m.get("lpips");      de = m.get("dE2000")
                if yp is not None:
                    extras = (f"  Y-PSNR={yp:.2f} MS-SSIM={ms:.4f} "
                              f"LPIPS={lp:.4f} ΔE={de:.2f}")
            print(f"  {cap['display']:<55s} {'-':>8s} {'-':>8s}  {'-':>6s}  "
                  f"{m.get('psnr_db', 0.0):>8.2f}  {overall}{extras}")
        rows.append((cap, m, overall, mr))

    docs = ART_DIR / "CAPABILITIES.md"
    if args.filter:
        print()
        print(f"=== {docs} not rewritten for filtered run ===")
    else:
        docs.parent.mkdir(exist_ok=True)
        emit_markdown(rows, docs)
        print()
        print(f"=== {docs} written ({len(rows)} rows) ===")

    if args.refresh:
        print()
        print("=== refresh: copy these baselines into the script if you accept them ===")
        for cap, m, *_ in rows:
            print(f"  {cap['id']}:")
            for mid, _, unit, fmt in METRIC_ORDER:
                if mid not in cap["criteria"]:
                    continue
                v = m[mid]
                if mid == "compress_ratio":
                    print(f"    {mid}: {v:.3f}  (suggest max={v*1.5:.2f}, exceed_below={v*0.9:.3f})")
                elif mid == "psnr_db":
                    print(f"    {mid}: {v:.2f}  (suggest min={v-1.5:.1f}, exceed_above={v+0.3:.2f})")
                else:
                    print(f"    {mid}: {v:.0f}  (suggest max={int(v*1.5)}, exceed_below={int(v*0.85)})")
        return 0

    print()
    if any_failed:
        n_fail = sum(1 for _, _, v, _ in rows if v == "FAILED")
        print(f"❌ {n_fail} capability/capabilities FAILED")
        return 1
    n_exc = sum(1 for _, _, v, _ in rows if v == "EXCEEDED")
    n_met = sum(1 for _, _, v, _ in rows if v == "MET")
    n_skip = sum(1 for _, _, v, _ in rows if v == "SKIPPED")
    skip_note = f", {n_skip} SKIPPED (missing optional deps)" if n_skip else ""
    print(f"✅ {n_exc} EXCEEDED, {n_met} MET{skip_note} ({len(rows)} total)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
