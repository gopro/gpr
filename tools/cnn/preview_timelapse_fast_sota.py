"""Optimized SOTA-v2 timelapse pipeline with MPS + parallel workers.

Architecture:
  - Worker pool (4-6 processes) handles per-frame codec + sips path:
      DNG -> coeff_io_tool -> half-res bayer -> gpr_tools+sips -> cnn=none RGB
    Workers return (frame_idx, half_bayer, cnn_none_rgb) as numpy arrays.
  - Main process holds the VA Y CNN on Apple Silicon MPS GPU.
    Receives worker results, runs CNN in batched tile mode (32 tiles per
    batch ~  173ms on MPS = 7.7x faster than CPU per tile).
  - Main process assembles SOTA-v2 (unsharp + YCbCr-swap) and streams
    raw RGB to ffmpeg via stdin (no per-frame PNG save).

Expected: 1-2 sec/frame end-to-end vs 35+ sec/frame in the unoptimized
version. 120-frame SOTA-v2 timelapse in ~2-4 min.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import tempfile
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from pathlib import Path
import numpy as np
from PIL import Image
import cv2
import tifffile
import torch



sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(os.environ.get("GPR_REPO", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(REPO / "tools/cnn"))
from model import build  # type: ignore

CODEC_BIN = Path(os.environ.get("GPR_CODEC_BIN", REPO / "build-local/bin/coeff_io_tool"))
GPR_TOOLS = Path(os.environ.get("GPR_TOOLS_BIN", REPO / "build-local/source/app/gpr_tools/gpr_tools"))
Y_CKPT = Path(os.environ.get("GPR_SOTA_Y_CKPT", REPO / "models/F_ane_no_sr_w16_y.pt"))

SRC_DIR = Path(os.environ.get("GPR_TIMELAPSE_SRC", str(external_path('barnsky_full_dngs'))))
OUT_DIR = Path(os.environ.get("GPR_TIMELAPSE_OUT", str(external_path('artifacts/preview_timelapse'))))

TARGET_W = 3840
TARGET_H = 2160
FPS = 24

TILE = 128
OUT_TILE = 512
OVERLAP = 16
RAW_NORM = 16383.0
BATCH = 32

Image.MAX_IMAGE_PIXELS = None


def resize_center_crop_rgb(rgb: np.ndarray, target_w: int, target_h: int) -> np.ndarray:
    """Resize to target aspect without geometric stretch."""
    h, w = rgb.shape[:2]
    src_aspect = w / h
    dst_aspect = target_w / target_h
    if src_aspect > dst_aspect:
        crop_w = int(round(h * dst_aspect))
        x0 = max((w - crop_w) // 2, 0)
        rgb = rgb[:, x0:x0 + crop_w]
    elif src_aspect < dst_aspect:
        crop_h = int(round(w / dst_aspect))
        y0 = max((h - crop_h) // 2, 0)
        rgb = rgb[y0:y0 + crop_h, :]
    return cv2.resize(rgb, (target_w, target_h), interpolation=cv2.INTER_LANCZOS4)


def get_cached_params(dng, work):
    cache = Path(work) / "params_shared.json"
    if not cache.exists():
        cp = subprocess.run([str(GPR_TOOLS), "-i", str(dng), "-d", "1"],
                            capture_output=True, text=True)
        if cp.returncode != 0:
            raise RuntimeError(f"params dump failed: {cp.stderr[-200:]}")
        lines = [l for l in cp.stdout.splitlines() if not l.startswith("[")]
        cache.write_text("\n".join(lines))
    return json.loads(cache.read_text())


def worker_one_frame(args):
    """Worker: codec + sips + cnn=none render. Returns (idx, half_bayer_u16, none_rgb).
    Default: none_rgb is uint16. Use eight_bit=True for legacy uint8 (banding-prone)."""
    frame_idx, dng_path, work_root, params_cached, eight_bit = args
    dng = Path(dng_path)
    work = Path(work_root) / f"w_{frame_idx}"
    work.mkdir(exist_ok=True)

    with tifffile.TiffFile(dng) as tf:
        bayer = tf.pages[0].pages[0].asarray().astype('<u2')
    h, w = bayer.shape

    in_raw = work / "in.raw"
    bayer.tofile(in_raw)
    half_raw = work / "half.raw"
    env = os.environ.copy()
    env.update({"FUSED_QUALITY":"3","FUSED_MULTI_LEVEL":"1","FUSED_WAVELET_LEVELS":"2",
                "GPR_COL_DECIMATE":"2","GPR_ROW_DECIMATE":"2","GPR_INCLUDE_LL":"1"})
    r = subprocess.run([str(CODEC_BIN), str(in_raw), str(w), str(h), str(half_raw)],
                       capture_output=True, env=env)
    if r.returncode != 0:
        raise RuntimeError(f"codec failed on {dng.name}: {r.stderr[-200:]}")
    half_bayer = np.fromfile(half_raw, dtype=np.uint16).reshape(h // 2, w // 2)

    hb, wb = half_bayer.shape
    params = dict(params_cached)
    params["input_width"]=wb; params["input_height"]=hb; params["input_pitch"]=wb*2
    params_path = work / "params.json"
    params_path.write_text(json.dumps(params))
    raw_in = work / "bayer_half.raw"
    half_bayer.astype("<u2").tofile(raw_in)
    dng_out = work / "halfres.dng"
    r = subprocess.run([str(GPR_TOOLS), "-i", str(raw_in), "-w", str(wb), "-h", str(hb),
                        "-x", "rggb14", "-o", str(dng_out), "-a", str(params_path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"gpr_tools failed: {r.stderr[-200:]}")

    if eight_bit:
        out_path = work / "rendered.png"
        r = subprocess.run(["sips", "-s", "format", "png", str(dng_out), "--out", str(out_path)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"sips failed: {r.stderr[-200:]}")
        none_rgb = np.array(Image.open(out_path).convert("RGB"), dtype=np.uint8)
    else:
        out_path = work / "rendered.tiff"
        r = subprocess.run(["sips", "-s", "format", "tiff", str(dng_out), "--out", str(out_path)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"sips failed: {r.stderr[-200:]}")
        arr = cv2.imread(str(out_path), cv2.IMREAD_UNCHANGED)   # BGR uint16
        if arr is None or arr.dtype != np.uint16:
            raise RuntimeError(f"sips TIFF not uint16: {out_path}")
        none_rgb = cv2.cvtColor(arr, cv2.COLOR_BGR2RGB)   # uint16 RGB
    shutil.rmtree(work, ignore_errors=True)
    return (frame_idx, half_bayer, none_rgb)


def deinterleave_bayer(bayer):
    R  = bayer[0::2, 0::2]; G1 = bayer[0::2, 1::2]
    G2 = bayer[1::2, 0::2]; B  = bayer[1::2, 1::2]
    return R, G1, G2, B


def build_tile_stack(half_bayer):
    """Pre-compute the tile coords + 4-channel tile tensors for this image.
    Returns (tile_tensors_stack, tile_coords)."""
    R, G1, G2, B = deinterleave_bayer(half_bayer)
    H_in, W_in = R.shape
    step = TILE - OVERLAP
    n_y = (H_in + step - 1) // step
    n_x = (W_in + step - 1) // step
    tiles = []
    coords = []
    for ty in range(n_y):
        for tx in range(n_x):
            y0 = min(ty * step, H_in - TILE)
            x0 = min(tx * step, W_in - TILE)
            R_t  = R [y0:y0+TILE, x0:x0+TILE].astype(np.float32) / RAW_NORM
            G1_t = G1[y0:y0+TILE, x0:x0+TILE].astype(np.float32) / RAW_NORM
            G2_t = G2[y0:y0+TILE, x0:x0+TILE].astype(np.float32) / RAW_NORM
            B_t  = B [y0:y0+TILE, x0:x0+TILE].astype(np.float32) / RAW_NORM
            tiles.append(np.stack([R_t, G1_t, G2_t, B_t]))
            coords.append((y0, x0))
    return np.stack(tiles), coords, (H_in*4, W_in*4)


_taper_cache = None
def taper2d():
    global _taper_cache
    if _taper_cache is not None: return _taper_cache
    def taper_1d(n, ov):
        if ov <= 0: return np.ones(n, dtype=np.float32)
        w = np.ones(n, dtype=np.float32)
        ramp = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, ov))
        w[:ov] = ramp; w[-ov:] = ramp[::-1]
        return w
    _taper_cache = np.outer(taper_1d(OUT_TILE, OVERLAP*4), taper_1d(OUT_TILE, OVERLAP*4)).astype(np.float32)
    return _taper_cache


def run_cnn_mps_batched(model, half_bayer, device, eight_bit=False):
    """Run VA Y CNN on MPS in batches of size BATCH. Returns full-res Y.
    Default: uint16 [0, 65535]. With eight_bit=True: legacy uint8 [0, 255]."""
    tiles_np, coords, (H_out, W_out) = build_tile_stack(half_bayer)
    n_tiles = tiles_np.shape[0]
    out = np.zeros((H_out, W_out), dtype=np.float32)
    weight = np.zeros((H_out, W_out), dtype=np.float32)
    tap = taper2d()

    tiles_t = torch.from_numpy(tiles_np).to(device, non_blocking=True)
    preds = []
    with torch.no_grad():
        for i in range(0, n_tiles, BATCH):
            b = tiles_t[i:i+BATCH]
            p = model(b).cpu().numpy()
            preds.append(p)
    preds = np.concatenate(preds, axis=0)
    if preds.ndim == 4:
        preds = preds.squeeze(1)

    for i, (y0, x0) in enumerate(coords):
        pred_clip = np.clip(preds[i], 0, 1).astype(np.float32)
        oy0 = y0 * 4; ox0 = x0 * 4
        out[oy0:oy0+OUT_TILE, ox0:ox0+OUT_TILE] += pred_clip * tap
        weight[oy0:oy0+OUT_TILE, ox0:ox0+OUT_TILE] += tap
    weight = np.maximum(weight, 1e-6)
    y_unit = out / weight   # [0, 1] float
    if eight_bit:
        return np.clip(y_unit * 255.0, 0, 255).astype(np.uint8)
    return np.clip(y_unit * 65535.0, 0, 65535).astype(np.uint16)


def unsharp(y, amount, sigma):
    """Unsharp on Y channel. Works with uint8 or uint16 (preserves dtype)."""
    yf = y.astype(np.float32)
    k = int(2 * round(3 * sigma) + 1)
    sharpened = yf + amount * (yf - cv2.GaussianBlur(yf, (k, k), sigma))
    if y.dtype == np.uint16:
        return np.clip(sharpened, 0, 65535).astype(np.uint16)
    return np.clip(sharpened, 0, 255).astype(np.uint8)


def sota_v2_assemble(va_y_full, none_rgb_half):
    """SOTA-v2: VA Y (full-res) + unsharp + YCbCr-swap with cnn=none chroma.
    The cnn=none input here is HALF-RES; we resize chroma up to match Y.
    Auto-detects bit depth from inputs (preserves uint16 throughout when given uint16)."""
    H, W = va_y_full.shape
    if none_rgb_half.shape[:2] != (H, W):
        none_full = cv2.resize(none_rgb_half, (W, H), interpolation=cv2.INTER_LANCZOS4)
    else:
        none_full = none_rgb_half
    # Match bit depth between Y and chroma source
    if va_y_full.dtype != none_full.dtype:
        if va_y_full.dtype == np.uint16 and none_full.dtype == np.uint8:
            none_full = (none_full.astype(np.uint16) * 257)   # u8 -> u16
        elif va_y_full.dtype == np.uint8 and none_full.dtype == np.uint16:
            none_full = (none_full // 257).astype(np.uint8)   # u16 -> u8
    sharp_y = unsharp(va_y_full, 0.3, 3.0)
    none_ycc = cv2.cvtColor(none_full, cv2.COLOR_RGB2YCrCb)
    out_ycc = none_ycc.copy()
    out_ycc[..., 0] = sharp_y
    return cv2.cvtColor(out_ycc, cv2.COLOR_YCrCb2RGB)


def main():
    global REPO, CODEC_BIN, GPR_TOOLS, Y_CKPT, SRC_DIR, OUT_DIR
    ap = argparse.ArgumentParser()
    ap.add_argument("--n-frames", type=int, default=120)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--max-in-flight", type=int, default=0,
                    help="Bound queued frame jobs; default is workers*2.")
    ap.add_argument("--max-buffered-frames", type=int, default=0,
                    help="Bound completed frames waiting for ordered write; default matches max-in-flight.")
    ap.add_argument("--repo", type=Path, default=REPO)
    ap.add_argument("--codec-bin", type=Path, default=CODEC_BIN)
    ap.add_argument("--gpr-tools", type=Path, default=GPR_TOOLS)
    ap.add_argument("--y-ckpt", type=Path, default=Y_CKPT)
    ap.add_argument("--src-dir", type=Path, default=SRC_DIR)
    ap.add_argument("--out-dir", type=Path, default=OUT_DIR)
    ap.add_argument("--eight-bit", action="store_true",
                    help="Render frames as 8-bit (legacy; causes sky banding). "
                         "Default: 16-bit throughout (uint16 CNN output, rgb48le ffmpeg).")
    args = ap.parse_args()
    REPO = args.repo
    CODEC_BIN = args.codec_bin
    GPR_TOOLS = args.gpr_tools
    Y_CKPT = args.y_ckpt
    SRC_DIR = args.src_dir
    OUT_DIR = args.out_dir
    os.environ.update({
        "GPR_REPO": str(REPO),
        "GPR_CODEC_BIN": str(CODEC_BIN),
        "GPR_TOOLS_BIN": str(GPR_TOOLS),
        "GPR_SOTA_Y_CKPT": str(Y_CKPT),
        "GPR_TIMELAPSE_SRC": str(SRC_DIR),
        "GPR_TIMELAPSE_OUT": str(OUT_DIR),
    })
    EIGHT_BIT = args.eight_bit
    FFMPEG_PIXFMT_IN = "rgb24" if EIGHT_BIT else "rgb48le"

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    mode_dir = OUT_DIR / "sota_v2_fast"
    mode_dir.mkdir(parents=True, exist_ok=True)
    work_root = Path(tempfile.mkdtemp(prefix="timelapse_sota_", dir=mode_dir))

    all_dngs = sorted(SRC_DIR.glob("*.dng"))
    selected = all_dngs[:args.n_frames]
    if not selected:
        raise SystemExit(f"No DNG frames found in {SRC_DIR}")
    print(f"Selected {len(selected)} frames ({selected[0].name} -> {selected[-1].name})")

    # Cache params once
    params_cached = get_cached_params(selected[0], work_root)

    # Load VA Y CNN onto MPS
    device = "mps" if torch.backends.mps.is_available() else "cpu"
    print(f"CNN device: {device}")
    ckpt = torch.load(str(Y_CKPT), map_location='cpu', weights_only=False)
    variant = ckpt.get('variant', 'F_ane_no_sr_w16_y')
    model = build(variant)
    state = ckpt['backbone_state'] if 'backbone_state' in ckpt else ckpt
    model.load_state_dict(state, strict=True)
    model.eval().to(device)

    # ffmpeg pipe (16-bit default; --eight-bit gates legacy uint8)
    mov = OUT_DIR / "preview_timelapse_sota_v2_fast.mov"
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pix_fmt", FFMPEG_PIXFMT_IN,
           "-s", f"{TARGET_W}x{TARGET_H}",
           "-framerate", str(FPS),
           "-i", "-",
           "-sws_dither", "auto",
           "-c:v", "prores_ks", "-profile:v", "3",
           "-pix_fmt", "yuv422p10le",
           "-vendor", "apl0",
           str(mov)]
    ffmpeg_log = work_root / "ffmpeg.stderr.log"
    ffmpeg_err = ffmpeg_log.open("wb")
    ffmpeg = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=ffmpeg_err)
    print(f"ffmpeg input: {FFMPEG_PIXFMT_IN} ({'8-bit RGB' if EIGHT_BIT else '16-bit RGB'})")

    frames_args = [(i, str(d), str(work_root), params_cached, EIGHT_BIT) for i, d in enumerate(selected)]
    completed_buffer = {}
    next_to_write = 0
    overall_t0 = time.time()
    completed = 0

    max_in_flight = args.max_in_flight if args.max_in_flight > 0 else args.workers * 2
    max_in_flight = max(1, max_in_flight)
    max_buffered_frames = args.max_buffered_frames if args.max_buffered_frames > 0 else max_in_flight
    max_buffered_frames = max(1, max_buffered_frames)

    # Worker pool runs the codec+sips path; main runs the CNN
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        frame_iter = iter(frames_args)
        futures = {}

        def submit_available() -> None:
            while len(futures) < max_in_flight and len(completed_buffer) < max_buffered_frames:
                try:
                    fa = next(frame_iter)
                except StopIteration:
                    return
                futures[ex.submit(worker_one_frame, fa)] = fa[0]

        submit_available()

        while futures:
            done, _ = wait(futures, return_when=FIRST_COMPLETED)
            for fut in done:
                futures.pop(fut)
                idx, half_bayer, none_rgb = fut.result()
                # Run CNN on MPS for this frame
                t_cnn = time.time()
                va_y_full = run_cnn_mps_batched(model, half_bayer, device, eight_bit=EIGHT_BIT)
                cnn_ms = (time.time() - t_cnn) * 1000
                # Assemble SOTA-v2 (preserves bit depth from inputs)
                sota = sota_v2_assemble(va_y_full, none_rgb)
                # Resize to 4K UHD without changing geometry (cv2 preserves dtype)
                rgb_4k = resize_center_crop_rgb(sota, TARGET_W, TARGET_H)
                # Serialize for ffmpeg: rgb24 (uint8) or rgb48le (uint16)
                if rgb_4k.dtype == np.uint16:
                    completed_buffer[idx] = rgb_4k.astype('<u2').tobytes()
                else:
                    completed_buffer[idx] = rgb_4k.tobytes()
                completed += 1
                # Write frames in strict order
                while next_to_write in completed_buffer:
                    ffmpeg.stdin.write(completed_buffer.pop(next_to_write))
                    next_to_write += 1
                submit_available()
                if completed % 10 == 0:
                    rate = completed / (time.time() - overall_t0)
                    rem = (len(selected) - completed) / max(rate, 1e-6)
                    print(f"  {completed}/{len(selected)} done  (last CNN: {cnn_ms:.0f}ms, {rate:.2f} f/s, est rem {rem:.0f}s)")

    ffmpeg.stdin.close()
    ffmpeg.wait()
    ffmpeg_err.close()
    if ffmpeg.returncode != 0:
        log_tail = ffmpeg_log.read_text(errors="replace")[-500:] if ffmpeg_log.exists() else ""
        print(f"ffmpeg ERROR:\n{log_tail}")
        return

    total = time.time() - overall_t0
    print(f"\nDONE in {total:.1f}s ({total/len(selected):.2f} sec/frame).")
    print(f"  ProRes 422 HQ: {mov}")
    print(f"  size: {mov.stat().st_size / 1024 / 1024:.1f} MB")
    print(f"  open with: open {mov}")
    shutil.rmtree(work_root, ignore_errors=True)


if __name__ == "__main__":
    main()
