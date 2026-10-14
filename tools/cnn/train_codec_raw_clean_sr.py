#!/usr/bin/env python3
"""Runtime helpers retained for registered quality gates; research commands are archived."""
from __future__ import annotations
import torch
import torch.nn as nn


class CodecRawCleanSR(nn.Module):
    def __init__(self, width: int, in_channels: int = 8) -> None:
        super().__init__()
        self.net = nn.Sequential(
            nn.Conv2d(in_channels, width, 3, padding=1),
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=2, dilation=2),
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=4, dilation=4),
            nn.GELU(),
            nn.Conv2d(width, width, 3, padding=1),
            nn.GELU(),
            nn.Conv2d(width, 4, 3, padding=1),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        base = x[:, :4]
        return torch.clamp(base + self.net(x), 0.0, 1.0)
