#!/usr/bin/env python3
"""PREVIEW RGB refiner architectures and fixed quality criteria."""
from __future__ import annotations

import sys
import warnings
from pathlib import Path

import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image


warnings.filterwarnings("ignore")
Image.MAX_IMAGE_PIXELS = None
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/cnn"))


PREVIEW = {"lpips": 0.15, "ms_ssim": 0.95, "y_psnr": 28.0, "dE2000_mean": 3.0}
DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")


class ResBlock(nn.Module):
    def __init__(self, width: int) -> None:
        super().__init__()
        self.a = nn.Conv2d(width, width, 3, padding=1)
        self.b = nn.Conv2d(width, width, 3, padding=1)
        self.n = nn.GroupNorm(8, width)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        y = F.gelu(self.n(self.a(x)))
        return F.gelu(x + self.b(y))


class DilatedResBlock(nn.Module):
    def __init__(self, width: int, dilation: int) -> None:
        super().__init__()
        self.a = nn.Conv2d(width, width, 3, padding=dilation, dilation=dilation)
        self.b = nn.Conv2d(width, width, 3, padding=dilation, dilation=dilation)
        self.n = nn.GroupNorm(8, width)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        y = F.gelu(self.n(self.a(x)))
        return F.gelu(x + self.b(y))


class DirectRGBRefiner(nn.Module):
    def __init__(self, width: int = 40, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__()
        self.residual_scale = float(residual_scale)
        self.i = nn.Conv2d(in_channels, width, 3, padding=1)
        self.r0 = ResBlock(width)
        self.d1 = nn.Conv2d(width, width, 3, stride=2, padding=1)
        self.r1 = nn.Sequential(ResBlock(width), ResBlock(width))
        self.d2 = nn.Conv2d(width, width * 2, 3, stride=2, padding=1)
        self.r2 = nn.Sequential(ResBlock(width * 2), ResBlock(width * 2), ResBlock(width * 2))
        self.u = nn.Conv2d(width * 2, width, 3, padding=1)
        self.r3 = nn.Sequential(ResBlock(width), ResBlock(width))
        self.o = nn.Conv2d(width, 3, 3, padding=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        source = x[:, :3]
        skip = self.r0(F.gelu(self.i(x)))
        h = F.gelu(self.d1(skip))
        h = self.r1(h)
        h = F.gelu(self.d2(h))
        h = self.r2(h)
        h = F.interpolate(h, scale_factor=2, mode="bilinear", align_corners=False)
        h = F.gelu(self.u(h))
        h = self.r3(h)
        h = F.interpolate(h, scale_factor=2, mode="bilinear", align_corners=False)
        return torch.clamp(source + self.residual_scale * torch.tanh(self.o(h + skip)), 0.0, 1.0)


class DilatedContextRGBRefiner(nn.Module):
    def __init__(self, width: int = 48, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__()
        self.residual_scale = float(residual_scale)
        self.i = nn.Conv2d(in_channels, width, 3, padding=1)
        self.r0 = ResBlock(width)
        self.d1 = nn.Conv2d(width, width, 3, stride=2, padding=1)
        self.r1 = nn.Sequential(ResBlock(width), DilatedResBlock(width, 2))
        self.d2 = nn.Conv2d(width, width * 2, 3, stride=2, padding=1)
        self.r2 = nn.Sequential(
            DilatedResBlock(width * 2, 1),
            DilatedResBlock(width * 2, 2),
            DilatedResBlock(width * 2, 4),
            DilatedResBlock(width * 2, 8),
            DilatedResBlock(width * 2, 16),
            DilatedResBlock(width * 2, 8),
            DilatedResBlock(width * 2, 4),
            DilatedResBlock(width * 2, 2),
        )
        self.u = nn.Conv2d(width * 2, width, 3, padding=1)
        self.r3 = nn.Sequential(DilatedResBlock(width, 2), ResBlock(width))
        self.o = nn.Conv2d(width, 3, 3, padding=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        source = x[:, :3]
        skip = self.r0(F.gelu(self.i(x)))
        h = F.gelu(self.d1(skip))
        h = self.r1(h)
        h = F.gelu(self.d2(h))
        h = self.r2(h)
        h = F.interpolate(h, scale_factor=2, mode="bilinear", align_corners=False)
        h = F.gelu(self.u(h))
        h = self.r3(h)
        h = F.interpolate(h, scale_factor=2, mode="bilinear", align_corners=False)
        return torch.clamp(source + self.residual_scale * torch.tanh(self.o(h + skip)), 0.0, 1.0)


class LowFreqSpatialRGBRefiner(nn.Module):
    def __init__(self, width: int = 48, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__()
        self.residual_scale = float(residual_scale)
        self.local = DirectRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
        lf_width = max(16, width // 2)
        self.lf = nn.Sequential(
            nn.Conv2d(in_channels, lf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(lf_width),
            nn.Conv2d(lf_width, lf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(lf_width),
            nn.Conv2d(lf_width, 3, 3, padding=1),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        source = x[:, :3]
        local = self.local(x)
        pooled_size = (
            min(48, max(8, int(x.shape[-2]))),
            min(48, max(8, int(x.shape[-1]))),
        )
        low_input = F.interpolate(x, size=pooled_size, mode="bilinear", align_corners=False)
        low = torch.tanh(self.lf(low_input))
        low = F.interpolate(low, size=source.shape[-2:], mode="bilinear", align_corners=False)
        detail_delta = local - source
        return torch.clamp(source + detail_delta + self.residual_scale * 0.5 * low, 0.0, 1.0)


class StrongLowFreqSpatialRGBRefiner(nn.Module):
    def __init__(self, width: int = 64, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__()
        self.residual_scale = float(residual_scale)
        self.local = DirectRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
        lf_width = max(32, width)
        self.lf = nn.Sequential(
            nn.Conv2d(in_channels, lf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(lf_width),
            ResBlock(lf_width),
            nn.Conv2d(lf_width, lf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(lf_width),
            ResBlock(lf_width),
            nn.Conv2d(lf_width, 3, 3, padding=1),
        )
        nn.init.zeros_(self.lf[-1].weight)
        nn.init.zeros_(self.lf[-1].bias)
        self.affine = nn.Sequential(
            nn.Conv2d(in_channels, lf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(lf_width),
            nn.Conv2d(lf_width, 6, 3, padding=1),
        )
        nn.init.zeros_(self.affine[-1].weight)
        nn.init.zeros_(self.affine[-1].bias)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        source = x[:, :3]
        local = self.local(x)
        pooled_size = (
            min(96, max(16, int(x.shape[-2]))),
            min(96, max(16, int(x.shape[-1]))),
        )
        low_input = F.interpolate(x, size=pooled_size, mode="bilinear", align_corners=False)
        low = torch.tanh(self.lf(low_input))
        affine = self.affine(low_input)
        gain = 0.25 * torch.tanh(affine[:, :3])
        bias = 0.25 * torch.tanh(affine[:, 3:])
        low = F.interpolate(low, size=source.shape[-2:], mode="bilinear", align_corners=False)
        gain = F.interpolate(gain, size=source.shape[-2:], mode="bilinear", align_corners=False)
        bias = F.interpolate(bias, size=source.shape[-2:], mode="bilinear", align_corners=False)
        corrected = source * (1.0 + gain) + bias
        detail_delta = local - source
        lf_delta = 0.5 * (corrected - source) + 0.5 * low
        return torch.clamp(source + detail_delta + self.residual_scale * lf_delta, 0.0, 1.0)


class CoordFieldRGBRefiner(nn.Module):
    def __init__(self, width: int = 32, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__()
        if in_channels < 9:
            raise ValueError("CoordFieldRGBRefiner expects source RGB, coordinate planes, and stat planes")
        self.residual_scale = float(residual_scale)
        field_channels = in_channels - 3
        self.field = nn.Sequential(
            nn.Conv2d(field_channels, width, 3, padding=1),
            nn.GELU(),
            ResBlock(width),
            nn.Conv2d(width, width, 3, padding=1),
            nn.GELU(),
            ResBlock(width),
            nn.Conv2d(width, 6, 3, padding=1),
        )
        nn.init.zeros_(self.field[-1].weight)
        nn.init.zeros_(self.field[-1].bias)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        source = x[:, :3]
        field_input = x[:, 3:]
        pooled_size = (
            min(64, max(8, int(x.shape[-2]))),
            min(64, max(8, int(x.shape[-1]))),
        )
        low_input = F.interpolate(field_input, size=pooled_size, mode="bilinear", align_corners=False)
        field = self.field(low_input)
        field = F.interpolate(field, size=source.shape[-2:], mode="bilinear", align_corners=False)
        gain = 0.25 * torch.tanh(field[:, :3])
        bias = 0.25 * torch.tanh(field[:, 3:])
        corrected = source * (1.0 + self.residual_scale * gain) + self.residual_scale * bias
        return torch.clamp(corrected, 0.0, 1.0)


class ResidualLowFreqSpatialRGBRefiner(nn.Module):
    def __init__(self, width: int = 64, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__()
        self.residual_scale = float(residual_scale)
        self.base = LowFreqSpatialRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
        lf_width = max(32, width)
        self.residual = nn.Sequential(
            nn.Conv2d(in_channels + 3, lf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(lf_width),
            ResBlock(lf_width),
            nn.Conv2d(lf_width, lf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(lf_width),
            nn.Conv2d(lf_width, 6, 3, padding=1),
        )
        nn.init.zeros_(self.residual[-1].weight)
        nn.init.zeros_(self.residual[-1].bias)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        source = x[:, :3]
        base = self.base(x)
        pooled_size = (
            min(96, max(16, int(x.shape[-2]))),
            min(96, max(16, int(x.shape[-1]))),
        )
        low_input = F.interpolate(torch.cat([x, base - source], dim=1), size=pooled_size, mode="bilinear", align_corners=False)
        field = self.residual(low_input)
        gain = 0.20 * torch.tanh(field[:, :3])
        bias = 0.20 * torch.tanh(field[:, 3:])
        gain = F.interpolate(gain, size=source.shape[-2:], mode="bilinear", align_corners=False)
        bias = F.interpolate(bias, size=source.shape[-2:], mode="bilinear", align_corners=False)
        return torch.clamp(base * (1.0 + self.residual_scale * gain) + self.residual_scale * bias, 0.0, 1.0)


class MidFreqResidualSpatialRGBRefiner(nn.Module):
    def __init__(self, width: int = 64, in_channels: int = 9, residual_scale: float = 0.5, mid_scale: float = 0.25) -> None:
        super().__init__()
        self.residual_scale = float(residual_scale)
        self.mid_scale = float(mid_scale)
        self.base = LowFreqSpatialRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
        mf_width = max(32, width)
        self.mid = nn.Sequential(
            nn.Conv2d(in_channels + 3, mf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(mf_width),
            DilatedResBlock(mf_width, 2),
            ResBlock(mf_width),
            nn.Conv2d(mf_width, mf_width, 3, padding=1),
            nn.GELU(),
            ResBlock(mf_width),
            nn.Conv2d(mf_width, 3, 3, padding=1),
        )
        nn.init.zeros_(self.mid[-1].weight)
        nn.init.zeros_(self.mid[-1].bias)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        source = x[:, :3]
        base = self.base(x)
        pooled_size = (
            min(256, max(32, int(x.shape[-2]))),
            min(256, max(32, int(x.shape[-1]))),
        )
        mid_input = F.interpolate(torch.cat([x, base - source], dim=1), size=pooled_size, mode="bilinear", align_corners=False)
        mid = torch.tanh(self.mid(mid_input))
        mid = F.interpolate(mid, size=source.shape[-2:], mode="bilinear", align_corners=False)
        return torch.clamp(base + self.residual_scale * self.mid_scale * mid, 0.0, 1.0)


class StrongMidFreqResidualSpatialRGBRefiner(MidFreqResidualSpatialRGBRefiner):
    def __init__(self, width: int = 64, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__(width=width, in_channels=in_channels, residual_scale=residual_scale, mid_scale=0.5)


class ExtraStrongMidFreqResidualSpatialRGBRefiner(MidFreqResidualSpatialRGBRefiner):
    def __init__(self, width: int = 64, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__(width=width, in_channels=in_channels, residual_scale=residual_scale, mid_scale=1.0)


class ContextUNetRGBRefiner(nn.Module):
    def __init__(self, width: int = 32, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__()
        self.residual_scale = float(residual_scale)
        self.i = nn.Conv2d(in_channels, width, 3, padding=1)
        self.e0 = nn.Sequential(ResBlock(width), ResBlock(width))
        self.d1 = nn.Conv2d(width, width * 2, 3, stride=2, padding=1)
        self.e1 = nn.Sequential(ResBlock(width * 2), DilatedResBlock(width * 2, 2))
        self.d2 = nn.Conv2d(width * 2, width * 4, 3, stride=2, padding=1)
        self.e2 = nn.Sequential(ResBlock(width * 4), DilatedResBlock(width * 4, 2), DilatedResBlock(width * 4, 4))
        self.d3 = nn.Conv2d(width * 4, width * 4, 3, stride=2, padding=1)
        self.b = nn.Sequential(DilatedResBlock(width * 4, 4), DilatedResBlock(width * 4, 8), DilatedResBlock(width * 4, 4))
        self.u2 = nn.Sequential(nn.Conv2d(width * 8, width * 4, 3, padding=1), nn.GELU(), ResBlock(width * 4))
        self.u1 = nn.Sequential(nn.Conv2d(width * 6, width * 2, 3, padding=1), nn.GELU(), ResBlock(width * 2))
        self.u0 = nn.Sequential(nn.Conv2d(width * 3, width, 3, padding=1), nn.GELU(), ResBlock(width))
        self.o = nn.Conv2d(width, 3, 3, padding=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        source = x[:, :3]
        e0 = self.e0(F.gelu(self.i(x)))
        e1 = self.e1(F.gelu(self.d1(e0)))
        e2 = self.e2(F.gelu(self.d2(e1)))
        h = self.b(F.gelu(self.d3(e2)))
        h = F.interpolate(h, size=e2.shape[-2:], mode="bilinear", align_corners=False)
        h = self.u2(torch.cat([h, e2], dim=1))
        h = F.interpolate(h, size=e1.shape[-2:], mode="bilinear", align_corners=False)
        h = self.u1(torch.cat([h, e1], dim=1))
        h = F.interpolate(h, size=e0.shape[-2:], mode="bilinear", align_corners=False)
        h = self.u0(torch.cat([h, e0], dim=1))
        return torch.clamp(source + self.residual_scale * torch.tanh(self.o(h)), 0.0, 1.0)


class ContextUNetRGBGenerator(nn.Module):
    def __init__(self, width: int = 32, in_channels: int = 9, residual_scale: float = 0.5) -> None:
        super().__init__()
        self.i = nn.Conv2d(in_channels, width, 3, padding=1)
        self.e0 = nn.Sequential(ResBlock(width), ResBlock(width))
        self.d1 = nn.Conv2d(width, width * 2, 3, stride=2, padding=1)
        self.e1 = nn.Sequential(ResBlock(width * 2), DilatedResBlock(width * 2, 2))
        self.d2 = nn.Conv2d(width * 2, width * 4, 3, stride=2, padding=1)
        self.e2 = nn.Sequential(ResBlock(width * 4), DilatedResBlock(width * 4, 2), DilatedResBlock(width * 4, 4))
        self.d3 = nn.Conv2d(width * 4, width * 4, 3, stride=2, padding=1)
        self.b = nn.Sequential(DilatedResBlock(width * 4, 4), DilatedResBlock(width * 4, 8), DilatedResBlock(width * 4, 4))
        self.u2 = nn.Sequential(nn.Conv2d(width * 8, width * 4, 3, padding=1), nn.GELU(), ResBlock(width * 4))
        self.u1 = nn.Sequential(nn.Conv2d(width * 6, width * 2, 3, padding=1), nn.GELU(), ResBlock(width * 2))
        self.u0 = nn.Sequential(nn.Conv2d(width * 3, width, 3, padding=1), nn.GELU(), ResBlock(width))
        self.o = nn.Conv2d(width, 3, 3, padding=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        e0 = self.e0(F.gelu(self.i(x)))
        e1 = self.e1(F.gelu(self.d1(e0)))
        e2 = self.e2(F.gelu(self.d2(e1)))
        h = self.b(F.gelu(self.d3(e2)))
        h = F.interpolate(h, size=e2.shape[-2:], mode="bilinear", align_corners=False)
        h = self.u2(torch.cat([h, e2], dim=1))
        h = F.interpolate(h, size=e1.shape[-2:], mode="bilinear", align_corners=False)
        h = self.u1(torch.cat([h, e1], dim=1))
        h = F.interpolate(h, size=e0.shape[-2:], mode="bilinear", align_corners=False)
        h = self.u0(torch.cat([h, e0], dim=1))
        return torch.sigmoid(self.o(h))


def build_rgb_refiner(
    architecture: str = "direct",
    *,
    width: int = 40,
    in_channels: int = 9,
    residual_scale: float = 0.5,
) -> nn.Module:
    if architecture == "direct":
        return DirectRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "dilated_context":
        return DilatedContextRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "lowfreq_spatial":
        return LowFreqSpatialRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "lowfreq_spatial_strong":
        return StrongLowFreqSpatialRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "coord_field":
        return CoordFieldRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "lowfreq_spatial_residual":
        return ResidualLowFreqSpatialRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "midfreq_spatial_residual":
        return MidFreqResidualSpatialRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "midfreq_spatial_residual_strong":
        return StrongMidFreqResidualSpatialRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "midfreq_spatial_residual_xstrong":
        return ExtraStrongMidFreqResidualSpatialRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "context_unet":
        return ContextUNetRGBRefiner(width=width, in_channels=in_channels, residual_scale=residual_scale)
    if architecture == "context_unet_generator":
        return ContextUNetRGBGenerator(width=width, in_channels=in_channels, residual_scale=residual_scale)
    raise ValueError(f"unsupported RGB refiner architecture {architecture!r}")


def parse_crop_png(path: Path) -> tuple[str, str, str] | None:
    parts = path.stem.split("_")
    if len(parts) < 4:
        return None
    image_id = "_".join(parts[:2])
    if parts[2] == "center":
        return image_id, "center", "_".join(parts[3:])
    if parts[2] == "upper" and len(parts) > 4 and parts[3] == "left":
        return image_id, "upper_left", "_".join(parts[4:])
    return None


def pass_preview(metrics: dict[str, float]) -> bool:
    return bool(
        metrics["lpips"] <= PREVIEW["lpips"]
        and metrics["ms_ssim"] >= PREVIEW["ms_ssim"]
        and metrics["y_psnr"] >= PREVIEW["y_psnr"]
        and metrics["dE2000_mean"] <= PREVIEW["dE2000_mean"]
    )
