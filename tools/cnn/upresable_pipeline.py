'End-to-end UPRESABLE pipeline:\n  source DNG (sensor sim)\n      → ml2_q3_dec2 encode → half-res .gpr (24 fps capture stream on Pi)\n      → decode → half-res Bayer\n      → BIBO_2x CNN on MPS → full-res Bayer (editable raw)\n      → ml2_q3 encode → full-res .gpr (the editable raw output file)\n      → decode (validate) → full-res Bayer\n      → pack full-res .gpr sequence → .gvid primary video container\n      → optional demosaic via gpr_tools + sips → 4K UHD ProRes review\n\nOutputs per frame:\n  $GPR_EXTERNAL_ROOT/artifacts/upresable/halfres/<name>.gpr   (capture file)\n  $GPR_EXTERNAL_ROOT/artifacts/upresable/fullres/<name>.gpr   (editable raw, post-upres)\n  $GPR_EXTERNAL_ROOT/artifacts/upresable/frames/<name>.tiff   (optional 4K UHD RGB for ProRes)\n\nFinal:\n  $GPR_EXTERNAL_ROOT/artifacts/upresable/upresable_timelapse.gvid (primary)\n  $GPR_EXTERNAL_ROOT/artifacts/upresable/upresable_timelapse.gpr1.mov (MOV compatibility)\n  $GPR_EXTERNAL_ROOT/artifacts/upresable/upresable_timelapse.mov  (optional ProRes 422 HQ)\n\nAlso: runs on the 4 gate images for regression-style verification of\nquality (bayer PSNR, Y-PSNR, etc.) of the full-res .gpr vs source DNG.\n'
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
import numpy as np
from PIL import Image
import cv2
import tifffile
import torch



sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(__file__).resolve().parents[2]
def default_external_root() -> Path:
    return external_path()


EXTERNAL_ROOT = Path(os.environ.get("GPR_EXTERNAL_ROOT", default_external_root()))
MODEL_ROOT = Path(os.environ.get("GPR_MODEL_ROOT", EXTERNAL_ROOT / "models"))
sys.path.insert(0, str(REPO / "tools" / "cnn"))
from model import build

CODEC = Path(os.environ.get("GPR_COEFF_IO_TOOL", REPO / "build-local/bin/coeff_io_tool"))
GPR_TOOLS = Path(os.environ.get(
    "GPR_TOOLS", REPO / "build-local/source/app/gpr_tools/gpr_tools"))


def _model_artifact(env_name: str, filename: str) -> Path:
    candidates = [
        os.environ.get(env_name),
        REPO / "models" / filename,
        MODEL_ROOT / filename,
        EXTERNAL_ROOT / "models" / filename,
    ]
    for c in candidates:
        if c and Path(c).exists():
            return Path(c)
    return Path(candidates[1])


BIBO2X_CKPT = _model_artifact(
    "GPR_BIBO2X_CKPT", "BayInBayOut_2x_AAon_w16_ANE_ML2_q3_dec2_diverse.pt")

OUT = Path(os.environ.get("GPR_UPRESABLE_OUT", EXTERNAL_ROOT / "artifacts" / "upresable"))
OUT.mkdir(parents=True, exist_ok=True)
(OUT / "halfres").mkdir(parents=True, exist_ok=True)
(OUT / "fullres").mkdir(parents=True, exist_ok=True)           # FUSED bitstream (codec native)
(OUT / "editable_dng").mkdir(parents=True, exist_ok=True)      # DNG-wrapped editable raw
(OUT / "editable_gpr").mkdir(parents=True, exist_ok=True)      # gpr_tools-encoded compressed editable raw
(OUT / "frames").mkdir(parents=True, exist_ok=True)

TARGET_W = 3840
TARGET_H = 2160
FULLRES_W = 8280
FULLRES_H = 5520
FPS = 24
RAW_NORM = 16383.0
TILE = 128
OUT_TILE = 512
OVERLAP = 16
BATCH = 32

Image.MAX_IMAGE_PIXELS = None


def encode_halfres(in_raw: Path, save_gpr: Path, out_raw: Path, w: int, h: int):
    """ml2_q3_dec2: encode source bayer to half-res .gpr + decode half-res bayer."""
    env = os.environ.copy()
    env.update({"FUSED_QUALITY":"3","FUSED_MULTI_LEVEL":"1","FUSED_WAVELET_LEVELS":"2",
                "GPR_COL_DECIMATE":"2","GPR_ROW_DECIMATE":"2","GPR_INCLUDE_LL":"1",
                "GPR_SAVE_TO": str(save_gpr)})
    r = subprocess.run([str(CODEC), str(in_raw), str(w), str(h), str(out_raw)],
                       capture_output=True, env=env)
    if r.returncode != 0:
        raise RuntimeError(f"halfres encode failed: {r.stderr[-300:]}")


def encode_fullres(in_raw: Path, save_gpr: Path, out_raw: Path, w: int, h: int):
    """ml2_q3 (no decimation): encode full-res bayer to full-res .gpr (editable raw)
    + decode validates the file. Output goes to out_raw at full-res."""
    env = os.environ.copy()
    env.update({"FUSED_QUALITY":"3","FUSED_MULTI_LEVEL":"1","FUSED_WAVELET_LEVELS":"2",
                "GPR_SAVE_TO": str(save_gpr)})
    r = subprocess.run([str(CODEC), str(in_raw), str(w), str(h), str(out_raw)],
                       capture_output=True, env=env)
    if r.returncode != 0:
        raise RuntimeError(f"fullres encode failed: {r.stderr[-300:]}")


def deinterleave(bayer):
    R  = bayer[0::2, 0::2]; G1 = bayer[0::2, 1::2]
    G2 = bayer[1::2, 0::2]; B  = bayer[1::2, 1::2]
    return R, G1, G2, B


_TAPER = None
def taper2d():
    global _TAPER
    if _TAPER is not None: return _TAPER
    def t1d(n, ov):
        w = np.ones(n, dtype=np.float32)
        ramp = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, ov))
        w[:ov] = ramp; w[-ov:] = ramp[::-1]
        return w
    _TAPER = np.outer(t1d(OUT_TILE, OVERLAP*4), t1d(OUT_TILE, OVERLAP*4)).astype(np.float32)
    return _TAPER


def run_bibo2x_mps(model, half_bayer_u16, device, res_scale=0.01):
    """BIBO_2x on MPS, tiled+batched. Input: half-res Bayer u16. Output: full-res Bayer u16."""
    R, G1, G2, B = deinterleave(half_bayer_u16)
    H_in, W_in = R.shape
    H_out, W_out = H_in * 2, W_in * 2          # per-plane 2x
    # Build all tiles
    step = TILE - OVERLAP
    n_y = (H_in + step - 1) // step
    n_x = (W_in + step - 1) // step
    tiles = []; coords = []
    for ty in range(n_y):
        for tx in range(n_x):
            y0 = min(ty * step, H_in - TILE)
            x0 = min(tx * step, W_in - TILE)
            tiles.append(np.stack([
                R [y0:y0+TILE, x0:x0+TILE].astype(np.float32) / RAW_NORM,
                G1[y0:y0+TILE, x0:x0+TILE].astype(np.float32) / RAW_NORM,
                G2[y0:y0+TILE, x0:x0+TILE].astype(np.float32) / RAW_NORM,
                B [y0:y0+TILE, x0:x0+TILE].astype(np.float32) / RAW_NORM,
            ]))
            coords.append((y0, x0))
    tiles_t = torch.from_numpy(np.stack(tiles)).to(device, non_blocking=True)
    n_tiles = tiles_t.shape[0]
    # Batched forward — output is (B, 4, 512, 512) (4-channel super-res 4x)
    preds = []
    with torch.no_grad():
        for i in range(0, n_tiles, BATCH):
            b = tiles_t[i:i+BATCH]
            base = torch.nn.functional.interpolate(b, scale_factor=2, mode="bicubic", align_corners=False).clamp(0,1)
            cnn = model(b)
            y = (base + res_scale * cnn).clamp(0, 1)
            preds.append(y.cpu().numpy())
    preds = np.concatenate(preds, axis=0)   # (n_tiles, 4, 256, 256) — bibo_2x produces 2x per plane
    # Re-stitch into 4 full-res planes (per plane 2x of TILE = 256x256)
    PLANE_TILE = TILE * 2     # 256
    H_p, W_p = H_in * 2, W_in * 2
    plane_out = [np.zeros((H_p, W_p), dtype=np.float32) for _ in range(4)]
    plane_wt = np.zeros((H_p, W_p), dtype=np.float32)
    # Build plane-sized taper
    def t1d(n, ov):
        w = np.ones(n, dtype=np.float32)
        ramp = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, ov))
        w[:ov] = ramp; w[-ov:] = ramp[::-1]
        return w
    plane_taper = np.outer(t1d(PLANE_TILE, OVERLAP*2), t1d(PLANE_TILE, OVERLAP*2)).astype(np.float32)
    for i, (y0, x0) in enumerate(coords):
        oy0 = y0 * 2; ox0 = x0 * 2
        for ch in range(4):
            plane_out[ch][oy0:oy0+PLANE_TILE, ox0:ox0+PLANE_TILE] += preds[i, ch] * plane_taper
        plane_wt[oy0:oy0+PLANE_TILE, ox0:ox0+PLANE_TILE] += plane_taper
    plane_wt = np.maximum(plane_wt, 1e-6)
    # Reassemble Bayer
    full_h, full_w = H_in * 4, W_in * 4
    full_bayer = np.zeros((full_h, full_w), dtype=np.uint16)
    for ch_idx, (off_y, off_x) in enumerate([(0,0), (0,1), (1,0), (1,1)]):
        plane = np.clip(plane_out[ch_idx] / plane_wt * RAW_NORM, 0, RAW_NORM).astype(np.uint16)
        full_bayer[off_y::2, off_x::2] = plane
    return full_bayer


def make_editable_dng(bayer: np.ndarray, src_dng: Path, work: Path,
                       persist_dng_path: Path = None) -> Path:
    """Wrap a full-res Bayer plane as a DNG (universal editable raw format).
    Returns the DNG path. Optionally also persists to a deliverable location."""
    params_cache = work / f"params_{src_dng.stem}.json"
    if not params_cache.exists():
        cp = subprocess.run([str(GPR_TOOLS), "-i", str(src_dng), "-d", "1"],
                            capture_output=True, text=True)
        if cp.returncode != 0:
            raise RuntimeError(f"params dump failed: {cp.stderr[-200:]}")
        lines = [l for l in cp.stdout.splitlines() if not l.startswith("[")]
        params_cache.write_text("\n".join(lines))
    params = json.loads(params_cache.read_text())
    h, w = bayer.shape
    params["input_width"]=w; params["input_height"]=h; params["input_pitch"]=w*2
    pp = work / f"params_run_{w}x{h}.json"
    pp.write_text(json.dumps(params))
    raw_in = work / f"render_in_{w}x{h}.raw"
    bayer.astype("<u2").tofile(raw_in)
    dng_out = work / f"render_{w}x{h}.dng"
    r = subprocess.run([str(GPR_TOOLS), "-i", str(raw_in), "-w", str(w), "-h", str(h),
                        "-x", "rggb14", "-o", str(dng_out), "-a", str(pp)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"gpr_tools DNG write failed: {r.stderr[-200:]}")
    if persist_dng_path is not None:
        shutil.copy(dng_out, persist_dng_path)
    return dng_out


def encode_dng_to_gpr_via_gpr_tools(dng_path: Path, gpr_path: Path, q: int = 3):
    """Use gpr_tools to encode a DNG as a compressed .gpr file (DNG-wrapped,
    openable by any GPR-aware tool). This produces a properly-wrapped GPR file,
    unlike coeff_io_tool which writes raw FUSED bitstream."""
    r = subprocess.run([str(GPR_TOOLS), "-q", str(q), "-i", str(dng_path),
                        "-o", str(gpr_path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"gpr_tools DNG→GPR failed: {r.stderr[-200:]}")


def render_dng_to_tiff16(dng_path: Path, out_tiff: Path, target_dims=None):
    """sips render a DNG to 16-bit TIFF (preserves source DNG bit depth).
    Returns the on-disk path; if target_dims is given, resizes preserving uint16."""
    r = subprocess.run(["sips", "-s", "format", "tiff", str(dng_path),
                        "--out", str(out_tiff)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"sips failed: {r.stderr[-200:]}")
    if target_dims is not None:
        arr = tifffile.imread(out_tiff)
        if arr.dtype != np.uint16:
            # Coerce — should not happen but guard
            arr = arr.astype(np.uint16) * 257 if arr.dtype == np.uint8 else arr.astype(np.uint16)
        H, W = arr.shape[:2]
        if (W, H) != target_dims:
            arr = cv2.resize(arr, target_dims, interpolation=cv2.INTER_LANCZOS4)
        tifffile.imwrite(out_tiff, arr)


def render_dng_to_png_8bit(dng_path: Path, out_png: Path, target_dims=None):
    """LEGACY 8-bit path. Only used when --eight-bit is passed on CLI.
    Will introduce banding in smooth gradients (sky, skin) — confirmed via
    sky-patch unique-level counts (24 unique levels in 8-bit PNG vs 738 in
    16-bit source DNG). Use render_dng_to_tiff16 instead unless an external
    consumer explicitly requires 8-bit PNG."""
    r = subprocess.run(["sips", "-s", "format", "png", str(dng_path), "--out", str(out_png)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"sips failed: {r.stderr[-200:]}")
    if target_dims is not None:
        im = Image.open(out_png).convert("RGB")
        if im.size != target_dims:
            arr = np.array(im)
            arr = cv2.resize(arr, target_dims, interpolation=cv2.INTER_LANCZOS4)
            Image.fromarray(arr).save(out_png)


def render_bayer_to_frame(bayer: np.ndarray, src_dng: Path, work: Path,
                          out_frame: Path, target_dims=None,
                          persist_dng: Path = None, persist_gpr: Path = None,
                          eight_bit: bool = False):
    """Wrap Bayer as DNG (saving to persist_dng if given), optionally
    re-encode as gpr_tools .gpr (saving to persist_gpr), then render the
    DNG to a video frame file. Default: 16-bit TIFF (preserves source
    bit depth). Use eight_bit=True only for 8-bit PNG (legacy / banding)."""
    dng = make_editable_dng(bayer, src_dng, work, persist_dng_path=persist_dng)
    if persist_gpr is not None:
        encode_dng_to_gpr_via_gpr_tools(dng, persist_gpr, q=3)
    if eight_bit:
        render_dng_to_png_8bit(dng, out_frame, target_dims=target_dims)
    else:
        render_dng_to_tiff16(dng, out_frame, target_dims=target_dims)


def process_one_frame(args):
    """Standalone worker: reads DNG, codec roundtrip, returns (idx, halfres_path,
    half_bayer_u16). Main thread runs BIBO_2x CNN + re-encode + render."""
    frame_idx, dng_path, work_root = args
    dng = Path(dng_path)
    work = Path(work_root) / f"w_{frame_idx}"
    work.mkdir(exist_ok=True)

    # 1. Read DNG → bayer
    with tifffile.TiffFile(dng) as tf:
        bayer = tf.pages[0].pages[0].asarray().astype('<u2')
    h, w = bayer.shape

    # 2. Encode half-res .gpr (24 fps capture sim) — also decodes half-res Bayer back
    in_raw = work / "in.raw"
    bayer.tofile(in_raw)
    halfres_gpr = OUT / "halfres" / f"{dng.stem}.gpr"
    half_raw = work / "half.raw"
    encode_halfres(in_raw, halfres_gpr, half_raw, w, h)
    half_bayer = np.fromfile(half_raw, dtype=np.uint16).reshape(h // 2, w // 2)

    shutil.rmtree(work, ignore_errors=True)
    return (frame_idx, str(halfres_gpr), half_bayer, str(dng))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["timelapse", "regression", "both"], default="both")
    ap.add_argument("--n-frames", type=int, default=60, help="timelapse frame count")
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--eight-bit", action="store_true",
                    help="Render frames as 8-bit PNG (legacy; causes sky banding). "
                         "Default is 16-bit TIFF.")
    ap.add_argument("--dng-export", action="store_true",
                    help="Also write per-frame editable DNG + gpr_tools .gpr "
                         "(legacy; ~70%% of per-frame time). Default off: .gvid "
                         "is the primary deliverable, DNG is an optional "
                         "correctness check / hand-off to Adobe / darktable.")
    ap.add_argument("--src-dir", type=Path, default=external_path('barnsky_full_dngs'),
                    help="Directory of source DNG frames for timelapse mode.")
    ap.add_argument("--image-id", action="append",
                    help="Specific source DNG stem to include in timelapse mode. "
                         "May be passed multiple times.")
    ap.add_argument("--render-prores", action="store_true",
                    help="Also assemble a 16-bit ProRes 422 HQ review file by "
                         "demosaicing every frame through sips. Useful for "
                         "human review; adds ~1.5 s/frame.")
    args = ap.parse_args()
    EIGHT_BIT = args.eight_bit
    FRAME_EXT = ".png" if EIGHT_BIT else ".tiff"
    FFMPEG_PIXFMT_IN = "rgb24" if EIGHT_BIT else "rgb48le"

    if not CODEC.exists():
        raise RuntimeError(f"missing {CODEC}")
    if not BIBO2X_CKPT.exists():
        raise RuntimeError(f"missing {BIBO2X_CKPT}")

    # Load BIBO_2x CNN onto MPS
    device = "mps" if torch.backends.mps.is_available() else "cpu"
    print(f"BIBO_2x CNN device: {device}")
    ck = torch.load(str(BIBO2X_CKPT), map_location='cpu', weights_only=False)
    variant = ck.get('variant', 'F_ane')
    model = build(variant)
    state = ck['backbone_state'] if 'backbone_state' in ck else ck
    model.load_state_dict(state, strict=True)
    model.eval().to(device)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"BIBO_2x params: {n_params:,}")

    work_root = Path(tempfile.mkdtemp(prefix="upresable_", dir=OUT))
    overall_t0 = time.time()

    # Stats accumulator for the comprehensive table
    stats = {
        "halfres_gpr_bytes": [],
        "fullres_gpr_bytes": [],
        "encode_half_ms": [],
        "encode_full_ms": [],
        "bibo2x_ms": [],
        "render_ms": [],
    }

    # === REGRESSION on 4 gate images first ===
    regression_results = {}
    if args.mode in ("regression", "both"):
        print("\n=== REGRESSION: full pipeline on 4 gate images ===")
        gate_dngs = [
            str(external_path('artifacts/visual_compare_20260525/source_dngs/Z8Z_0001.dng')),
            str(external_path('barnsky_full_dngs/Z8Z_0067.dng')),
            str(external_path('artifacts/visual_compare_20260525/source_dngs/Z8Z_5323.dng')),
            str(external_path('artifacts/visual_compare_20260525/source_dngs/Z8Z_6693.dng')),
        ]
        for i, dng_path in enumerate(gate_dngs):
            t0 = time.time()
            idx, halfres_gpr, half_bayer, _dng = process_one_frame((i, dng_path, str(work_root)))
            dng = Path(dng_path); img_id = dng.stem
            with tifffile.TiffFile(dng) as tf:
                source_bayer = tf.pages[0].pages[0].asarray().astype('<u2')
            half_size = Path(halfres_gpr).stat().st_size

            # BIBO_2x → full-res Bayer
            t_cnn = time.time()
            full_bayer = run_bibo2x_mps(model, half_bayer, device)
            cnn_ms = (time.time() - t_cnn) * 1000

            # Encode full-res Bayer → full-res .gpr (editable raw)
            fullres_gpr = OUT / "fullres" / f"{img_id}.gpr"
            full_in = work_root / f"reg_full_{i}.raw"
            full_out = work_root / f"reg_full_dec_{i}.raw"
            full_bayer.tofile(full_in)
            t_enc = time.time()
            encode_fullres(full_in, fullres_gpr, full_out, full_bayer.shape[1], full_bayer.shape[0])
            enc_full_ms = (time.time() - t_enc) * 1000
            full_size = fullres_gpr.stat().st_size

            # Validate the saved full-res .gpr decodes back
            decoded_full_bayer = np.fromfile(full_out, dtype=np.uint16).reshape(
                full_bayer.shape[0], full_bayer.shape[1])

            # PSNR vs SOURCE bayer (the editable-raw fidelity metric)
            source_resized = source_bayer.astype(np.float32)
            decoded_f = decoded_full_bayer.astype(np.float32)
            mse = np.mean((source_resized - decoded_f) ** 2)
            bayer_psnr = 99.0 if mse <= 1e-9 else 10 * np.log10(16383**2 / mse)

            # Make proper editable-raw deliverables: DNG + gpr_tools .gpr
            editable_dng = OUT / "editable_dng" / f"{img_id}.dng"
            editable_gpr = OUT / "editable_gpr" / f"{img_id}.gpr"
            wk = work_root / "reg_render_work"; wk.mkdir(exist_ok=True)
            t_dng = time.time()
            make_editable_dng(decoded_full_bayer, Path(dng_path), wk, persist_dng_path=editable_dng)
            dng_ms = (time.time() - t_dng) * 1000
            t_gprt = time.time()
            encode_dng_to_gpr_via_gpr_tools(editable_dng, editable_gpr, q=3)
            gprt_ms = (time.time() - t_gprt) * 1000

            # Quick check: confirm the DNG opens via sips (proves it's editable raw)
            test_out = work_root / f"reg_{img_id}_open_test.tiff"
            sips_ok = True
            try:
                render_dng_to_tiff16(editable_dng, test_out)
            except Exception as e:
                sips_ok = False
            dng_size = editable_dng.stat().st_size
            gpr_t_size = editable_gpr.stat().st_size

            print(f"  {img_id}: halfres={half_size/1e6:.2f} MB  fullres_FUSED={full_size/1e6:.2f} MB  "
                  f"editable_DNG={dng_size/1e6:.2f} MB  editable_GPR={gpr_t_size/1e6:.2f} MB  "
                  f"bayer_PSNR={bayer_psnr:.2f} dB  DNG_opens={sips_ok}  ({time.time()-t0:.1f}s)")
            regression_results[img_id] = {
                "halfres_gpr_MB": half_size / 1e6,
                "fullres_FUSED_gpr_MB": full_size / 1e6,
                "editable_DNG_MB": dng_size / 1e6,
                "editable_GPR_MB": gpr_t_size / 1e6,
                "bayer_psnr_vs_source_dB": bayer_psnr,
                "bibo2x_ms": cnn_ms,
                "encode_fullres_ms": enc_full_ms,
                "wrap_dng_ms": dng_ms,
                "gpr_tools_encode_ms": gprt_ms,
                "dng_opens_in_raw_editor": sips_ok,
            }

    # === TIMELAPSE on N barnsky frames ===
    if args.mode in ("timelapse", "both"):
        print(f"\n=== TIMELAPSE: source frames from {args.src_dir} ===")
        if args.image_id:
            selected = [args.src_dir / f"{image_id}.dng" for image_id in args.image_id]
            missing = [str(path) for path in selected if not path.exists()]
            if missing:
                raise FileNotFoundError(f"missing requested source DNGs: {missing}")
        else:
            all_dngs = sorted(args.src_dir.glob("*.dng"))
            selected = all_dngs[:args.n_frames]
        if not selected:
            raise RuntimeError(f"no source DNG frames selected from {args.src_dir}")
        args.n_frames = len(selected)

        # File-based ffmpeg assembly (NO stdin pipe — avoids deadlocks under
        # parallel load; workers write TIFFs/PNGs to disk, ffmpeg reads at end).
        mov = OUT / "upresable_timelapse.mov"
        ffmpeg_seq_dir = OUT / "ffmpeg_seq"
        if args.render_prores:
            if ffmpeg_seq_dir.exists():
                shutil.rmtree(ffmpeg_seq_dir)
            ffmpeg_seq_dir.mkdir(parents=True)
            print(f"frames staged in: {ffmpeg_seq_dir}")
        print(f"output container: .gvid (neutral per-frame FUSED .gpr video)")
        print(f"MOV compatibility: GPR1/GPRr wrapper for gpr2prores/FFmpeg hand-off")
        if args.render_prores:
            print(f"ProRes review:     {mov} ({FRAME_EXT[1:].upper()} source, "
                  f"{'8-bit' if EIGHT_BIT else '16-bit'})")
        if args.dng_export:
            print(f"DNG export:        per-frame editable_dng/ + editable_gpr/")

        from concurrent.futures import ProcessPoolExecutor, as_completed
        frames_args = [(i, str(d), str(work_root)) for i, d in enumerate(selected)]
        completed = 0
        with ProcessPoolExecutor(max_workers=args.workers) as ex:
            futures = {ex.submit(process_one_frame, fa): fa[0] for fa in frames_args}
            for fut in as_completed(futures):
                idx, halfres_gpr, half_bayer, dng_path = fut.result()
                dng = Path(dng_path); name = dng.stem
                # BIBO_2x
                t_cnn = time.time()
                full_bayer = run_bibo2x_mps(model, half_bayer, device)
                cnn_ms = (time.time() - t_cnn) * 1000
                stats["bibo2x_ms"].append(cnn_ms)
                # Encode full-res .gpr + decode validation
                fullres_gpr = OUT / "fullres" / f"{name}.gpr"
                full_in = work_root / f"full_in_{idx}.raw"
                full_out = work_root / f"full_dec_{idx}.raw"
                full_bayer.tofile(full_in)
                t_enc = time.time()
                encode_fullres(full_in, fullres_gpr, full_out, full_bayer.shape[1], full_bayer.shape[0])
                stats["encode_full_ms"].append((time.time() - t_enc) * 1000)
                stats["halfres_gpr_bytes"].append(Path(halfres_gpr).stat().st_size)
                stats["fullres_gpr_bytes"].append(fullres_gpr.stat().st_size)
                decoded = np.fromfile(full_out, dtype=np.uint16).reshape(
                    full_bayer.shape[0], full_bayer.shape[1])
                # Render path is opt-in. GVID is the deliverable; the per-frame
                # DNG wrap + TIFF render exists only for human review or DNG export.
                if args.render_prores or args.dng_export:
                    t_render = time.time()
                    wk = work_root / "render_work"; wk.mkdir(exist_ok=True)
                    frame_path = OUT / "frames" / f"{name}{FRAME_EXT}"
                    editable_dng = (OUT / "editable_dng" / f"{name}.dng") if args.dng_export else None
                    editable_gpr = (OUT / "editable_gpr" / f"{name}.gpr") if args.dng_export else None
                    render_bayer_to_frame(decoded, dng, wk, frame_path,
                                           target_dims=(TARGET_W, TARGET_H),
                                           persist_dng=editable_dng,
                                           persist_gpr=editable_gpr,
                                           eight_bit=EIGHT_BIT)
                    stats["render_ms"].append((time.time() - t_render) * 1000)
                    if args.dng_export:
                        if "editable_dng_bytes" not in stats: stats["editable_dng_bytes"] = []
                        if "editable_gpr_bytes" not in stats: stats["editable_gpr_bytes"] = []
                        stats["editable_dng_bytes"].append(editable_dng.stat().st_size)
                        stats["editable_gpr_bytes"].append(editable_gpr.stat().st_size)
                    if args.render_prores:
                        seq_link = ffmpeg_seq_dir / f"frame_{idx:05d}{FRAME_EXT}"
                        if seq_link.exists() or seq_link.is_symlink():
                            seq_link.unlink()
                        seq_link.symlink_to(frame_path.absolute())
                # Clean up per-frame intermediates
                for p in [full_in, full_out]:
                    if p.exists(): p.unlink()
                completed += 1
                if completed % 5 == 0 or completed == args.n_frames:
                    rate = completed / (time.time() - overall_t0)
                    rem = (args.n_frames - completed) / max(rate, 1e-6)
                    print(f"  {completed}/{args.n_frames}  rate={rate:.2f} f/s  rem={rem:.0f}s  "
                          f"(last: cnn={cnn_ms:.0f}ms)")

        # Primary deliverable: neutral .gvid of the full-res .gpr sequence.
        gvid = OUT / "upresable_timelapse.gvid"
        print(f"\nPacking .gvid from {args.n_frames} full-res .gpr frames...")
        t_gvid = time.time()
        gvid_cmd = [
            sys.executable, str(REPO / "tools/gvid_pack.py"),
            str(OUT / "fullres"), str(gvid),
            "--width", str(FULLRES_W),
            "--height", str(FULLRES_H),
            "--fps", str(FPS),
            "--quality", "3",
            "--pixel-format", "4",
        ]
        r = subprocess.run(gvid_cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"gvid_pack FAILED:\n{r.stderr[-400:]}")
        else:
            gvid_s = time.time() - t_gvid
            print(f"GVID assembled: {gvid.stat().st_size / 1024 / 1024:.1f} MB "
                  f"in {gvid_s:.1f}s ({gvid_s*1000/args.n_frames:.1f} ms/frame amortized)")

        # Compatibility artifact: MOV wrapper for gpr2prores / patched FFmpeg.
        gvid_mov = OUT / "upresable_timelapse.gpr1.mov"
        print(f"\nPacking MOV compatibility wrapper from {args.n_frames} full-res .gpr frames...")
        t_pack = time.time()
        gpr_mov_tool = REPO / "tools/gpr2prores/gpr_mov_tool"
        cmd = [str(gpr_mov_tool), "pack", str(OUT / "fullres"),
               str(gvid_mov), "--fps", str(FPS)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"gpr_mov_tool pack FAILED:\n{r.stderr[-400:]}")
        else:
            pack_s = time.time() - t_pack
            print(f"MOV wrapper assembled: {gvid_mov.stat().st_size / 1024 / 1024:.1f} MB "
                  f"in {pack_s:.1f}s ({pack_s*1000/args.n_frames:.1f} ms/frame amortized)")

        # Optional ProRes review (off by default; opt-in via --render-prores).
        if args.render_prores:
            print(f"\nAssembling ProRes review from {args.n_frames} frames...")
            cmd = ["ffmpeg", "-y",
                   "-framerate", str(FPS),
                   "-i", str(ffmpeg_seq_dir / f"frame_%05d{FRAME_EXT}"),
                   "-sws_dither", "auto",
                   "-c:v", "prores_ks", "-profile:v", "3",
                   "-pix_fmt", "yuv422p10le",
                   "-vendor", "apl0",
                   "-movflags", "+faststart",
                   str(mov)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"ffmpeg FAILED:\n{r.stderr[-800:]}")
                return
            print(f"ProRes review: {mov.stat().st_size / 1024 / 1024:.1f} MB")

    # === SUMMARY ===
    print("\n=== SUMMARY ===")
    if regression_results:
        print("\nRegression (4 gate images):")
        for img, r in regression_results.items():
            print(f"  {img}: halfres={r['halfres_gpr_MB']:.2f} MB  fullres_FUSED={r['fullres_FUSED_gpr_MB']:.2f} MB  "
                  f"editable_DNG={r['editable_DNG_MB']:.2f} MB  editable_GPR={r['editable_GPR_MB']:.2f} MB  "
                  f"bayer_PSNR={r['bayer_psnr_vs_source_dB']:.2f} dB  DNG_opens={r['dng_opens_in_raw_editor']}")
    if stats["halfres_gpr_bytes"]:
        def med(xs): return float(np.median(xs)) if xs else 0.0
        print(f"\nTimelapse stats (median of {len(stats['halfres_gpr_bytes'])} frames):")
        print(f"  halfres .gpr  : {med(stats['halfres_gpr_bytes'])/1e6:.2f} MB/frame")
        print(f"  fullres .gpr  : {med(stats['fullres_gpr_bytes'])/1e6:.2f} MB/frame")
        print(f"  BIBO_2x CNN   : {med(stats['bibo2x_ms']):.0f} ms/frame  (MPS, batched-32)")
        print(f"  encode fullres: {med(stats['encode_full_ms']):.0f} ms/frame")
        if stats['render_ms']:
            print(f"  render+DNG    : {med(stats['render_ms']):.0f} ms/frame")
        total = (med(stats['bibo2x_ms']) + med(stats['encode_full_ms']) + med(stats['render_ms']))
        print(f"  total/frame   : {total:.0f} ms")

    summary_file = OUT / "summary.json"
    summary_file.write_text(json.dumps({
        "regression": regression_results,
        "timelapse_stats": {
            "halfres_gpr_mb_median": float(np.median(stats['halfres_gpr_bytes']) / 1e6) if stats['halfres_gpr_bytes'] else None,
            "fullres_gpr_mb_median": float(np.median(stats['fullres_gpr_bytes']) / 1e6) if stats['fullres_gpr_bytes'] else None,
            "bibo2x_ms_median":      float(np.median(stats['bibo2x_ms'])) if stats['bibo2x_ms'] else None,
            "encode_full_ms_median": float(np.median(stats['encode_full_ms'])) if stats['encode_full_ms'] else None,
            "render_ms_median":      float(np.median(stats['render_ms'])) if stats['render_ms'] else None,
            "total_ms_median":       float(np.median([a + b + (c if stats['render_ms'] else 0) for a, b, c in zip(
                                          stats['bibo2x_ms'], stats['encode_full_ms'],
                                          stats['render_ms'] or [0]*len(stats['bibo2x_ms']))])) if stats['bibo2x_ms'] else None,
            "n_frames": len(stats['halfres_gpr_bytes']),
            "deliverable":           "gvid" if not args.render_prores else "gvid+prores",
            "mov_compatibility":     True,
            "dng_exported":          bool(args.dng_export),
        },
    }, indent=2, default=lambda o: float(o) if hasattr(o, "item") else str(o)))
    shutil.rmtree(work_root, ignore_errors=True)
    print(f"\nsummary saved: {summary_file}")
    print(f"GVID: {OUT / 'upresable_timelapse.gvid'}")
    print(f"MOV compatibility: {OUT / 'upresable_timelapse.gpr1.mov'}")
    print(f"ProRes review: {OUT / 'upresable_timelapse.mov'}")


if __name__ == "__main__":
    main()
