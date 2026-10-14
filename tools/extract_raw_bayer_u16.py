#!/usr/bin/env python3
"""Extract visible raw Bayer samples to little-endian uint16 with a receipt."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path
from typing import Any


SCHEMA = "gpr.raw_bayer_u16_extract.v1"
NORMAL_BAYER_PHASES = ("RGGB", "GBRG", "GRBG", "BGGR")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def normalize_phase(phase: str | None) -> str | None:
    if not phase:
        return None
    cleaned = "".join(ch for ch in phase.upper() if ch in "RGBGCMY")
    if len(cleaned) == 4:
        return cleaned
    return cleaned or None


def phase_from_raw(raw: Any) -> str | None:
    pattern = getattr(raw, "raw_pattern", None)
    desc = getattr(raw, "color_desc", "")
    if isinstance(desc, bytes):
        desc_text = desc.decode("ascii", "replace")
    else:
        desc_text = str(desc)
    if pattern is None:
        return None
    rows = int(pattern.shape[0])
    cols = int(pattern.shape[1])
    letters: list[str] = []
    for y in range(min(2, rows)):
        for x in range(min(2, cols)):
            idx = int(pattern[y, x])
            letters.append(desc_text[idx] if 0 <= idx < len(desc_text) else "?")
    phase = normalize_phase("".join(letters))
    return phase if phase in NORMAL_BAYER_PHASES else phase


def scalar_or_first(value: Any, default: float | None = None) -> float | None:
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return float(value)
    try:
        seq = list(value)
    except TypeError:
        return default
    vals = [float(v) for v in seq if v is not None]
    return vals[0] if vals else default


def extract(args: argparse.Namespace) -> dict[str, Any]:
    try:
        import rawpy
    except ModuleNotFoundError as exc:
        raise RuntimeError("rawpy is required to extract raw Bayer samples") from exc

    if not args.input.is_file():
        raise FileNotFoundError(args.input)

    with rawpy.imread(str(args.input)) as raw:
        image = raw.raw_image_visible
        if image is None:
            raise RuntimeError("rawpy did not expose raw_image_visible")
        height, width = int(image.shape[0]), int(image.shape[1])
        phase = phase_from_raw(raw)
        output_bytes = image.astype("<u2", copy=False).tobytes()
        black_level = scalar_or_first(getattr(raw, "black_level_per_channel", None), 0.0)
        white_level = scalar_or_first(
            getattr(raw, "camera_white_level_per_channel", None),
            scalar_or_first(getattr(raw, "white_level", None), None),
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output_bytes)
    output_sha = hashlib.sha256(output_bytes).hexdigest()
    receipt = {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "input": {
            "path": args.input.as_posix(),
            "sha256": sha256_file(args.input),
        },
        "output": {
            "path": args.output.as_posix(),
            "sha256": output_sha,
            "format": "little_endian_uint16_bayer",
            "bytes": len(output_bytes),
        },
        "raw_metadata": {
            "width": width,
            "height": height,
            "cfa_phase": phase,
            "normal_bayer": phase in NORMAL_BAYER_PHASES,
            "black_level": black_level,
            "white_level": white_level,
        },
        "policy": {
            "demosaiced_or_linear_rgb_input": False,
            "usable_for_noise_sidecar_input": phase in NORMAL_BAYER_PHASES,
            "requires_darkframe_stack_for_noise_signal_separation": True,
        },
    }
    if args.write_receipt:
        args.write_receipt.parent.mkdir(parents=True, exist_ok=True)
        args.write_receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return receipt


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", type=Path, required=True, help="DNG/GPR raw input readable by rawpy")
    ap.add_argument("--output", type=Path, required=True, help="little-endian uint16 Bayer output")
    ap.add_argument("--write-receipt", type=Path, help="optional extraction receipt JSON")
    args = ap.parse_args()
    try:
        receipt = extract(args)
    except Exception as exc:  # noqa: BLE001 - command-line tool should report concise errors.
        print(f"extract_raw_bayer_u16: {exc}", file=sys.stderr)
        return 2
    print(args.write_receipt if args.write_receipt else receipt["output"]["path"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
