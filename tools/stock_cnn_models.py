"""Portable legacy BIBO architecture, ported from PR #95 / upstream #71.

State-dict names and arithmetic match tools/cnn/model.py at
8be29eee57b4637865b572f18ab9e3f4edaa2236. Weights are external. Supporting
an architecture does not approve a checkpoint for the stock codec.
Repository Apache-2.0 / MIT license applies.
"""
import torch
import torch.nn as nn


class NAFBlockANE(nn.Module):
    """NAFBlock with LayerNorm → BatchNorm and SimpleGate → SiLU."""
    def __init__(self, c):
        super().__init__()
        # Attention branch
        self.bn1 = nn.BatchNorm2d(c)
        self.conv1 = nn.Conv2d(c, 2*c, 1)
        self.dw = nn.Conv2d(2*c, 2*c, 3, padding=1, groups=2*c)
        self.act1 = nn.SiLU()
        self.proj1 = nn.Conv2d(2*c, c, 1)

        # MLP branch
        self.bn2 = nn.BatchNorm2d(c)
        self.mlp1 = nn.Conv2d(c, 2*c, 1)
        self.act2 = nn.SiLU()
        self.mlp2 = nn.Conv2d(2*c, c, 1)

    def forward(self, x):
        # Attention residual
        h = self.bn1(x)
        h = self.conv1(h)
        h = self.dw(h)
        h = self.act1(h)
        h = self.proj1(h)
        x = x + h
        # MLP residual
        h = self.bn2(x)
        h = self.mlp1(h)
        h = self.act2(h)
        h = self.mlp2(h)
        return x + h


class NAFUNetANE(nn.Module):
    """3-level U-Net of NAFBlockANE — drop-in compute replacement for the F
    architecture (width=16, enc=[1,1,1], dec=[1,1,1], mid=1) with ANE-friendly
    ops. `sr` toggles between 2× super-res (subpixel head) and 1× clean (outro
    Conv3x3) — same conditional as F's NAFUNet.

    `sr4x` mode: output is 4× per spatial dim instead of 2×. Used by the
    demosaic+super-res variant (BIBO_DEMOSAIC_SR4x): input is half-res
    bayer (4ch), output is full-res RGB (3ch) at 2× super-res across the
    bayer-pixel dimension, which is 4× across the codec-channel spatial
    dimension. Combines demosaic and super-res into one network — avoids
    the per-channel bayer-plane bicubic upscale that the 2× super-res
    head applies, which is what produces the over-smoothing artifacts on
    out-of-distribution content (see SHIP_DECISION.md 2026-05-26)."""
    def __init__(self, width=16, enc_blocks=(1, 1, 1), dec_blocks=(1, 1, 1),
                 mid_blocks=1, sr=True, sr4x=False, in_c=4, out_c=4):
        super().__init__()
        self.sr = sr
        self.sr4x = sr4x
        self.intro = nn.Conv2d(in_c, width, 3, padding=1)

        self.encoders = nn.ModuleList()
        self.downs = nn.ModuleList()
        c = width
        for nb in enc_blocks:
            self.encoders.append(nn.Sequential(*[NAFBlockANE(c) for _ in range(nb)]))
            self.downs.append(nn.Conv2d(c, 2*c, 2, stride=2))
            c *= 2

        self.middle = nn.Sequential(*[NAFBlockANE(c) for _ in range(mid_blocks)])

        self.ups = nn.ModuleList()
        self.decoders = nn.ModuleList()
        for nb in dec_blocks:
            self.ups.append(nn.Sequential(nn.Conv2d(c, 2*c, 1, bias=False),
                                           nn.PixelShuffle(2)))
            c //= 2
            self.decoders.append(nn.Sequential(*[NAFBlockANE(c) for _ in range(nb)]))

        if sr4x:
            # 4× per dim via two PixelShuffle(2) stages: total spatial scale 4×.
            # For demosaic+super-res: input 128×128 4ch → output 512×512 3ch.
            self.subpixel = nn.Sequential(
                nn.Conv2d(c, 4*c, 3, padding=1),
                nn.PixelShuffle(2),
                nn.Conv2d(c, 4*out_c, 3, padding=1),
                nn.PixelShuffle(2),
            )
        elif sr:
            self.subpixel = nn.Sequential(
                nn.Conv2d(c, 4*out_c, 3, padding=1),
                nn.PixelShuffle(2),
            )
        else:
            self.outro = nn.Conv2d(c, out_c, 3, padding=1)

    def forward(self, x):
        x = self.intro(x)
        skips = []
        for enc, dn in zip(self.encoders, self.downs):
            x = enc(x); skips.append(x); x = dn(x)
        x = self.middle(x)
        for up, dec, sk in zip(self.ups, self.decoders, reversed(skips)):
            x = up(x)
            x = x + sk
            x = dec(x)
        if self.sr or self.sr4x:
            return self.subpixel(x)
        return self.outro(x)


VARIANTS = {
    'F_ane_no_sr': False, 'bibo_1x': False,
    'F_ane': True, 'bibo_2x': True,
}


def build(variant):
    if variant not in VARIANTS:
        raise ValueError('unsupported bundled BIBO architecture')
    return NAFUNetANE(width=16, enc_blocks=(1,1,1), dec_blocks=(1,1,1),
                      mid_blocks=1, sr=VARIANTS[variant])
