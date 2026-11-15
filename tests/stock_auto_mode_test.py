#!/usr/bin/env python3
"""
Unit & Integration Tests for Sparse-8K Wavelet Encoding (`GPR_SPARSE_8K`) and
Scene-Adaptive Auto Mode (`tools/stock_auto_mode.py`).
"""

import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))

import stock_auto_mode as auto_mode


def find_gpr_tools() -> Path:
    candidates = [
        Path(os.environ.get("GPR_TOOLS_BIN", "")),
        Path("/tmp/gpr-native-build/source/app/gpr_tools/gpr_tools"),
        Path("/mnt/ssd/gpr_work/stock-pi5-validation-20261113/build/source/app/gpr_tools/gpr_tools"),
        REPO_ROOT / "build" / "source" / "app" / "gpr_tools" / "gpr_tools",
    ]
    for c in candidates:
        if c.name and c.exists() and os.access(c, os.X_OK):
            return c
    raise FileNotFoundError("Could not locate compiled gpr_tools binary")


def find_vc5_apps():
    enc_candidates = [
        Path(os.environ.get("VC5_ENCODER_BIN", "")),
        Path("/tmp/gpr-native-build/source/app/vc5_encoder_app/vc5_encoder_app"),
        Path("/mnt/ssd/gpr_work/stock-pi5-validation-20261113/build/source/app/vc5_encoder_app/vc5_encoder_app"),
        REPO_ROOT / "build" / "source" / "app" / "vc5_encoder_app" / "vc5_encoder_app",
    ]
    dec_candidates = [
        Path(os.environ.get("VC5_DECODER_BIN", "")),
        Path("/tmp/gpr-native-build/source/app/vc5_decoder_app/vc5_decoder_app"),
        Path("/mnt/ssd/gpr_work/stock-pi5-validation-20261113/build/source/app/vc5_decoder_app/vc5_decoder_app"),
        REPO_ROOT / "build" / "source" / "app" / "vc5_decoder_app" / "vc5_decoder_app",
    ]
    enc = next((c for c in enc_candidates if c.name and c.exists() and os.access(c, os.X_OK)), None)
    dec = next((c for c in dec_candidates if c.name and c.exists() and os.access(c, os.X_OK)), None)
    if not enc or not dec:
        raise FileNotFoundError("Could not locate compiled vc5_encoder_app / vc5_decoder_app binaries")
    return enc, dec


class StockAutoModeAndSparse8KTests(unittest.TestCase):
    def test_dark_scene_selects_12mp_high_snr_dr(self):
        rng = np.random.default_rng(42)
        # Dark scene with median around 140 counts above black=0
        dark_bayer = rng.integers(80, 200, size=(512, 512), dtype=np.uint16)
        decision = auto_mode.analyze_bayer_scene(dark_bayer)
        self.assertTrue(decision.is_dark)
        self.assertFalse(decision.is_high_contrast)
        self.assertEqual(decision.selected_mode, "12MP_HIGH_SNR_DR")
        self.assertEqual(decision.target_resolution, (4096, 3072))
        self.assertGreater(decision.radiometry.expected_binning_snr_gain_db, 5.95)

    def test_high_contrast_scene_selects_12mp_high_snr_dr(self):
        # High contrast scene: bottom half ~120 counts, top half ~11000 counts (median > 300, C_99/10 > 6 stops)
        hc_bayer = np.full((512, 512), 450, dtype=np.uint16)
        hc_bayer[:100, :] = 90
        hc_bayer[350:, :] = 11500
        decision = auto_mode.analyze_bayer_scene(hc_bayer)
        self.assertFalse(decision.is_dark)
        self.assertTrue(decision.is_high_contrast)
        self.assertEqual(decision.selected_mode, "12MP_HIGH_SNR_DR")
        self.assertEqual(decision.target_resolution, (4096, 3072))

    def test_bright_low_contrast_scene_selects_50mp_sparse_8k(self):
        rng = np.random.default_rng(7)
        # Bright, well-exposed low-contrast daylight scene: 800..2400 counts
        bright_bayer = rng.integers(800, 2400, size=(512, 512), dtype=np.uint16)
        decision = auto_mode.analyze_bayer_scene(bright_bayer)
        self.assertFalse(decision.is_dark)
        self.assertFalse(decision.is_high_contrast)
        self.assertEqual(decision.selected_mode, "50MP_SPARSE_8K")
        self.assertEqual(decision.target_resolution, (8192, 6144))
        self.assertEqual(decision.recommended_env["GPR_SPARSE_8K"], "1")

    def test_sparse_8k_bit_exact_default_and_12mp_ll1_preservation(self):
        gpr_tools = find_gpr_tools()
        w, h = 1024, 1024
        rng = np.random.default_rng(1234)
        # Construct a synthetic 14-bit RGGB image with smooth 12 MP gradients, sharp 8K edges, and realistic Poisson shot noise
        yy, xx = np.mgrid[0:h, 0:w]
        base = (800.0 + 400.0 * np.sin(xx / 40.0) + 300.0 * np.cos(yy / 40.0)).astype(np.float64)
        # Add sharp 8K checkerboard/bars in a localized bright region
        edge_patch = ((xx >= 320) & (xx < 704) & (yy >= 320) & (yy < 704))
        base[edge_patch] += 900.0 * ((xx[edge_patch] // 2 + yy[edge_patch] // 2) % 2)
        noise = rng.normal(0.0, np.sqrt(base * 1.5 + 16.0))
        raw_u16 = np.clip(base + noise, 64, 15000).astype(np.uint16)

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            raw_in = tmp / "in.raw"
            raw_u16.tofile(raw_in)

            def encode_and_decode(tag: str, extra_env: dict):
                env = dict(os.environ)
                env.pop("GPR_OPTICAL_CORING", None)
                env.pop("GPR_SPARSE_8K", None)
                env.update(extra_env)
                gpr_out = tmp / f"{tag}.gpr"
                raw_out = tmp / f"{tag}.raw"
                subprocess.run(
                    [str(gpr_tools), "-i", str(raw_in), "-o", str(gpr_out), "-w", str(w), "-h", str(h), "-p", str(w * 2), "-q", "3"],
                    env=env,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                clean_env = dict(os.environ)
                clean_env.pop("GPR_OPTICAL_CORING", None)
                clean_env.pop("GPR_SPARSE_8K", None)
                subprocess.run(
                    [str(gpr_tools), "-i", str(gpr_out), "-o", str(raw_out)],
                    env=clean_env,
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                gpr_bytes = gpr_out.read_bytes()
                dec = np.fromfile(raw_out, dtype=np.uint16).reshape(h, w)
                return gpr_bytes, dec

            b_unset, dec_unset = encode_and_decode("unset", {})
            b_zero, dec_zero = encode_and_decode("zero", {"GPR_SPARSE_8K": "0", "GPR_OPTICAL_CORING": "0"})
            b_sparse, dec_sparse = encode_and_decode("sparse", {"GPR_SPARSE_8K": "1", "GPR_OPTICAL_CORING": "1"})

            # 1. Default unset and explicit 0 must be 100% bit-exact
            self.assertEqual(hashlib.sha256(b_unset).hexdigest(), hashlib.sha256(b_zero).hexdigest())
            np.testing.assert_array_equal(dec_unset, dec_zero)

            # 2. Sparse-8K must reduce .gpr byte size significantly
            self.assertLess(len(b_sparse), int(len(b_unset) * 0.85))

            # 3. 12 MP 2x2-binned hierarchy (LL1) must remain ultra-high fidelity (> 60 dB PSNR)
            ref_12mp = auto_mode.bin_50mp_to_12mp_rggb(raw_u16).astype(np.float64)
            dec_12mp = auto_mode.bin_50mp_to_12mp_rggb(dec_sparse).astype(np.float64)
            mse_12mp = float(np.mean((ref_12mp - dec_12mp) ** 2))
            psnr_12mp = 10.0 * np.log10((16383.0 ** 2) / mse_12mp)
            self.assertGreater(psnr_12mp, 60.0)

    def test_dual_raw_half_resolution_12mp_extraction(self):
        enc_bin, dec_bin = find_vc5_apps()
        w, h = 1024, 768
        rng = np.random.default_rng(2026)
        yy, xx = np.mgrid[0:h, 0:w]
        base = (900.0 + 350.0 * np.sin(xx / 32.0) + 250.0 * np.cos(yy / 32.0)).astype(np.float64)
        noise = rng.normal(0.0, np.sqrt(base * 1.5 + 16.0))
        raw_u16 = np.clip(base + noise, 64, 15000).astype(np.uint16)

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            raw_in = tmp / "in_full.raw"
            vc5_out = tmp / "sparse.vc5"
            dec_half_out = tmp / "dec_half.raw"
            raw_u16.tofile(raw_in)

            env_enc = dict(os.environ)
            env_enc["GPR_OPTICAL_CORING"] = "1"
            env_enc["GPR_SPARSE_8K"] = "1"
            subprocess.run(
                [str(enc_bin), "-i", str(raw_in), "-o", str(vc5_out), "-w", str(w), "-h", str(h), "-p", str(w * 2), "-x", "rggb14"],
                env=env_enc,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            res = auto_mode.extract_dual_raw_12mp(
                vc5_out, dec_half_out, dec_bin, full_width=w, full_height=h
            )
            self.assertEqual(res["width"], w // 2)
            self.assertEqual(res["height"], h // 2)
            self.assertEqual(res["raw_bytes"], (w // 2) * (h // 2) * 2)

            half_arr = np.fromfile(dec_half_out, dtype=np.uint16).reshape(h // 2, w // 2).astype(np.float64)
            ref_half = auto_mode.bin_50mp_to_12mp_rggb(raw_u16).astype(np.float64)
            mse = float(np.mean((half_arr - ref_half) ** 2))
            psnr = 10.0 * np.log10((16383.0 ** 2) / mse)
            self.assertGreater(psnr, 58.0)

    def test_hindsight_raw_ring_buffer_bounded_memory_and_gvid_v3_flush(self):
        # 1 MB ring buffer, 1.0s pre-roll window
        ring = auto_mode.HindSightRawRingBuffer(
            max_bytes=1024 * 1024,
            max_pre_roll_seconds=1.0,
            fps_num=60,
            fps_den=1,
        )
        fake_payload = b"VC-5" + (b"\x12\x34\x56\x78" * 32000)  # ~128 KB per frame
        # Push 20 frames spaced by 100 ms (2.0 seconds total) -> only last ~8 frames fit in 1 MB / 1.0s
        for i in range(20):
            ring.push_frame(fake_payload, timestamp_us=i * 100_000, width=4096, height=3072)

        self.assertLessEqual(ring.current_bytes, 1024 * 1024)
        self.assertLessEqual(ring.frame_count, 8)
        self.assertGreaterEqual(ring.frame_count, 6)

        with tempfile.TemporaryDirectory() as tmpdir:
            gvid_path = Path(tmpdir) / "hindsight.gvid"
            post_frame = auto_mode.HindSightRawFrame(
                timestamp_us=2_000_000,
                vc5_payload=fake_payload,
                width=4096,
                height=3072,
                mode="12MP_HIGH_SNR_DR",
            )
            receipt = ring.flush_to_gvid_v3(gvid_path, post_roll_frames=[post_frame])
            self.assertEqual(receipt["frame_count"], ring.frame_count + 1)
            blob = gvid_path.read_bytes()
            self.assertEqual(blob[:4], b"GVID")
            self.assertEqual(blob[-20:-16], b"GVDX")

    def test_capture_best_image_for_scene_end_to_end(self):
        gpr_tools = find_gpr_tools()
        rng = np.random.default_rng(99)
        # 1. Dark scene (1024x768) -> should bin 2x2 to 512x384 at Quality 5 High-Bitrate
        dark_bayer = rng.integers(80, 220, size=(768, 1024), dtype=np.uint16)
        # 2. Bright low-contrast scene (1024x768) -> should keep full 1024x768 with Sparse-50MP at Quality 3
        yy, xx = np.mgrid[0:768, 0:1024]
        bright_clean = 1400.0 + 350.0 * np.sin(xx / 32.0) + 250.0 * np.cos(yy / 32.0)
        bright_bayer = np.clip(bright_clean + rng.normal(0.0, np.sqrt(bright_clean * 1.5 + 16.0)), 64, 15000).astype(np.uint16)

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            dark_res = auto_mode.capture_best_image_for_scene(
                dark_bayer,
                tmp / "dark_best.gpr",
                gpr_tools,
                reconstruct_raw_out=tmp / "dark_rec.raw",
            )
            self.assertEqual(dark_res["selected_mode"], "12MP_HIGH_SNR_DR")
            self.assertEqual(dark_res["quality_tier"], 5)
            self.assertEqual((dark_res["encoded_width"], dark_res["encoded_height"]), (512, 384))
            self.assertGreater(dark_res["reconstructed_psnr_db"], 62.0)

            bright_res = auto_mode.capture_best_image_for_scene(
                bright_bayer,
                tmp / "bright_best.gpr",
                gpr_tools,
                reconstruct_raw_out=tmp / "bright_rec.raw",
            )
            self.assertEqual(bright_res["selected_mode"], "50MP_SPARSE_8K")
            self.assertEqual(bright_res["quality_tier"], 3)
            self.assertEqual((bright_res["encoded_width"], bright_res["encoded_height"]), (1024, 768))
            self.assertGreater(bright_res["reconstructed_psnr_db"], 50.0)
            dec_bright = np.fromfile(tmp / "bright_rec.raw", dtype=np.uint16).reshape(768, 1024).astype(np.float64)
            mse_vs_clean = float(np.mean((dec_bright - bright_clean) ** 2))
            psnr_vs_clean = 10.0 * np.log10((16383.0 ** 2) / mse_vs_clean)
            self.assertGreater(psnr_vs_clean, 56.0)


if __name__ == "__main__":
    unittest.main()
