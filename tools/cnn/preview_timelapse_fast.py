"""Optimized PREVIEW timelapse pipeline.

Wins over preview_timelapse.py:
  1. Skip the unnecessary 8K intermediate - single Lanczos resize from
     half-res sips output (4140x2760) straight to 4K UHD (3840x2160).
     Saves ~600 ms/frame.
  2. Stream frames to ffmpeg via stdin (raw RGB pipe). No per-frame PNG
     save/load. Saves ~400 ms/frame and ~50 GB of intermediate disk I/O.
  3. Parallel workers via concurrent.futures.ProcessPoolExecutor. Mac
     has 8+ cores so 4-way parallel is conservative. ~4x throughput on
     the subprocess-bound stages.
  4. Cache the params.json once for the whole sequence (all frames are
     from the same camera).

Expected: codec_only at ~0.5-1.0 sec/frame (vs 4.2 baseline). 120 frames
in ~1-2 min.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import time
import tempfile
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
from pathlib import Path
import numpy as np
from PIL import Image
import cv2
import tifffile


import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(os.environ.get("GPR_REPO", Path(__file__).resolve().parents[2]))
CODEC_BIN = Path(os.environ.get("GPR_CODEC_BIN", REPO / "build-local/bin/coeff_io_tool"))
GPR_TOOLS = Path(os.environ.get("GPR_TOOLS_BIN", REPO / "build-local/source/app/gpr_tools/gpr_tools"))

SRC_DIR = Path(os.environ.get("GPR_TIMELAPSE_SRC", str(external_path('barnsky_full_dngs'))))
OUT_DIR = Path(os.environ.get("GPR_TIMELAPSE_OUT", str(external_path('artifacts/preview_timelapse'))))

TARGET_W = 3840
TARGET_H = 2160
FPS = 24
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


def get_cached_params(dng: Path, work: Path) -> dict:
    """Cache params.json once; all timelapse frames share the same camera."""
    cache = work / "params_shared.json"
    if not cache.exists():
        cp = subprocess.run([str(GPR_TOOLS), "-i", str(dng), "-d", "1"],
                            capture_output=True, text=True)
        if cp.returncode != 0:
            raise RuntimeError(f"params dump failed: {cp.stderr[-200:]}")
        lines = [l for l in cp.stdout.splitlines() if not l.startswith("[")]
        cache.write_text("\n".join(lines))
    return json.loads(cache.read_text())


def process_one_frame(args: tuple) -> tuple[int, bytes]:
    """Process one frame end-to-end. Returns (frame_idx, raw_rgb_bytes_4k).
    Designed for ProcessPoolExecutor - all heavy state passed in args.
    Uses 16-bit TIFF rendering by default; 8-bit PNG only via eight_bit=True.
    """
    frame_idx, dng_path, work_root, params_cached, eight_bit = args
    dng = Path(dng_path)
    work = Path(work_root) / f"w_{frame_idx}"
    work.mkdir(exist_ok=True)

    # 1. Read DNG -> Bayer
    with tifffile.TiffFile(dng) as tf:
        bayer = tf.pages[0].pages[0].asarray().astype('<u2')
    h, w = bayer.shape

    # 2. Encode + decode via coeff_io_tool
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

    # 3. Repackage half-res Bayer as DNG (uses cached params)
    hb, wb = half_bayer.shape
    params = dict(params_cached)
    params["input_width"] = wb; params["input_height"] = hb; params["input_pitch"] = wb * 2
    params_path = work / "params.json"
    params_path.write_text(json.dumps(params))
    raw_in = work / "bayer_half.raw"
    half_bayer.astype("<u2").tofile(raw_in)
    dng_out = work / "halfres.dng"
    r = subprocess.run([str(GPR_TOOLS), "-i", str(raw_in), "-w", str(wb), "-h", str(hb),
                        "-x", "rggb14", "-o", str(dng_out), "-a", str(params_path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"gpr_tools repackage failed on {dng.name}: {r.stderr[-200:]}")

    # 4. sips render DNG -> TIFF (16-bit) or PNG (8-bit, legacy)
    if eight_bit:
        out_frame = work / "rendered.png"
        r = subprocess.run(["sips", "-s", "format", "png", str(dng_out), "--out", str(out_frame)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"sips failed on {dng.name}: {r.stderr[-200:]}")
        im = Image.open(out_frame).convert("RGB")
        rgb = np.array(im)   # uint8
        rgb_4k = resize_center_crop_rgb(rgb, TARGET_W, TARGET_H)
        out_bytes = rgb_4k.tobytes()
    else:
        out_frame = work / "rendered.tiff"
        r = subprocess.run(["sips", "-s", "format", "tiff", str(dng_out), "--out", str(out_frame)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"sips failed on {dng.name}: {r.stderr[-200:]}")
        arr = cv2.imread(str(out_frame), cv2.IMREAD_UNCHANGED)   # BGR uint16
        if arr is None or arr.dtype != np.uint16:
            raise RuntimeError(f"sips TIFF not uint16 on {dng.name}")
        rgb = cv2.cvtColor(arr, cv2.COLOR_BGR2RGB)   # uint16 RGB
        rgb_4k = resize_center_crop_rgb(rgb, TARGET_W, TARGET_H)
        out_bytes = rgb_4k.astype('<u2').tobytes()   # rgb48le

    # 5. Clean up the per-frame work dir (we only return bytes)
    shutil.rmtree(work, ignore_errors=True)

    return (frame_idx, out_bytes)


def main():
    global REPO, CODEC_BIN, GPR_TOOLS, SRC_DIR, OUT_DIR
    ap = argparse.ArgumentParser()
    ap.add_argument("--n-frames", type=int, default=120)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--max-in-flight", type=int, default=0,
                    help="Bound queued frame jobs; default is workers*2.")
    ap.add_argument("--max-buffered-frames", type=int, default=0,
                    help="Bound completed frames waiting for ordered write; default matches max-in-flight.")
    ap.add_argument("--mode", choices=["codec_only"], default="codec_only")
    ap.add_argument("--repo", type=Path, default=REPO)
    ap.add_argument("--codec-bin", type=Path, default=CODEC_BIN)
    ap.add_argument("--gpr-tools", type=Path, default=GPR_TOOLS)
    ap.add_argument("--src-dir", type=Path, default=SRC_DIR)
    ap.add_argument("--out-dir", type=Path, default=OUT_DIR)
    ap.add_argument("--eight-bit", action="store_true",
                    help="Render frames as 8-bit (legacy; causes sky banding). "
                         "Default: 16-bit (rgb48le to ffmpeg).")
    args = ap.parse_args()
    REPO = args.repo
    CODEC_BIN = args.codec_bin
    GPR_TOOLS = args.gpr_tools
    SRC_DIR = args.src_dir
    OUT_DIR = args.out_dir
    os.environ.update({
        "GPR_REPO": str(REPO),
        "GPR_CODEC_BIN": str(CODEC_BIN),
        "GPR_TOOLS_BIN": str(GPR_TOOLS),
        "GPR_TIMELAPSE_SRC": str(SRC_DIR),
        "GPR_TIMELAPSE_OUT": str(OUT_DIR),
    })
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    EIGHT_BIT = args.eight_bit
    FFMPEG_PIXFMT_IN = "rgb24" if EIGHT_BIT else "rgb48le"

    mode_dir = OUT_DIR / f"{args.mode}_fast"
    mode_dir.mkdir(parents=True, exist_ok=True)
    work_root = Path(tempfile.mkdtemp(prefix="timelapse_", dir=mode_dir))

    all_dngs = sorted(SRC_DIR.glob("*.dng"))
    selected = all_dngs[:args.n_frames]
    if not selected:
        raise SystemExit(f"No DNG frames found in {SRC_DIR}")
    print(f"Selected {len(selected)} frames ({selected[0].name} -> {selected[-1].name})")
    print(f"Workers: {args.workers}")

    # Cache params once (all frames share)
    print(f"Caching shared params from first DNG...")
    params_cached = get_cached_params(selected[0], work_root)

    # Build ffmpeg pipe (16-bit by default; 8-bit only via --eight-bit)
    mov = OUT_DIR / f"preview_timelapse_{args.mode}_fast.mov"
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", FFMPEG_PIXFMT_IN,
        "-s", f"{TARGET_W}x{TARGET_H}",
        "-framerate", str(FPS),
        "-i", "-",  # stdin
        "-sws_dither", "auto",
        "-c:v", "prores_ks", "-profile:v", "3",  # ProRes 422 HQ
        "-pix_fmt", "yuv422p10le",
        "-vendor", "apl0",
        str(mov),
    ]
    ffmpeg_log = work_root / "ffmpeg.stderr.log"
    ffmpeg_err = ffmpeg_log.open("wb")
    ffmpeg = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=ffmpeg_err)
    print(f"ffmpeg piped to: {mov} (input: {FFMPEG_PIXFMT_IN})")

    # Submit all frames to the pool
    frames_args = [(i, str(d), str(work_root), params_cached, EIGHT_BIT) for i, d in enumerate(selected)]
    overall_t0 = time.time()
    next_to_write = 0
    completed_buffer = {}

    max_in_flight = args.max_in_flight if args.max_in_flight > 0 else args.workers * 2
    max_in_flight = max(1, max_in_flight)
    max_buffered_frames = args.max_buffered_frames if args.max_buffered_frames > 0 else max_in_flight
    max_buffered_frames = max(1, max_buffered_frames)

    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        frame_iter = iter(frames_args)
        futures = {}

        def submit_available() -> None:
            while len(futures) < max_in_flight and len(completed_buffer) < max_buffered_frames:
                try:
                    fa = next(frame_iter)
                except StopIteration:
                    return
                futures[ex.submit(process_one_frame, fa)] = fa[0]

        submit_available()

        completed = 0
        while futures:
            done, _ = wait(futures, return_when=FIRST_COMPLETED)
            for fut in done:
                futures.pop(fut)
                idx, rgb_bytes = fut.result()
                completed += 1
                completed_buffer[idx] = rgb_bytes
                # Write frames in strict order to ffmpeg
                while next_to_write in completed_buffer:
                    ffmpeg.stdin.write(completed_buffer.pop(next_to_write))
                    next_to_write += 1
                submit_available()
                if completed % 10 == 0:
                    rate = completed / (time.time() - overall_t0)
                    rem = (len(selected) - completed) / max(rate, 1e-6)
                    print(f"  {completed}/{len(selected)} done  ({rate:.2f} frames/sec, est remaining {rem:.0f}s)")

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
