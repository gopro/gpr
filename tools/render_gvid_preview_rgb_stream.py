#!/usr/bin/env python3
"""Stream a .gvid screen-preview decode as RGB24 frames.

The script is stdlib-only so it can run on the Pi target. It extracts one
.gvid payload at a time, decodes it with fused_decode_cli using the
mission1_preview_4x_1024x768 target, converts the Bayer preview to a simple
RGB review image, writes RGB24 bytes to stdout, and removes temporaries.
"""
from __future__ import annotations

import argparse
import array
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

from gvid_metadata import read_gvid_frames


def extract_payload(gvid_file, frame: dict[str, int], output: Path) -> None:
    gvid_file.seek(int(frame["payload_offset"]))
    remaining = int(frame["payload_size"])
    with output.open("wb") as dst:
        while remaining:
            chunk = gvid_file.read(min(1024 * 1024, remaining))
            if not chunk:
                raise EOFError("unexpected EOF extracting .gvid frame")
            dst.write(chunk)
            remaining -= len(chunk)


def percentile_from_hist(hist: list[int], total: int, pct: float) -> int:
    want = max(0, min(total - 1, int(total * pct + 0.5)))
    seen = 0
    for idx, count in enumerate(hist):
        seen += count
        if seen > want:
            return idx
    return len(hist) - 1


def invert_3x3(m: tuple[float, ...]) -> tuple[float, ...]:
    a, b, c, d, e, f, g, h, i = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-12:
        raise ValueError("singular color matrix")
    inv_det = 1.0 / det
    return (
        (e * i - f * h) * inv_det,
        (c * h - b * i) * inv_det,
        (b * f - c * e) * inv_det,
        (f * g - d * i) * inv_det,
        (a * i - c * g) * inv_det,
        (c * d - a * f) * inv_det,
        (d * h - e * g) * inv_det,
        (b * g - a * h) * inv_det,
        (a * e - b * d) * inv_det,
    )


def matmul_3x3(a: tuple[float, ...], b: tuple[float, ...]) -> tuple[float, ...]:
    return tuple(
        a[r * 3 + 0] * b[0 * 3 + c] +
        a[r * 3 + 1] * b[1 * 3 + c] +
        a[r * 3 + 2] * b[2 * 3 + c]
        for r in range(3)
        for c in range(3)
    )


def matvec_3(m: tuple[float, ...], r: float, g: float, b: float) -> tuple[float, float, float]:
    return (
        m[0] * r + m[1] * g + m[2] * b,
        m[3] * r + m[4] * g + m[5] * b,
        m[6] * r + m[7] * g + m[8] * b,
    )


# Mission 1 sample DNG values. ColorMatrix maps XYZ D50 -> camera RGB; invert
# it for camera RGB -> XYZ, then adapt approximately to sRGB display.
MISSION1_AS_SHOT_NEUTRAL = (0.454707, 1.0, 0.453097)
MISSION1_COLOR_MATRIX2 = (
    1.0344, -0.4210, -0.0620,
    -0.2315, 1.0625, 0.1948,
    0.0093, 0.1058, 0.5541,
)
XYZ_TO_SRGB_D65 = (
    3.2404542, -1.5371385, -0.4985314,
    -0.9692660, 1.8760108, 0.0415560,
    0.0556434, -0.2040259, 1.0572252,
)
MISSION1_CAMERA_TO_SRGB = matmul_3x3(XYZ_TO_SRGB_D65, invert_3x3(MISSION1_COLOR_MATRIX2))


def bayer_to_rgb24(raw_path: Path, width: int, height: int, *, color_mode: str) -> bytes:
    raw = array.array("H")
    with raw_path.open("rb") as f:
        raw.fromfile(f, width * height)
    if sys.byteorder != "little":
        raw.byteswap()

    if color_mode == "mission":
        r_gain = 1.0 / MISSION1_AS_SHOT_NEUTRAL[0]
        g_gain = 1.0 / MISSION1_AS_SHOT_NEUTRAL[1]
        b_gain = 1.0 / MISSION1_AS_SHOT_NEUTRAL[2]
    elif color_mode in ("grayworld", "jpegish"):
        r_sum = g_sum = b_sum = 0.0
        blocks = 0
        for y in range(0, height - 1, 2):
            row0 = y * width
            row1 = row0 + width
            for x in range(0, width - 1, 2):
                r_sum += raw[row0 + x]
                g_sum += (raw[row0 + x + 1] + raw[row1 + x]) * 0.5
                b_sum += raw[row1 + x + 1]
                blocks += 1
        r_mean = r_sum / max(blocks, 1)
        g_mean = g_sum / max(blocks, 1)
        b_mean = b_sum / max(blocks, 1)
        r_gain = max(0.25, min(4.0, g_mean / max(r_mean, 1.0)))
        g_gain = 1.0
        b_gain = max(0.25, min(4.0, g_mean / max(b_mean, 1.0)))
    else:
        r_gain = g_gain = b_gain = 1.0

    # 14-bit histogram on block luma after gray-world gains.
    hist = [0] * 16384
    blocks = 0
    for y in range(0, height - 1, 2):
        row0 = y * width
        row1 = row0 + width
        for x in range(0, width - 1, 2):
            cr = raw[row0 + x] * r_gain
            cg = ((raw[row0 + x + 1] + raw[row1 + x]) * 0.5) * g_gain
            cb = raw[row1 + x + 1] * b_gain
            if color_mode == "mission":
                r, g, b = matvec_3(MISSION1_CAMERA_TO_SRGB, cr, cg, cb)
            else:
                r, g, b = cr, cg, cb
            lum = int(max(0.0, min(16383.0, 0.2126 * r + 0.7152 * g + 0.0722 * b)))
            hist[lum] += 1
            blocks += 1
    lo_pct = 0.008 if color_mode == "jpegish" else 0.005
    hi_pct = 0.998 if color_mode == "jpegish" else 0.995
    lo = percentile_from_hist(hist, blocks, lo_pct)
    hi = percentile_from_hist(hist, blocks, hi_pct)
    if hi <= lo + 64:
        lo, hi = 0, 16383
    scale = 1.0 / float(hi - lo)

    out = bytearray(width * height * 3)

    def tone01(v: float) -> float:
        x = (v - lo) * scale
        if x <= 0.0:
            return 0.0
        if x >= 1.0:
            return 1.0
        if color_mode == "jpegish":
            # Preview-display curve: protect highlights, keep blacks anchored,
            # then apply a modest S-curve. This is not a final ISP, just a
            # usable camera-like review look for the fast Bayer preview.
            x = x ** (1.0 / 2.25)
            c = 1.18
            xp = x ** c
            yp = (1.0 - x) ** c
            return xp / (xp + yp + 1.0e-9)
        return x ** (1.0 / 2.2)

    def to_u8(x: float) -> int:
        if x <= 0.0:
            return 0
        if x >= 1.0:
            return 255
        return int(x * 255.0 + 0.5)

    for y in range(0, height - 1, 2):
        row0 = y * width
        row1 = row0 + width
        out0 = row0 * 3
        out1 = row1 * 3
        for x in range(0, width - 1, 2):
            cr = raw[row0 + x] * r_gain
            cg = ((raw[row0 + x + 1] + raw[row1 + x]) * 0.5) * g_gain
            cb = raw[row1 + x + 1] * b_gain
            if color_mode == "mission":
                sr, sg, sb = matvec_3(MISSION1_CAMERA_TO_SRGB, cr, cg, cb)
            else:
                sr, sg, sb = cr, cg, cb
            r = tone01(sr)
            g = tone01(sg)
            b = tone01(sb)
            if color_mode == "jpegish":
                lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
                sat = 1.12
                r = max(0.0, min(1.0, lum + (r - lum) * sat))
                g = max(0.0, min(1.0, lum + (g - lum) * sat))
                b = max(0.0, min(1.0, lum + (b - lum) * sat))
            r8 = to_u8(r)
            g8 = to_u8(g)
            b8 = to_u8(b)
            p0 = out0 + x * 3
            p1 = p0 + 3
            p2 = out1 + x * 3
            p3 = p2 + 3
            out[p0 : p0 + 3] = bytes((r8, g8, b8))
            out[p1 : p1 + 3] = bytes((r8, g8, b8))
            out[p2 : p2 + 3] = bytes((r8, g8, b8))
            out[p3 : p3 + 3] = bytes((r8, g8, b8))
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gvid", type=Path, required=True)
    ap.add_argument("--decoder", type=Path, required=True)
    ap.add_argument("--sensor-width", type=int, default=4096)
    ap.add_argument("--sensor-height", type=int, default=3072)
    ap.add_argument("--target", default="mission1_preview_4x_1024x768")
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--height", type=int, default=768)
    ap.add_argument("--limit", type=int, default=0, help="0 means all frames")
    ap.add_argument("--tmp-dir", type=Path, default=None)
    ap.add_argument(
        "--color-mode",
        choices=("jpegish", "grayworld", "mission", "raw"),
        default="jpegish",
        help="human-review color preview mode; the decoded output remains raw Bayer",
    )
    ap.add_argument("--no-camera-color", action="store_true", help="Deprecated alias for --color-mode raw.")
    args = ap.parse_args()
    if args.no_camera_color:
        args.color_mode = "raw"

    frames = read_gvid_frames(args.gvid)
    if args.limit > 0:
        frames = frames[: args.limit]

    if args.tmp_dir:
        args.tmp_dir.mkdir(parents=True, exist_ok=True)
    tmp_root = Path(tempfile.mkdtemp(prefix="gvid_preview_rgb_", dir=args.tmp_dir))
    payload = tmp_root / "frame.gpr"
    raw = tmp_root / "frame.raw"

    try:
        with args.gvid.open("rb") as gvid_file:
            for i, frame in enumerate(frames, start=1):
                extract_payload(gvid_file, frame, payload)
                proc = subprocess.run(
                    [
                        str(args.decoder),
                        str(payload),
                        str(args.sensor_width),
                        str(args.sensor_height),
                        str(raw),
                        args.target,
                    ],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    text=True,
                    check=False,
                )
                if proc.returncode != 0:
                    raise RuntimeError(proc.stderr[-2000:])
                sys.stdout.buffer.write(
                    bayer_to_rgb24(raw, args.width, args.height, color_mode=args.color_mode)
                )
                payload.unlink(missing_ok=True)
                raw.unlink(missing_ok=True)
                if i % 25 == 0 or i == len(frames):
                    print(f"preview_rgb_stream {i}/{len(frames)}", file=sys.stderr, flush=True)
    finally:
        payload.unlink(missing_ok=True)
        raw.unlink(missing_ok=True)
        try:
            tmp_root.rmdir()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
