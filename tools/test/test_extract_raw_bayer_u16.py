#!/usr/bin/env python3
"""Regression test for raw Bayer uint16 extraction receipts."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import struct
import sys
import tempfile
import types
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/extract_raw_bayer_u16.py"


def temp_root() -> Path:
    if os.environ.get("GPR_TMPDIR"):
        root = Path(os.environ["GPR_TMPDIR"])
    else:
        root = Path(tempfile.gettempdir())
    root.mkdir(parents=True, exist_ok=True)
    return root


class FakePattern:
    shape = (2, 2)

    def __getitem__(self, key):
        y, x = key
        return ((0, 1), (3, 2))[y][x]


class FakeImage:
    shape = (4, 4)

    def astype(self, dtype, copy=False):  # noqa: ARG002
        assert dtype == "<u2"
        return self

    def tobytes(self) -> bytes:
        return struct.pack("<16H", *range(16))


class FakeRaw:
    raw_pattern = FakePattern()
    color_desc = b"RGBG"
    raw_image_visible = FakeImage()
    black_level_per_channel = [64, 64, 64, 64]
    camera_white_level_per_channel = [16383, 16383, 16383, 16383]

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):  # noqa: ANN001
        return False


def import_tool():
    spec = importlib.util.spec_from_file_location("extract_raw_bayer_u16_under_test", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    rawpy = types.SimpleNamespace(imread=lambda path: FakeRaw())
    old_rawpy = sys.modules.get("rawpy")
    sys.modules["rawpy"] = rawpy
    try:
        module = import_tool()
        with tempfile.TemporaryDirectory(prefix="gpr_extract_raw_bayer_u16_", dir=temp_root()) as tmp:
            work = Path(tmp)
            src = work / "input.dng"
            out = work / "out.raw"
            receipt = work / "receipt.json"
            src.write_bytes(b"fake dng bytes")
            data = module.extract(
                argparse.Namespace(input=src, output=out, write_receipt=receipt)
            )
            assert data["schema"] == "gpr.raw_bayer_u16_extract.v1"
            assert data["raw_metadata"]["width"] == 4
            assert data["raw_metadata"]["height"] == 4
            assert data["raw_metadata"]["cfa_phase"] == "RGGB"
            assert data["raw_metadata"]["normal_bayer"] is True
            assert data["output"]["bytes"] == 32
            assert out.read_bytes() == struct.pack("<16H", *range(16))
            from_disk = json.loads(receipt.read_text(encoding="utf-8"))
            assert from_disk["output"]["sha256"] == data["output"]["sha256"]
            assert from_disk["policy"]["usable_for_noise_sidecar_input"] is True
    finally:
        if old_rawpy is None:
            sys.modules.pop("rawpy", None)
        else:
            sys.modules["rawpy"] = old_rawpy
    print("test_extract_raw_bayer_u16: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
