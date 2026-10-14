#!/usr/bin/env python3
"""Pack a lexicographic .gpr frame sequence into the neutral .gvid container."""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import tempfile
from pathlib import Path

GVID_MAGIC = 0x44495647
FRAME_MAGIC = 0x004D5246
GVID_VERSION = 1
FLAG_RATE_CONTROL = 0x01
FLAG_DENOISE = 0x02
QUALITY_MAX = 11
PIXEL_FORMAT_MAX = 5
PAYLOAD_KINDS = {"fused_gpr", "camera_gpr"}


def positive_float(text: str) -> float:
    value = float(text)
    if value <= 0.0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def pack_gvid(
    frame_dir: Path,
    output: Path,
    *,
    width: int,
    height: int,
    fps: float,
    pixel_format: int,
    quality: int,
    target_mbps: float,
    denoise: bool,
) -> int:
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    if not (0 <= pixel_format <= PIXEL_FORMAT_MAX):
        raise ValueError(f"pixel-format must be in 0..{PIXEL_FORMAT_MAX}")
    if not (0 <= quality <= QUALITY_MAX):
        raise ValueError(f"quality must be in 0..{QUALITY_MAX}")
    if fps <= 0.0:
        raise ValueError("fps must be positive")

    frames = sorted(p for p in frame_dir.iterdir() if p.is_file() and p.suffix.lower() == ".gpr")
    if not frames:
        raise FileNotFoundError(f"no .gpr/.GPR frames found in {frame_dir}")

    output.parent.mkdir(parents=True, exist_ok=True)
    flags = 0
    if target_mbps > 0.0:
        flags |= FLAG_RATE_CONTROL
    if denoise:
        flags |= FLAG_DENOISE
    fps_x1000 = int(fps * 1000.0 + 0.5)
    target_kbps = int(target_mbps * 8.0 * 1024.0 + 0.5) if target_mbps > 0.0 else 0

    with output.open("wb") as out:
        out.write(struct.pack(
            "<IBBHHHIIIII",
            GVID_MAGIC,
            GVID_VERSION,
            flags,
            pixel_format,
            quality,
            0,
            width,
            height,
            fps_x1000,
            target_kbps,
            len(frames),
        ))
        for idx, frame in enumerate(frames):
            payload = frame.read_bytes()
            if len(payload) > 0xFFFFFFFF:
                raise ValueError(f"frame too large for .gvid header: {frame}")
            out.write(struct.pack("<IIQ", FRAME_MAGIC, len(payload), idx))
            out.write(payload)
    return len(frames)


def metadata_gvid_path(final_gvid: Path, sidecar: Path) -> str:
    try:
        return str(final_gvid.resolve().relative_to(sidecar.parent.resolve()))
    except ValueError:
        return str(final_gvid)


def write_attached_metadata(
    metadata: Path,
    validation_gvid: Path,
    final_gvid: Path,
    metadata_output: Path | None,
) -> Path:
    from gvid_metadata import validate_against_gvid, validate_metadata

    meta = json.loads(metadata.read_text())
    validate_metadata(meta)
    validate_against_gvid(meta, validation_gvid)
    dst = metadata_output if metadata_output else final_gvid.with_name(final_gvid.name + ".meta.json")
    dst.parent.mkdir(parents=True, exist_ok=True)
    meta["gvid"] = metadata_gvid_path(final_gvid, dst)
    dst.write_text(json.dumps(meta, indent=2) + "\n")
    return dst


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Pack a directory of .gpr frame payloads into a .gvid video container."
    )
    ap.add_argument("frame_dir", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--fps", type=positive_float, default=24.0)
    ap.add_argument("--pixel-format", type=int, default=4, help="fused pixel_format enum; default RGGB16")
    ap.add_argument("--quality", type=int, default=3)
    ap.add_argument("--target-mbps", type=float, default=0.0)
    ap.add_argument("--denoise", action="store_true")
    ap.add_argument("--metadata", type=Path, help="validate and attach a gvid_source_metadata.v1 sidecar")
    ap.add_argument("--metadata-output", type=Path, help="sidecar output path; default is <output>.meta.json")
    ap.add_argument(
        "--payload-kind",
        choices=sorted(PAYLOAD_KINDS),
        default="fused_gpr",
        help="payload interpretation recorded in attached metadata; v1 stream bytes stay unchanged",
    )
    args = ap.parse_args(argv)

    try:
        if args.metadata_output and not args.metadata:
            raise ValueError("--metadata-output requires --metadata")
        if args.metadata:
            meta = json.loads(args.metadata.read_text())
            meta_kind = meta.get("payload_kind", "fused_gpr")
            if meta_kind != args.payload_kind:
                raise ValueError(f"--payload-kind {args.payload_kind!r} does not match metadata payload_kind {meta_kind!r}")
        output = args.output
        temp_output = None
        if args.metadata:
            output.parent.mkdir(parents=True, exist_ok=True)
            fd, temp_name = tempfile.mkstemp(
                prefix=f".{output.name}.",
                suffix=".tmp",
                dir=output.parent,
            )
            os.close(fd)
            temp_output = Path(temp_name)
            temp_output.unlink()
            output = temp_output
        count = pack_gvid(
            args.frame_dir,
            output,
            width=args.width,
            height=args.height,
            fps=args.fps,
            pixel_format=args.pixel_format,
            quality=args.quality,
            target_mbps=args.target_mbps,
            denoise=args.denoise,
        )
        metadata_output = None
        if args.metadata:
            assert temp_output is not None
            metadata_output = write_attached_metadata(args.metadata, temp_output, args.output, args.metadata_output)
            os.replace(temp_output, args.output)
    except Exception as exc:
        if "temp_output" in locals() and temp_output is not None:
            temp_output.unlink(missing_ok=True)
        print(f"gvid_pack: {exc}", file=sys.stderr)
        return 1

    print(f"gvid_pack: wrote {args.output} ({count} frames)")
    if metadata_output:
        print(f"gvid_pack: attached metadata {metadata_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
