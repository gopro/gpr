"""Visual quality metrics for codec evaluation.

Computes the metric stack we agreed on after the multi-level cross-hatch
incident: PSNR (Y-channel), MS-SSIM, LPIPS, and ΔE2000. All operate on
demosaiced sRGB at the same dimensions. Plus a separate bayer-PSNR helper
for codec internal sanity checks (NOT to be used as the only metric).

Usage:
    from tools.test.metrics import compute_visual_metrics, bayer_psnr
    metrics = compute_visual_metrics(rgb_ref, rgb_test)  # dict of all metrics

bayer_psnr is computed pre-demosaic for the channel-aware codec stats.
"""
from __future__ import annotations
import numpy as np
from typing import Dict


def bayer_psnr(ref_bayer: np.ndarray, test_bayer: np.ndarray,
               peak: float = 16383.0) -> float:
    """Per-pixel PSNR on bayer planes (the OLD metric — kept for codec
    sanity checks, not to be reported as the only quality number)."""
    ref = ref_bayer.astype(np.float64)
    test = test_bayer.astype(np.float64)
    mse = float(np.mean((ref - test) ** 2))
    if mse <= 0.0:
        return float("inf")
    return 10.0 * np.log10((peak * peak) / mse)


def y_psnr(ref_rgb: np.ndarray, test_rgb: np.ndarray) -> float:
    """PSNR on the luminance (Y) channel of sRGB inputs. Inputs in [0, 255]
    uint8 or [0, 1] float; either works as long as both are the same scale."""
    if ref_rgb.dtype == np.uint8:
        ref = ref_rgb.astype(np.float32) / 255.0
        test = test_rgb.astype(np.float32) / 255.0
    else:
        ref, test = ref_rgb.astype(np.float32), test_rgb.astype(np.float32)
    # BT.709 luma weights — standard for HD/sRGB content
    w = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    y_ref = ref @ w
    y_test = test @ w
    mse = float(np.mean((y_ref - y_test) ** 2))
    if mse <= 0.0:
        return float("inf")
    return -10.0 * np.log10(mse + 1e-12)


def ms_ssim(ref_rgb: np.ndarray, test_rgb: np.ndarray) -> float:
    """Multi-scale SSIM via pytorch-msssim. Inputs in [0, 255] uint8 or
    [0, 1] float — converted to the [0, 1] float tensor MS-SSIM expects."""
    import torch
    from pytorch_msssim import ms_ssim as _ms
    if ref_rgb.dtype == np.uint8:
        ref = torch.from_numpy(ref_rgb.astype(np.float32) / 255.0)
        test = torch.from_numpy(test_rgb.astype(np.float32) / 255.0)
    else:
        ref = torch.from_numpy(ref_rgb.astype(np.float32))
        test = torch.from_numpy(test_rgb.astype(np.float32))
    # (H, W, 3) → (1, 3, H, W)
    ref = ref.permute(2, 0, 1).unsqueeze(0)
    test = test.permute(2, 0, 1).unsqueeze(0)
    with torch.no_grad():
        v = _ms(ref, test, data_range=1.0, win_size=11)
    return float(v.item())


_lpips_state = {"model": None, "device": None}

def lpips_alex(ref_rgb: np.ndarray, test_rgb: np.ndarray) -> float:
    """LPIPS with AlexNet backbone. Lower = more similar. ~0.01 = very close,
    >0.2 = visibly different. Inputs in [0, 255] uint8 or [0, 1] float."""
    import torch
    import lpips
    if _lpips_state["model"] is None:
        _lpips_state["device"] = torch.device(
            "mps" if torch.backends.mps.is_available() else "cpu"
        )
        _lpips_state["model"] = lpips.LPIPS(net="alex").to(_lpips_state["device"]).eval()
    dev = _lpips_state["device"]
    m = _lpips_state["model"]
    if ref_rgb.dtype == np.uint8:
        ref = torch.from_numpy(ref_rgb.astype(np.float32) / 127.5 - 1.0)
        test = torch.from_numpy(test_rgb.astype(np.float32) / 127.5 - 1.0)
    else:
        ref = torch.from_numpy(ref_rgb.astype(np.float32) * 2 - 1.0)
        test = torch.from_numpy(test_rgb.astype(np.float32) * 2 - 1.0)
    ref = ref.permute(2, 0, 1).unsqueeze(0).to(dev)
    test = test.permute(2, 0, 1).unsqueeze(0).to(dev)
    with torch.no_grad():
        v = m(ref, test)
    return float(v.item())


def delta_e_2000(ref_rgb: np.ndarray, test_rgb: np.ndarray) -> float:
    """Mean CIEDE2000 color difference in Lab space. Reported as the mean
    over all pixels. Rough perceptual guide: <1 = imperceptible, 1-3 =
    perceptible but small, 3-6 = visible, >6 = obvious."""
    import skimage.color as sc
    if ref_rgb.dtype == np.uint8:
        ref = ref_rgb.astype(np.float32) / 255.0
        test = test_rgb.astype(np.float32) / 255.0
    else:
        ref, test = ref_rgb.astype(np.float32), test_rgb.astype(np.float32)
    lab_ref = sc.rgb2lab(ref)
    lab_test = sc.rgb2lab(test)
    de = sc.deltaE_ciede2000(lab_ref, lab_test)
    return float(np.mean(de))


def compute_visual_metrics(ref_rgb: np.ndarray, test_rgb: np.ndarray) -> Dict[str, float]:
    """Compute the full visual metric stack. Returns a dict with keys:
    y_psnr, ms_ssim, lpips, dE2000_mean. Inputs at SAME dimensions."""
    if ref_rgb.shape != test_rgb.shape:
        raise ValueError(f"shape mismatch: {ref_rgb.shape} vs {test_rgb.shape}")
    out = {}
    out["y_psnr"] = y_psnr(ref_rgb, test_rgb)
    out["ms_ssim"] = ms_ssim(ref_rgb, test_rgb)
    try:
        out["lpips"] = lpips_alex(ref_rgb, test_rgb)
    except Exception as e:
        out["lpips"] = float("nan")
        out["_lpips_error"] = str(e)[:120]
    try:
        out["dE2000_mean"] = delta_e_2000(ref_rgb, test_rgb)
    except Exception as e:
        out["dE2000_mean"] = float("nan")
        out["_dE_error"] = str(e)[:120]
    return out


def format_metrics(m: Dict[str, float]) -> str:
    """One-line summary suitable for logging or HTML."""
    return (f"Y-PSNR={m.get('y_psnr', float('nan')):6.2f} dB  "
            f"MS-SSIM={m.get('ms_ssim', float('nan')):.4f}  "
            f"LPIPS={m.get('lpips', float('nan')):.4f}  "
            f"ΔE2000={m.get('dE2000_mean', float('nan')):.2f}")


if __name__ == "__main__":
    # Smoke test: ref vs noisy version of itself
    rng = np.random.default_rng(0)
    h, w = 256, 384
    ref = (rng.random((h, w, 3)) * 255).astype(np.uint8)
    test = np.clip(ref.astype(np.int32) + rng.normal(0, 8, ref.shape), 0, 255).astype(np.uint8)
    m = compute_visual_metrics(ref, test)
    print("Self-test (uint8 RGB, σ=8 noise):")
    print(" ", format_metrics(m))
    # Identity (zero diff)
    m0 = compute_visual_metrics(ref, ref)
    print("Identity test (should be perfect):")
    print(" ", format_metrics(m0))
