#!/usr/bin/env python3
"""Regression-test the dependency-light `.gvid` v1 validator."""
from __future__ import annotations

import importlib.util
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFY = ROOT / "tools/verify_labs_bundle.py"


def load_verify_module():
    spec = importlib.util.spec_from_file_location("verify_labs_bundle", VERIFY)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {VERIFY}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def clip_header(
    *,
    magic: int = 0x44495647,
    version: int = 1,
    flags: int = 0,
    pixel_format: int = 4,
    quality: int = 3,
    reserved2: int = 0,
    width: int = 640,
    height: int = 360,
    fps_x1000: int = 24000,
    target_kbps: int = 0,
    frame_count_hint: int = 2,
) -> bytes:
    return struct.pack(
        "<IBBHHHIIIII",
        magic,
        version,
        flags,
        pixel_format,
        quality,
        reserved2,
        width,
        height,
        fps_x1000,
        target_kbps,
        frame_count_hint,
    )


def frame(payload: bytes, tag: int, *, magic: int = 0x004D5246, size: int | None = None) -> bytes:
    payload_size = len(payload) if size is None else size
    return struct.pack("<IIQ", magic, payload_size, tag) + payload


def write(path: Path, data: bytes) -> Path:
    path.write_bytes(data)
    return path


def expect_ok(module, path: Path) -> None:
    result = module.validate_gvid(path)
    assert result["frame_count"] == 2
    assert result["width"] == 640
    assert result["height"] == 360


def expect_bad(module, path: Path, needle: str) -> None:
    try:
        module.validate_gvid(path)
    except Exception as exc:
        message = str(exc)
        assert needle in message, f"expected {needle!r} in {message!r}"
        return
    raise AssertionError(f"{path.name}: expected validation failure")


def main() -> int:
    module = load_verify_module()
    work = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd() / "gvid_conformance_smoke"
    work.mkdir(parents=True, exist_ok=True)

    valid = clip_header() + frame(b"alpha", 0) + frame(b"bravo", 1)
    expect_ok(module, write(work / "valid.gvid", valid))

    cases = {
        "bad_magic.gvid": (clip_header(magic=0) + frame(b"a", 0), "bad .gvid clip magic"),
        "bad_version.gvid": (clip_header(version=2) + frame(b"a", 0), "unsupported .gvid version"),
        "unknown_flags.gvid": (clip_header(flags=0x80) + frame(b"a", 0), "unknown .gvid flag"),
        "reserved2.gvid": (clip_header(reserved2=1) + frame(b"a", 0), "nonzero .gvid reserved2"),
        "zero_dims.gvid": (clip_header(width=0) + frame(b"a", 0), "zero .gvid dimensions"),
        "rate_flag_mismatch.gvid": (clip_header(flags=1, target_kbps=0) + frame(b"a", 0), "rate-control flag"),
        "bad_frame_magic.gvid": (clip_header(frame_count_hint=1) + frame(b"a", 0, magic=0), "bad .gvid frame magic"),
        "zero_payload.gvid": (clip_header(frame_count_hint=1) + frame(b"", 0), "zero-size .gvid frame payload"),
        "truncated_payload.gvid": (clip_header(frame_count_hint=1) + frame(b"a", 0, size=4), "truncated .gvid frame payload"),
        "truncated_header.gvid": (clip_header(frame_count_hint=0) + b"abc", "truncated .gvid frame header"),
        "duplicate_tag.gvid": (clip_header() + frame(b"a", 0) + frame(b"b", 0), "non-monotonic .gvid frame tag"),
        "hint_mismatch.gvid": (clip_header(frame_count_hint=3) + frame(b"a", 0) + frame(b"b", 1), "frame_count_hint mismatch"),
        "zero_frame.gvid": (clip_header(frame_count_hint=0), "zero-frame .gvid stream"),
    }
    for name, (data, needle) in cases.items():
        expect_bad(module, write(work / name, data), needle)

    print("test_gvid_conformance: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
