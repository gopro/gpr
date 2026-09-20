#!/usr/bin/env python3
"""Fit DNG WarpRectilinear radial coefficients for a fisheye-to-rectilinear correction.

GoPro lenses are close to an equidistant fisheye (r_captured = f * theta), while a
corrected image is rectilinear (r_corrected = f * tan(theta)). A DNG OpcodeList3
WarpRectilinear maps each destination (corrected) radius to the source (captured)
radius it should sample:

    r_src = r_dst * (k0 + k1*r_dst^2 + k2*r_dst^4 + k3*r_dst^6)

with radii normalized by the distance from the optical center to the farthest
corner. For the equidistant model the exact mapping is

    r_src = f_n * atan(r_dst / f_n),      f_n = focal_length_px / max_dist_px

This script least-squares fits k0..k3 to that curve over r in (0, 1] so the fitted
polynomial can be pasted into gpr_lens_profiles.cpp or passed to
gpr_tools --lens_correction=k0,k1,k2,k3.

Usage: fit_warp_rectilinear.py <f_norm> [strength]

    f_norm    normalized focal length (e.g. 0.75; smaller = wider lens = stronger correction)
    strength  0..1 blend toward identity, default 1.0 (full rectilinear correction)

Alternative: convert an OpenCV-fisheye calibration (e.g. a Gyroflow lens profile):

    fit_warp_rectilinear.py --opencv-fisheye <fx> <cx> <cy> <w> <h> <k1> <k2> <k3> <k4>

OpenCV fisheye: theta_d = theta*(1 + k1*theta^2 + k2*theta^4 + k3*theta^6 + k4*theta^8),
r_px = fx*theta_d. The destination image is treated as rectilinear with the same focal
length (r_dst_px = fx*tan(theta)); both radii are normalized by the distance from the
principal point to the farthest corner of the calibration frame. When the photo mode
uses a different active area than the calibrated video mode but the same pixel pitch,
rescale fx by width_photo/width_calib and pass the photo dimensions as w/h.
"""

import math
import sys


def solve4(a, b):
    """Gaussian elimination for a 4x4 system (no numpy dependency)."""
    n = 4
    m = [row[:] + [rhs] for row, rhs in zip(a, b)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(m[r][col]))
        m[col], m[piv] = m[piv], m[col]
        for r in range(col + 1, n):
            fac = m[r][col] / m[col][col]
            for c in range(col, n + 1):
                m[r][c] -= fac * m[col][c]
    x = [0.0] * n
    for r in range(n - 1, -1, -1):
        x[r] = (m[r][n] - sum(m[r][c] * x[c] for c in range(r + 1, n))) / m[r][r]
    return x


def fit(f_norm, strength=1.0):
    def g(r):
        full = f_norm * math.atan(r / f_norm) / r  # r_src / r_dst for the exact model
        return 1.0 + strength * (full - 1.0)

    return fit_curve(g)


def fit_curve(g, samples=2000):
    """Least-squares fit k0..k3 of k0 + k1 r^2 + k2 r^4 + k3 r^6 to g(r) over (0, 1]."""
    rs = [(i + 0.5) / samples for i in range(samples)]
    basis = [[1.0, r * r, r ** 4, r ** 6] for r in rs]
    ata = [[sum(row[i] * row[j] for row in basis) for j in range(4)] for i in range(4)]
    atb = [sum(row[i] * g(r) for row, r in zip(basis, rs)) for i in range(4)]
    k = solve4(ata, atb)
    err = max(abs((k[0] + k[1] * r * r + k[2] * r ** 4 + k[3] * r ** 6) - g(r)) for r in rs)
    return k, err


def opencv_fisheye(argv):
    fx, cx, cy, w, h, k1, k2, k3, k4 = (float(v) for v in argv)

    max_dist = max(math.hypot(x - cx, y - cy) for x in (0, w) for y in (0, h))
    f_n = fx / max_dist

    def g(r):
        theta = math.atan(r / f_n)
        theta_d = theta * (1 + k1 * theta ** 2 + k2 * theta ** 4 + k3 * theta ** 6 + k4 * theta ** 8)
        return f_n * theta_d / r  # r_src / r_dst, both normalized by max_dist

    k, err = fit_curve(g)
    corner = sum(k)
    print(f"opencv-fisheye: f_norm={f_n:.4f} center=({cx / w:.4f},{cy / h:.4f})")
    print(f"k0..k3 = {k[0]:.6f},{k[1]:.6f},{k[2]:.6f},{k[3]:.6f}")
    print(f"max fit error: {err:.2e}   corner scale (r_src at r_dst=1): {corner:.4f}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    if sys.argv[1] == "--opencv-fisheye":
        if len(sys.argv) != 11:
            print(__doc__)
            sys.exit(1)
        opencv_fisheye(sys.argv[2:])
        return

    f_norm = float(sys.argv[1])
    strength = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0

    k, err = fit(f_norm, strength)
    corner = k[0] + k[1] + k[2] + k[3]
    print(f"f_norm={f_norm} strength={strength}")
    print(f"k0..k3 = {k[0]:.6f},{k[1]:.6f},{k[2]:.6f},{k[3]:.6f}")
    print(f"max fit error: {err:.2e}   corner scale (r_src at r_dst=1): {corner:.4f}")


if __name__ == "__main__":
    main()
