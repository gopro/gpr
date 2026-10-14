#!/usr/bin/env python3
"""Runtime helpers retained for registered quality gates; research commands are archived."""
from __future__ import annotations
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any
import numpy as np
import tifffile


def number_list(value: Any) -> list[float]:
    if isinstance(value, (int, float)):
        return [float(value)]
    if isinstance(value, list):
        return [float(v) for v in value]
    return [float(v) for v in re.findall(r"[-+]?\d*\.?\d+(?:e[-+]?\d+)?", str(value), re.I)]


@dataclass
class DngMeta:
    image_id: str
    path: Path
    iso: int
    black: float
    black_levels: list[float]
    white: float
    white_levels: list[float]
    noise_profile: list[float]
    cfa_pattern: list[int]
    cfa_plane_color: list[str]
    make: str
    model: str

    @property
    def white_minus_black(self) -> float:
        return self.white - self.black


def read_dng_meta(image_id: str, path: Path) -> DngMeta:
    tags = [
        "-j",
        "-n",
        "-ISO",
        "-BlackLevel",
        "-WhiteLevel",
        "-NoiseProfile",
        "-CFAPattern",
        "-CFAPlaneColor",
        "-Make",
        "-Model",
    ]
    meta = json.loads(subprocess.check_output(["exiftool", *tags, str(path)], text=True))[0]
    if "NoiseProfile" not in meta:
        raise RuntimeError(f"{path} has no DNG NoiseProfile tag")
    cfa = [int(v) for v in number_list(meta.get("CFAPattern", ""))]
    if len(cfa) >= 6 and cfa[:2] == [2, 2]:
        cfa = cfa[2:6]
    if cfa != [0, 1, 1, 2]:
        raise RuntimeError(
            f"{path} has unsupported CFA pattern {cfa}; this analyzer currently expects RGGB"
        )
    cfa_plane_color = [part.strip() for part in re.split(r"[,\s]+", str(meta.get("CFAPlaneColor", ""))) if part.strip()]
    if cfa_plane_color and cfa_plane_color[:3] not in (["Red", "Green", "Blue"], ["0", "1", "2"]):
        raise RuntimeError(
            f"{path} has unsupported CFAPlaneColor {cfa_plane_color}; expected Red,Green,Blue"
        )
    black_levels = number_list(meta["BlackLevel"])
    white_levels = number_list(meta["WhiteLevel"])
    return DngMeta(
        image_id=image_id,
        path=path,
        iso=int(number_list(meta["ISO"])[0]),
        black=float(np.mean(black_levels)),
        black_levels=black_levels,
        white=float(np.mean(white_levels)),
        white_levels=white_levels,
        noise_profile=number_list(meta["NoiseProfile"]),
        cfa_pattern=cfa,
        cfa_plane_color=cfa_plane_color,
        make=str(meta.get("Make", "")),
        model=str(meta.get("Model", "")),
    )


def read_bayer(path: Path) -> np.ndarray:
    with tifffile.TiffFile(path) as tf:
        candidates: list[Any] = []
        for page in tf.pages:
            if len(page.shape) == 2 and np.issubdtype(page.dtype, np.integer):
                candidates.append(page)
            for subpage in getattr(page, "pages", None) or []:
                if len(subpage.shape) == 2 and np.issubdtype(subpage.dtype, np.integer):
                    candidates.append(subpage)
        if not candidates:
            raise RuntimeError(f"{path} has no 2D integer raw image IFD")
        page = max(candidates, key=lambda p: int(p.shape[0]) * int(p.shape[1]))
        return page.asarray().astype(np.float32)


def noise_sigma_map(raw: np.ndarray, meta: DngMeta) -> np.ndarray:
    """Return per-pixel DNG NoiseProfile sigma in raw counts."""
    if len(meta.noise_profile) == 2:
        pairs = {
            "R": meta.noise_profile,
            "G": meta.noise_profile,
            "B": meta.noise_profile,
        }
    elif len(meta.noise_profile) >= 6:
        # DNG may store three (scale, offset) variance pairs for R, G, B.
        pairs = {
            "R": meta.noise_profile[0:2],
            "G": meta.noise_profile[2:4],
            "B": meta.noise_profile[4:6],
        }
    else:
        raise RuntimeError(f"expected 2 or 6 NoiseProfile values, got {meta.noise_profile}")
    black = np.full_like(raw, meta.black, dtype=np.float32)
    if len(meta.black_levels) == 3:
        black[0::2, 0::2] = meta.black_levels[0]
        black[0::2, 1::2] = meta.black_levels[1]
        black[1::2, 0::2] = meta.black_levels[1]
        black[1::2, 1::2] = meta.black_levels[2]
    elif len(meta.black_levels) == 4:
        black[0::2, 0::2] = meta.black_levels[0]
        black[0::2, 1::2] = meta.black_levels[1]
        black[1::2, 0::2] = meta.black_levels[2]
        black[1::2, 1::2] = meta.black_levels[3]
    elif len(meta.black_levels) != 1:
        raise RuntimeError(f"unsupported BlackLevel shape: {meta.black_levels}")
    white = np.full_like(raw, meta.white, dtype=np.float32)
    if len(meta.white_levels) == 4:
        white[0::2, 0::2] = meta.white_levels[0]
        white[0::2, 1::2] = meta.white_levels[1]
        white[1::2, 0::2] = meta.white_levels[2]
        white[1::2, 1::2] = meta.white_levels[3]
    elif len(meta.white_levels) != 1:
        raise RuntimeError(f"unsupported WhiteLevel shape: {meta.white_levels}")
    raw_range = white - black
    norm = np.clip((raw - black) / np.maximum(raw_range, 1.0), 0.0, 1.0)
    out = np.zeros_like(norm, dtype=np.float32)
    def fill(view: np.ndarray, pair: list[float]) -> np.ndarray:
        a, b = pair
        return np.sqrt(np.maximum(a * view + b, 0.0))

    out[0::2, 0::2] = fill(norm[0::2, 0::2], pairs["R"]) * raw_range[0::2, 0::2]
    out[0::2, 1::2] = fill(norm[0::2, 1::2], pairs["G"]) * raw_range[0::2, 1::2]
    out[1::2, 0::2] = fill(norm[1::2, 0::2], pairs["G"]) * raw_range[1::2, 0::2]
    out[1::2, 1::2] = fill(norm[1::2, 1::2], pairs["B"]) * raw_range[1::2, 1::2]
    return out
