#!/usr/bin/env python3
"""Mission 1 Bayer SR architectures and checkpoint-config construction."""
from __future__ import annotations

from typing import Any

import torch
import torch.nn as nn
import torch.nn.functional as F


RAW_SCALE = 16383.0
DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")


class ResidualSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.residual_scale = residual_scale
        layers: list[nn.Module] = [nn.Conv2d(4, width, 3, padding=1), nn.GELU()]
        for _ in range(max(0, depth - 2)):
            layers += [nn.Conv2d(width, width, 3, padding=1), nn.GELU()]
        layers.append(nn.Conv2d(width, 4, 3, padding=1))
        self.net = nn.Sequential(*layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        base = F.interpolate(x, scale_factor=2, mode="bilinear", align_corners=False)
        residual = self.net(base)
        return torch.clamp(base + residual * self.residual_scale, 0.0, 1.0)


class LowResPixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.residual_scale = residual_scale
        layers: list[nn.Module] = [nn.Conv2d(4, width, 3, padding=1), nn.GELU()]
        for _ in range(max(0, depth - 2)):
            layers += [nn.Conv2d(width, width, 3, padding=1), nn.GELU()]
        residual_head = nn.Conv2d(width, 16, 3, padding=1)
        nn.init.zeros_(residual_head.weight)
        nn.init.zeros_(residual_head.bias)
        layers += [residual_head, nn.PixelShuffle(2)]
        self.net = nn.Sequential(*layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        base = F.interpolate(x, scale_factor=2, mode="bilinear", align_corners=False)
        residual = self.net(x)
        return torch.clamp(base + residual * self.residual_scale, 0.0, 1.0)


class ResidualBlock(nn.Module):
    def __init__(self, width: int, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.residual_scale = residual_scale
        self.net = nn.Sequential(
            nn.Conv2d(width, width, 3, padding=1),
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=1),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return x + self.net(x) * self.residual_scale


class ResBlockPixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        block_count = max(1, depth - 2)
        self.residual_scale = residual_scale
        self.head = nn.Conv2d(4, width, 3, padding=1)
        self.body = nn.Sequential(*[ResidualBlock(width) for _ in range(block_count)])
        residual_head = nn.Conv2d(width, 16, 3, padding=1)
        nn.init.zeros_(residual_head.weight)
        nn.init.zeros_(residual_head.bias)
        self.tail = nn.Sequential(
            nn.GELU(),
            residual_head,
            nn.PixelShuffle(2),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        base = F.interpolate(x, scale_factor=2, mode="bilinear", align_corners=False)
        feat = self.head(x)
        residual = self.tail(self.body(feat))
        return torch.clamp(base + residual * self.residual_scale, 0.0, 1.0)


class EdgePixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.residual_scale = residual_scale
        layers: list[nn.Module] = [nn.Conv2d(4, width, 3, padding=1), nn.GELU()]
        for _ in range(max(0, depth - 2)):
            layers += [nn.Conv2d(width, width, 3, padding=1), nn.GELU()]
        residual_head = nn.Conv2d(width, 16, 3, padding=1)
        nn.init.zeros_(residual_head.weight)
        nn.init.zeros_(residual_head.bias)
        layers += [residual_head, nn.PixelShuffle(2)]
        self.net = nn.Sequential(*layers)
        edge_head = nn.Conv2d(4, 16, 3, padding=1)
        nn.init.zeros_(edge_head.weight)
        nn.init.zeros_(edge_head.bias)
        self.edge = nn.Sequential(edge_head, nn.PixelShuffle(2))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        base = F.interpolate(x, scale_factor=2, mode="bilinear", align_corners=False)
        residual = self.net(x)
        blur = F.avg_pool2d(x, kernel_size=3, stride=1, padding=1)
        edge_residual = self.edge(x - blur)
        return torch.clamp(base + (residual + edge_residual) * self.residual_scale, 0.0, 1.0)


class AdapterPixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1, input_channels: int = 4) -> None:
        super().__init__()
        self.residual_scale = residual_scale
        self.input_channels = input_channels
        layers: list[nn.Module] = [nn.Conv2d(input_channels, width, 3, padding=1), nn.GELU()]
        for _ in range(max(0, depth - 2)):
            layers += [nn.Conv2d(width, width, 3, padding=1), nn.GELU()]
        residual_head = nn.Conv2d(width, 16, 3, padding=1)
        nn.init.zeros_(residual_head.weight)
        nn.init.zeros_(residual_head.bias)
        layers += [residual_head, nn.PixelShuffle(2)]
        self.net = nn.Sequential(*layers)

        adapter_layers: list[nn.Module] = [
            nn.Conv2d(input_channels, width, 3, padding=2, dilation=2),
            nn.GELU(),
        ]
        for dilation in (2, 4):
            adapter_layers += [
                nn.Conv2d(width, width, 3, padding=dilation, dilation=dilation),
                nn.GELU(),
            ]
        adapter_head = nn.Conv2d(width, 16, 3, padding=1)
        nn.init.zeros_(adapter_head.weight)
        nn.init.zeros_(adapter_head.bias)
        adapter_layers += [adapter_head, nn.PixelShuffle(2)]
        self.adapter = nn.Sequential(*adapter_layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        base = F.interpolate(x[:, :4], scale_factor=2, mode="bilinear", align_corners=False)
        residual = self.net(x) + self.adapter(x)
        return torch.clamp(base + residual * self.residual_scale, 0.0, 1.0)


class GreenDetailAdapterPixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.residual_scale = residual_scale
        self.sr = AdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
        detail_layers: list[nn.Module] = [
            nn.Conv2d(4, width, 3, padding=1),
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=2, dilation=2),
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=4, dilation=4),
            nn.GELU(),
        ]
        detail_head = nn.Conv2d(width, 8, 3, padding=1)
        nn.init.zeros_(detail_head.weight)
        nn.init.zeros_(detail_head.bias)
        detail_layers += [detail_head, nn.PixelShuffle(2)]
        self.green_detail = nn.Sequential(*detail_layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        out = self.sr(x)
        green_residual = self.green_detail(x)
        green = torch.clamp(out[:, 1:3] + green_residual * self.residual_scale, 0.0, 1.0)
        return torch.cat([out[:, 0:1], green, out[:, 3:4]], dim=1)


class PrecleanAdapterPixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.preclean_scale = 0.05
        self.sr = AdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
        preclean_head = nn.Conv2d(4, width, 3, padding=1)
        preclean_tail = nn.Conv2d(width, 4, 3, padding=1)
        nn.init.zeros_(preclean_tail.weight)
        nn.init.zeros_(preclean_tail.bias)
        self.preclean = nn.Sequential(
            preclean_head,
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=2, dilation=2),
            nn.GELU(),
            preclean_tail,
        )

    def clean_low(self, x: torch.Tensor) -> torch.Tensor:
        return torch.clamp(x + self.preclean(x) * self.preclean_scale, 0.0, 1.0)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.sr(self.clean_low(x))


class CoordPrecleanAdapterPixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.preclean_scale = 0.05
        self.sr = AdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale, input_channels=6)
        preclean_head = nn.Conv2d(4, width, 3, padding=1)
        preclean_tail = nn.Conv2d(width, 4, 3, padding=1)
        nn.init.zeros_(preclean_tail.weight)
        nn.init.zeros_(preclean_tail.bias)
        self.preclean = nn.Sequential(
            preclean_head,
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=2, dilation=2),
            nn.GELU(),
            preclean_tail,
        )

    def clean_low(self, x: torch.Tensor) -> torch.Tensor:
        raw = x[:, :4]
        return torch.clamp(raw + self.preclean(raw) * self.preclean_scale, 0.0, 1.0)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if x.shape[1] != 6:
            raise ValueError("coord_preclean_adapter_pixelshuffle expects 6 input channels")
        clean = self.clean_low(x)
        return self.sr(torch.cat([clean, x[:, 4:6]], dim=1))


class CoordDeepPrecleanAdapterPixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.preclean_scale = 0.05
        self.sr = AdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale, input_channels=6)
        preclean_head = nn.Conv2d(4, width, 3, padding=1)
        preclean_tail = nn.Conv2d(width, 4, 3, padding=1)
        nn.init.zeros_(preclean_tail.weight)
        nn.init.zeros_(preclean_tail.bias)
        self.preclean = nn.Sequential(
            preclean_head,
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=2, dilation=2),
            nn.GELU(),
            preclean_tail,
        )

        extra_layers: list[nn.Module] = [
            nn.Conv2d(4, width, 3, padding=1),
            nn.GELU(),
        ]
        for dilation in (1, 2, 4, 2, 1):
            extra_layers += [
                nn.Conv2d(width, width, 3, padding=dilation, dilation=dilation),
                nn.GELU(),
            ]
        extra_tail = nn.Conv2d(width, 4, 3, padding=1)
        nn.init.zeros_(extra_tail.weight)
        nn.init.zeros_(extra_tail.bias)
        extra_layers.append(extra_tail)
        self.preclean_extra = nn.Sequential(*extra_layers)

    def clean_low(self, x: torch.Tensor) -> torch.Tensor:
        raw = x[:, :4]
        correction = self.preclean(raw) + self.preclean_extra(raw)
        return torch.clamp(raw + correction * self.preclean_scale, 0.0, 1.0)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if x.shape[1] != 6:
            raise ValueError("coord_deep_preclean_adapter_pixelshuffle expects 6 input channels")
        clean = self.clean_low(x)
        return self.sr(torch.cat([clean, x[:, 4:6]], dim=1))


class CoordDetailPrecleanAdapterPixelShuffleSR(nn.Module):
    def __init__(self, width: int = 32, depth: int = 5, residual_scale: float = 0.1) -> None:
        super().__init__()
        self.preclean_scale = 0.05
        self.sr = AdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale, input_channels=10)
        preclean_head = nn.Conv2d(4, width, 3, padding=1)
        preclean_tail = nn.Conv2d(width, 4, 3, padding=1)
        nn.init.zeros_(preclean_tail.weight)
        nn.init.zeros_(preclean_tail.bias)
        self.preclean = nn.Sequential(
            preclean_head,
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=2, dilation=2),
            nn.GELU(),
            preclean_tail,
        )

    def clean_low(self, x: torch.Tensor) -> torch.Tensor:
        raw = x[:, :4]
        return torch.clamp(raw + self.preclean(raw) * self.preclean_scale, 0.0, 1.0)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if x.shape[1] != 6:
            raise ValueError("coord_detail_preclean_adapter_pixelshuffle expects 6 input channels")
        clean = self.clean_low(x)
        detail = binomial_detail(clean)
        return self.sr(torch.cat([clean, x[:, 4:6], detail], dim=1))


def make_model(architecture: str, width: int, depth: int, residual_scale: float) -> nn.Module:
    if architecture == "residual_highres":
        return ResidualSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "lowres_pixelshuffle":
        return LowResPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "resblock_pixelshuffle":
        return ResBlockPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "edge_pixelshuffle":
        return EdgePixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "adapter_pixelshuffle":
        return AdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "green_detail_adapter_pixelshuffle":
        return GreenDetailAdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "preclean_adapter_pixelshuffle":
        return PrecleanAdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "coord_preclean_adapter_pixelshuffle":
        return CoordPrecleanAdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "coord_detail_preclean_adapter_pixelshuffle":
        return CoordDetailPrecleanAdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    if architecture == "coord_deep_preclean_adapter_pixelshuffle":
        return CoordDeepPrecleanAdapterPixelShuffleSR(width=width, depth=depth, residual_scale=residual_scale)
    raise ValueError(f"unknown architecture: {architecture}")


def make_model_from_config(config: dict[str, Any]) -> nn.Module:
    return make_model(
        str(config.get("architecture", "residual_highres")),
        width=int(config["width"]),
        depth=int(config["depth"]),
        residual_scale=float(config["residual_scale"]),
    )


def binomial_detail(x: torch.Tensor) -> torch.Tensor:
    kernel = torch.tensor(
        [[1.0, 2.0, 1.0], [2.0, 4.0, 2.0], [1.0, 2.0, 1.0]],
        dtype=x.dtype,
        device=x.device,
    ).view(1, 1, 3, 3) / 16.0
    kernel = kernel.repeat(x.shape[1], 1, 1, 1)
    low = F.conv2d(F.pad(x, (1, 1, 1, 1), mode="reflect"), kernel, groups=x.shape[1])
    return x - low


def architecture_uses_coords(architecture: str) -> bool:
    return architecture in {
        "coord_preclean_adapter_pixelshuffle",
        "coord_deep_preclean_adapter_pixelshuffle",
        "coord_detail_preclean_adapter_pixelshuffle",
    }
