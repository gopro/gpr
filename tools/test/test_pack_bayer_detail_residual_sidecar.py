#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np


REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools/cnn/pack_bayer_detail_residual_sidecar.py"


def test_encode_decode_sidecar() -> None:
    tmp_parent = os.environ.get("GPR_TMPDIR") or os.environ.get("TMPDIR")
    with tempfile.TemporaryDirectory(dir=tmp_parent) as td:
        root = Path(td)
        codec = np.full((8, 8), 1000, dtype=np.uint16)
        clean = codec.copy()
        clean[0::4, 0::4] += 12
        codec_raw = root / "codec.raw"
        clean_raw = root / "clean.raw"
        sidecar = root / "residual_sidecar.npz"
        out_raw = root / "out.raw"
        enc_receipt = root / "encode.json"
        dec_receipt = root / "decode.json"
        codec.tofile(codec_raw)
        clean.tofile(clean_raw)
        subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "encode",
                "--codec-raw",
                str(codec_raw),
                "--clean-raw",
                str(clean_raw),
                "--sidecar",
                str(sidecar),
                "--width",
                "8",
                "--height",
                "8",
                "--planes",
                "r",
                "--quant-step",
                "2",
                "--receipt",
                str(enc_receipt),
            ],
            check=True,
            cwd=REPO,
        )
        subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "decode",
                "--codec-raw",
                str(codec_raw),
                "--sidecar",
                str(sidecar),
                "--out-raw",
                str(out_raw),
                "--width",
                "8",
                "--height",
                "8",
                "--clean-raw",
                str(clean_raw),
                "--receipt",
                str(dec_receipt),
            ],
            check=True,
            cwd=REPO,
        )
        enc = json.loads(enc_receipt.read_text())
        dec = json.loads(dec_receipt.read_text())
        assert enc["schema"] == "gpr.bayer_detail_residual_sidecar.v1"
        assert enc["sidecar_bytes"] > 0
        assert dec["output_clean_rmse"] < dec["codec_clean_rmse"]
        out = np.fromfile(out_raw, dtype="<u2").reshape((8, 8))
        assert not np.array_equal(out, codec)


if __name__ == "__main__":
    test_encode_decode_sidecar()
    print("test_pack_bayer_detail_residual_sidecar: PASS")
