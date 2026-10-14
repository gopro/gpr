#!/usr/bin/env python3
"""Regression tests for CFA-preserving Bayer resampling."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

from bayer_resample import cfa_downsample_2x, cfa_downsample_2x_area  # noqa: E402


def make_plane_separated_source() -> np.ndarray:
    src = np.zeros((8, 8), dtype=np.uint16)
    for y in range(8):
        for x in range(8):
            if y % 2 == 0 and x % 2 == 0:
                base = 1000  # R
            elif y % 2 == 0 and x % 2 == 1:
                base = 2000  # G1
            elif y % 2 == 1 and x % 2 == 0:
                base = 3000  # G2
            else:
                base = 4000  # B
            src[y, x] = base + y * 10 + x
    return src


def expected_same_plane_average(src: np.ndarray) -> np.ndarray:
    out = np.empty((4, 4), dtype=np.uint16)
    for y in range(4):
        for x in range(4):
            sy = y % 2
            sx = x % 2
            block_y = (y // 2) * 4 + sy
            block_x = (x // 2) * 4 + sx
            vals = [
                int(src[block_y, block_x]),
                int(src[block_y, block_x + 2]),
                int(src[block_y + 2, block_x]),
                int(src[block_y + 2, block_x + 2]),
            ]
            out[y, x] = (sum(vals) + 2) >> 2
    return out


def main() -> int:
    src = make_plane_separated_source()
    got = cfa_downsample_2x_area(src)
    want = expected_same_plane_average(src)
    if not np.array_equal(got, want):
        print("same-plane 2x area average mismatch", file=sys.stderr)
        print("got:\n", got, file=sys.stderr)
        print("want:\n", want, file=sys.stderr)
        return 1

    if got[0, 0] // 1000 != 1 or got[0, 1] // 1000 != 2:
        print("top-row CFA phase was not preserved", file=sys.stderr)
        return 1
    if got[1, 0] // 1000 != 3 or got[1, 1] // 1000 != 4:
        print("bottom-row CFA phase was not preserved", file=sys.stderr)
        return 1

    hq = cfa_downsample_2x(src, mode="gaussian_area")
    if hq.shape != (4, 4) or hq.dtype != np.uint16:
        print(f"gaussian_area shape/dtype mismatch: {hq.shape} {hq.dtype}", file=sys.stderr)
        return 1
    if hq[0, 0] // 1000 != 1 or hq[0, 1] // 1000 != 2 or hq[1, 0] // 1000 != 3 or hq[1, 1] // 1000 != 4:
        print("gaussian_area mixed CFA planes", file=sys.stderr)
        return 1
    const = np.zeros((8, 8), dtype=np.uint16)
    const[0::2, 0::2] = 1111
    const[0::2, 1::2] = 2222
    const[1::2, 0::2] = 3333
    const[1::2, 1::2] = 4444
    const_hq = cfa_downsample_2x(const, mode="gaussian_area")
    if not np.array_equal(const_hq, const[:4, :4]):
        print("gaussian_area did not preserve constant same-plane values", file=sys.stderr)
        print(const_hq, file=sys.stderr)
        return 1

    impulse = np.zeros((24, 24), dtype=np.uint16)
    impulse[0::2, 0::2] = 1000
    impulse[0::2, 1::2] = 2000
    impulse[1::2, 0::2] = 3000
    impulse[1::2, 1::2] = 4000
    impulse[8, 8] = 17000
    impulse_area = cfa_downsample_2x(impulse, mode="area")
    impulse_hq = cfa_downsample_2x(impulse, mode="gaussian_area")
    if impulse_hq[4, 4] >= impulse_area[4, 4]:
        print("gaussian_area anti-aliasing did not attenuate same-plane impulse", file=sys.stderr)
        print("area center", impulse_area[4, 4], "hq center", impulse_hq[4, 4], file=sys.stderr)
        return 1
    if impulse_hq[2, 2] <= 1000 or impulse_hq[6, 6] <= 1000:
        print("gaussian_area anti-aliasing did not spread same-plane impulse", file=sys.stderr)
        print(impulse_hq[0::2, 0::2], file=sys.stderr)
        return 1
    if (
        impulse_hq[0, 1] != 2000
        or impulse_hq[1, 0] != 3000
        or impulse_hq[1, 1] != 4000
    ):
        print("gaussian_area anti-aliasing touched other CFA planes", file=sys.stderr)
        return 1

    try:
        cfa_downsample_2x_area(np.zeros((6, 8), dtype=np.uint16))
    except ValueError:
        pass
    else:
        print("expected non-divisible-by-4 height to fail", file=sys.stderr)
        return 1

    work_parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
    work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bayer_resample_", dir=work_parent) as td:
        work = Path(td)
        raw_in = work / "in.raw"
        raw_out = work / "out.raw"
        src.astype("<u2", copy=False).tofile(raw_in)
        subprocess.run(
            [
                sys.executable,
                str(TOOLS / "bayer_resample.py"),
                str(raw_in),
                str(raw_out),
                "--width",
                "8",
                "--height",
                "8",
                "--mode",
                "area",
            ],
            cwd=ROOT,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        cli = np.fromfile(raw_out, dtype="<u2").reshape((4, 4))
        if not np.array_equal(cli, want):
            print("CLI output mismatch", file=sys.stderr)
            return 1

    print("test_bayer_resample: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
