#!/usr/bin/env python3
"""Runtime helpers retained for registered quality gates; research commands are archived."""
from __future__ import annotations
from pathlib import Path
import numpy as np
import torch
import torch.nn.functional as F
from skimage import color
from skimage.filters import gaussian
from model import build as build_variant


AB_NORM = 128.0


def _load_model(ckpt_path: str | Path, device: torch.device):
    ck = torch.load(str(ckpt_path), map_location=device, weights_only=False)
    variant = ck.get("variant")
    if variant is None:
        raise RuntimeError(f"Checkpoint {ckpt_path} has no 'variant' field")
    model = build_variant(variant)
    model.load_state_dict(ck["backbone_state"])
    model.to(device).eval()
    return model, ck


def _bayer_to_4plane_tensor(bayer_u16: np.ndarray, raw_norm: float, device: torch.device):
    h, w = bayer_u16.shape
    eh, ew = h - (h & 1), w - (w & 1)
    b = bayer_u16[:eh, :ew]
    planes = np.stack(
        [
            b[0::2, 0::2],
            b[0::2, 1::2],
            b[1::2, 0::2],
            b[1::2, 1::2],
        ],
        axis=0,
    ).astype(np.float32) / raw_norm
    return torch.from_numpy(planes).unsqueeze(0).to(device), planes, (eh, ew)


def _bayer_to_mosaic_tensor(bayer_u16: np.ndarray, raw_norm: float, device: torch.device):
    h, w = bayer_u16.shape
    eh, ew = h - (h & 1), w - (w & 1)
    mosaic = bayer_u16[:eh, :ew].astype(np.float32)[None, None] / raw_norm
    return torch.from_numpy(mosaic).to(device), (eh, ew)


def _bayer_to_mosaic_coord_tensor(bayer_u16: np.ndarray, raw_norm: float, device: torch.device):
    h, w = bayer_u16.shape
    eh, ew = h - (h & 1), w - (w & 1)
    mosaic = bayer_u16[:eh, :ew].astype(np.float32) / raw_norm
    yy = np.arange(eh, dtype=np.float32) / max(1.0, float(eh - 1))
    xx = np.arange(ew, dtype=np.float32) / max(1.0, float(ew - 1))
    y_grid = np.broadcast_to(yy[:, None], (eh, ew))
    x_grid = np.broadcast_to(xx[None, :], (eh, ew))
    arr = np.stack([mosaic, y_grid, x_grid], axis=0)[None]
    return torch.from_numpy(arr).to(device), (eh, ew)


def _pad16(x: torch.Tensor) -> torch.Tensor:
    h, w = x.shape[-2:]
    ph = (16 - h % 16) % 16
    pw = (16 - w % 16) % 16
    return F.pad(x, (0, pw, 0, ph))


def _codec_planes_to_naive_ab_half(planes_chw: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    r, g1, g2, b = planes_chw
    rgb = np.stack([r, 0.5 * (g1 + g2), b], axis=-1)
    lab = color.rgb2lab(np.clip(rgb, 0.0, 1.0))
    return lab[..., 1].astype(np.float32), lab[..., 2].astype(np.float32)


def _display_baseline_to_ab_half(
    baseline_rgb_u8: np.ndarray,
    h: int,
    w: int,
) -> tuple[np.ndarray, np.ndarray]:
    lab = color.rgb2lab(baseline_rgb_u8.astype(np.float32) / 255.0)
    ab = torch.from_numpy(np.transpose(lab[..., 1:3].astype(np.float32), (2, 0, 1))[None])
    ab_half = F.interpolate(ab, size=(h, w), mode="area").squeeze(0).numpy()
    return ab_half[0].astype(np.float32), ab_half[1].astype(np.float32)


def _resolve_ab_prediction(
    raw_ab_t: torch.Tensor,
    a_t: torch.Tensor,
    b_t: torch.Tensor,
    ck: dict,
    ab_norm: float,
) -> torch.Tensor:
    target_mode = ck.get("target_mode", "absolute")
    if target_mode == "absolute":
        return raw_ab_t
    if target_mode != "residual":
        raise RuntimeError(f"Unsupported Lab chroma target_mode {target_mode!r}")

    baseline = F.interpolate(torch.cat([a_t, b_t], dim=1), scale_factor=4, mode="bilinear", align_corners=False)
    residual = raw_ab_t
    limit = float(ck.get("residual_limit_ab", 0.0))
    if limit > 0:
        residual = residual.clamp(-limit / ab_norm, limit / ab_norm)
    return (baseline + residual).clamp(-1.0, 1.0)


def _unsharp_luma(luma: np.ndarray, amount: float, sigma: float) -> np.ndarray:
    if amount <= 0:
        return luma
    blur = gaussian(luma.astype(np.float32), sigma=float(sigma), preserve_range=True)
    return np.clip(luma + amount * (luma - blur), 0.0, 1.0).astype(np.float32)


def _apply_wavelet_hf_luma(
    l_chan: np.ndarray,
    gain: float = 1.0,
    wavelet: str = "sym4",
    levels: int = 3,
    hf_levels: int = 1,
    max_delta: float = 2.0,
) -> np.ndarray:
    if abs(float(gain) - 1.0) < 1e-6 or int(hf_levels) <= 0:
        return l_chan
    try:
        import pywt
    except Exception as exc:
        raise RuntimeError("luma wavelet high-frequency gain requires pywavelets") from exc

    l_f = l_chan.astype(np.float32)
    coeffs = pywt.wavedec2(l_f, wavelet, level=int(levels))
    detail_only = [np.zeros_like(coeffs[0])]
    first_selected = max(1, len(coeffs) - int(hf_levels))
    for idx, detail in enumerate(coeffs[1:], start=1):
        if idx >= first_selected:
            detail_only.append(tuple((float(gain) - 1.0) * band for band in detail))
        else:
            detail_only.append(tuple(np.zeros_like(band) for band in detail))
    delta = pywt.waverec2(detail_only, wavelet)[: l_f.shape[0], : l_f.shape[1]]
    if max_delta > 0:
        delta = np.clip(delta, -float(max_delta), float(max_delta))
    return np.clip(l_f + delta, 0.0, 100.0).astype(np.float32)


def run_lab_chroma_corrector(
    bayer_u16: np.ndarray,
    y_ckpt: str | Path,
    chroma_ckpt: str | Path,
    baseline_rgb_u8: np.ndarray | None = None,
    device: torch.device | None = None,
    raw_norm: float = 16383.0,
    luma_unsharp_amount: float = 0.0,
    luma_unsharp_sigma: float = 2.0,
    luma_detail_refiner_path: str | None = None,
    luma_detail_cnn_path: str | None = None,
    luma_detail_cnn_strength: float = 1.0,
    rgb_detail_cnn_path: str | None = None,
    luma_wavelet_hf_gain: float = 1.0,
    luma_wavelet_hf_wavelet: str = "sym4",
    luma_wavelet_hf_levels: int = 3,
    luma_wavelet_hf_hf_levels: int = 1,
    luma_wavelet_hf_max_delta: float = 2.0,
) -> np.ndarray:
    if luma_detail_refiner_path or luma_detail_cnn_path or rgb_detail_cnn_path:
        raise RuntimeError("detail sidecar paths are not implemented in this runner")
    if device is None:
        device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")

    y_model, y_ck = _load_model(y_ckpt, device)
    chroma_model, ck = _load_model(chroma_ckpt, device)
    ab_norm = float(ck.get("ab_norm", AB_NORM))

    x, planes, _ = _bayer_to_4plane_tensor(bayer_u16, raw_norm, device)
    h, w = x.shape[-2:]
    input_mode = y_ck.get("input_mode", "planes4x")

    with torch.no_grad():
        if input_mode == "mosaic2x":
            xm, (eh, ew) = _bayer_to_mosaic_tensor(bayer_u16, raw_norm, device)
            y_full_t = y_model(_pad16(xm)).clamp(0, 1)[..., : 2 * eh, : 2 * ew]
        elif input_mode == "mosaic2x_coord":
            xm, (eh, ew) = _bayer_to_mosaic_coord_tensor(bayer_u16, raw_norm, device)
            y_full_t = y_model(_pad16(xm)).clamp(0, 1)[..., : 2 * eh, : 2 * ew]
        elif input_mode == "planes4x":
            y_full_t = y_model(_pad16(x)).clamp(0, 1)[..., : 4 * h, : 4 * w]
        else:
            raise RuntimeError(f"Unsupported Y checkpoint input_mode {input_mode!r}")

        y_half_t = F.avg_pool2d(y_full_t, kernel_size=4, stride=4)
        if baseline_rgb_u8 is None:
            a_half, b_half = _codec_planes_to_naive_ab_half(planes)
        else:
            a_half, b_half = _display_baseline_to_ab_half(baseline_rgb_u8, h, w)

        a_t = torch.from_numpy(a_half[None, None] / ab_norm).to(device)
        b_t = torch.from_numpy(b_half[None, None] / ab_norm).to(device)
        inp = torch.cat([x, y_half_t, a_t, b_t], dim=1)
        raw_ab_t = chroma_model(_pad16(inp))[..., : 4 * h, : 4 * w]
        ab_t = _resolve_ab_prediction(raw_ab_t, a_t, b_t, ck, ab_norm)

    y_full = y_full_t.squeeze(0).squeeze(0).cpu().numpy()
    y_full = _unsharp_luma(y_full, float(luma_unsharp_amount), float(luma_unsharp_sigma))
    ab = ab_t.squeeze(0).cpu().numpy() * ab_norm

    y_rgb = np.repeat(y_full[..., None], 3, axis=2)
    l_chan = color.rgb2lab(np.clip(y_rgb, 0.0, 1.0))[..., 0]
    l_chan = _apply_wavelet_hf_luma(
        l_chan,
        gain=float(luma_wavelet_hf_gain),
        wavelet=str(luma_wavelet_hf_wavelet),
        levels=int(luma_wavelet_hf_levels),
        hf_levels=int(luma_wavelet_hf_hf_levels),
        max_delta=float(luma_wavelet_hf_max_delta),
    )

    lab = np.empty((y_full.shape[0], y_full.shape[1], 3), dtype=np.float32)
    lab[..., 0] = l_chan
    lab[..., 1] = ab[0]
    lab[..., 2] = ab[1]
    rgb = color.lab2rgb(lab)
    return np.clip(rgb * 255.0, 0, 255).astype(np.uint8)
