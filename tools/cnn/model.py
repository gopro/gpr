"""F-arch backbone, ANE-friendly variant.

Same 3-level UNet skeleton as F (NAFNetTiny w=16). Two ops swapped vs the
original NAFBlock so the converted CoreML model can run on Apple Neural
Engine at peak throughput:

  LayerNorm2d (per-pixel mean+var) → BatchNorm2d (folds into preceding conv at inference)
  SimpleGate (chunk + multiply halves) → SiLU (single ANE-native op)

Channel sizes are preserved (16/32/64/128 across the U-net). The SimpleGate
removal means the "halving" effect now lives in the proj1 / mlp2 convs which
go 2c → c (vs c → c in the original). Net ~+25% params per block.

See docs/ANE_FRIENDLY_F_PLAN.md in the main gpr repo for the rationale.
"""
import torch
import torch.nn as nn


def count_params(m):
    return sum(p.numel() for p in m.parameters() if p.requires_grad)


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


# Naming convention (referenced in docs and checkpoint filenames):
#   BIBO = Bayer In, Bayer Out — codec-domain CNN (in_c=4, out_c=4, no rendering)
#   BIDO = Bayer In, Demosaic-Out — joint demosaic CNN (in_c=4, out_c=3 RGB)
# Scale suffix: 1x = same res, 2x = 2x spatial, 4x = 4x spatial.
VARIANTS = {
    # BIBO variants (bayer in, bayer out)
    "F_ane":       dict(width=16, enc=[1, 1, 1], dec=[1, 1, 1], mid=1, sr=True),    # BIBO 2x
    "F_ane_no_sr": dict(width=16, enc=[1, 1, 1], dec=[1, 1, 1], mid=1, sr=False),   # BIBO 1x
    # BIDO variant (bayer in, RGB out, 4x spatial — joint demosaic+super-res)
    "F_ane_dm_sr": dict(width=16, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                        sr=False, sr4x=True, in_c=4, out_c=3),
}
# Future-naming aliases — same configs under BIBO/BIDO names. Use these in new
# checkpoint filenames going forward (e.g. BayInDemosaicOut_4x_AAon_w16_ANE.pt).
VARIANTS["bibo_2x"]   = VARIANTS["F_ane"]
VARIANTS["bibo_1x"]   = VARIANTS["F_ane_no_sr"]
VARIANTS["bido_4x"]   = VARIANTS["F_ane_dm_sr"]
# Capacity-scaled BIDO variants are added below in the research-variants section.


def build(tag):
    cfg = VARIANTS[tag]
    if "dw_kernel" in cfg:
        return NAFUNetANE_LK(width=cfg["width"], enc_blocks=cfg["enc"],
                             dec_blocks=cfg["dec"], mid_blocks=cfg["mid"],
                             sr=cfg.get("sr", False), sr4x=cfg.get("sr4x", False),
                             in_c=cfg.get("in_c", 4), out_c=cfg.get("out_c", 4),
                             dw_kernel=cfg["dw_kernel"])
    return NAFUNetANE(width=cfg["width"], enc_blocks=cfg["enc"],
                      dec_blocks=cfg["dec"], mid_blocks=cfg["mid"],
                      sr=cfg.get("sr", False), sr4x=cfg.get("sr4x", False),
                      in_c=cfg.get("in_c", 4), out_c=cfg.get("out_c", 4))


if __name__ == "__main__":
    for tag in VARIANTS:
        m = build(tag)
        n = count_params(m)
        print(f"{tag:15s}  params={n:>10,}")
    # smoke test on MPS
    import os
    if "KMP_DUPLICATE_LIB_OK" not in os.environ:
        os.environ["KMP_DUPLICATE_LIB_OK"] = "TRUE"
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    print(f"device: {device}")
    m = build("F_ane").to(device).eval()
    x = torch.randn(1, 4, 1384, 2072, device=device)
    import time
    # Warm up
    with torch.no_grad():
        for _ in range(3):
            _ = m(x)
    if device.type == "mps":
        torch.mps.synchronize()
    t0 = time.time()
    with torch.no_grad():
        for _ in range(10):
            y = m(x)
    if device.type == "mps":
        torch.mps.synchronize()
    t1 = time.time()
    print(f"F_ane forward 10x on {tuple(x.shape)}: avg {(t1-t0)/10*1000:.1f} ms  out {tuple(y.shape)}")


# --- Wider / large-kernel variants for research-driven experiments ---
# These extend the VARIANTS dict with arch variations to test what actually
# moves rendered PSNR (as opposed to cosmetic op-swaps).

import torch.nn as _nn


class NAFBlockANE_LK(_nn.Module):
    """NAFBlockANE but with dw 7x7 (large kernel) in the attention path.
    Adaptation of SMFFRaw's large-kernel attention idea — wider receptive
    field per layer."""
    def __init__(self, c, dw_kernel=7):
        super().__init__()
        pad = dw_kernel // 2
        self.bn1 = _nn.BatchNorm2d(c)
        self.conv1 = _nn.Conv2d(c, 2*c, 1)
        self.dw = _nn.Conv2d(2*c, 2*c, dw_kernel, padding=pad, groups=2*c)
        self.act1 = _nn.SiLU()
        self.proj1 = _nn.Conv2d(2*c, c, 1)
        self.bn2 = _nn.BatchNorm2d(c)
        self.mlp1 = _nn.Conv2d(c, 2*c, 1)
        self.act2 = _nn.SiLU()
        self.mlp2 = _nn.Conv2d(2*c, c, 1)

    def forward(self, x):
        h = self.bn1(x); h = self.conv1(h); h = self.dw(h); h = self.act1(h); h = self.proj1(h)
        x = x + h
        h = self.bn2(x); h = self.mlp1(h); h = self.act2(h); h = self.mlp2(h)
        return x + h


class NAFUNetANE_LK(NAFUNetANE):
    """U-Net with large-kernel NAFBlockANE_LK at every level."""
    def __init__(self, width=16, enc_blocks=(1, 1, 1), dec_blocks=(1, 1, 1),
                 mid_blocks=1, sr=True, sr4x=False, in_c=4, out_c=4, dw_kernel=7):
        # Initialize NAFUNetANE first so all the layers exist, then swap blocks
        super().__init__(width=width, enc_blocks=enc_blocks, dec_blocks=dec_blocks,
                         mid_blocks=mid_blocks, sr=sr, sr4x=sr4x,
                         in_c=in_c, out_c=out_c)
        # Replace every NAFBlockANE with NAFBlockANE_LK(dw_kernel=dw_kernel)
        def _channel_of(seq):
            return seq[0].bn1.weight.shape[0]
        for i, enc in enumerate(self.encoders):
            c = _channel_of(enc)
            self.encoders[i] = _nn.Sequential(*[NAFBlockANE_LK(c, dw_kernel) for _ in range(enc_blocks[i])])
        mid_c = _channel_of(self.middle)
        self.middle = _nn.Sequential(*[NAFBlockANE_LK(mid_c, dw_kernel) for _ in range(mid_blocks)])
        for i, dec in enumerate(self.decoders):
            c = _channel_of(dec)
            self.decoders[i] = _nn.Sequential(*[NAFBlockANE_LK(c, dw_kernel) for _ in range(dec_blocks[i])])


# Extend VARIANTS with research-driven variants
VARIANTS.update({
    "F_ane_w24":       dict(width=24, enc=[1, 1, 1], dec=[1, 1, 1], mid=1, sr=True),
    "F_ane_w24_no_sr": dict(width=24, enc=[1, 1, 1], dec=[1, 1, 1], mid=1, sr=False),
    "F_ane_w32":       dict(width=32, enc=[1, 1, 1], dec=[1, 1, 1], mid=1, sr=True),
    "F_ane_w32_no_sr": dict(width=32, enc=[1, 1, 1], dec=[1, 1, 1], mid=1, sr=False),
    "F_ane_dm_sr_w24": dict(width=24, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                            sr=False, sr4x=True, in_c=4, out_c=3),
    "F_ane_dm_sr_w32": dict(width=32, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                            sr=False, sr4x=True, in_c=4, out_c=3),
})
# Aliases for capacity-scaled BIDO variants.
VARIANTS["bido_4x_w24"] = VARIANTS["F_ane_dm_sr_w24"]
VARIANTS["bido_4x_w32"] = VARIANTS["F_ane_dm_sr_w32"]
# Aliases preserving the "F_ane_<no_sr>_<width>" naming used by the
# 2026-05-28 heavy-decoder branch — same arch, different name order.
VARIANTS["F_ane_no_sr_w24"] = VARIANTS["F_ane_w24_no_sr"]
VARIANTS["F_ane_no_sr_w32"] = VARIANTS["F_ane_w32_no_sr"]

# --- YCbCr per-channel decomposition variants (PREVIEW_CHANNEL_DECOMP_PLAN
# Variant A). Bayer in (4ch half-res), single-channel out (Y or Cb or Cr) at
# 4× spatial scale to match the BIDO output dims. Reuses the sr4x output head
# (two PixelShuffle stages) so the spatial scaling is identical to BIDO; only
# the final out_c=1 differs. The narrower w8 variant is for the chroma
# channels which carry less high-frequency content.
VARIANTS.update({
    "F_ane_no_sr_w16_y": dict(width=16, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                              sr=False, sr4x=True, in_c=4, out_c=1),
    "F_ane_no_sr_w24_y": dict(width=24, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                              sr=False, sr4x=True, in_c=4, out_c=1),
    "F_ane_no_sr_w32_y": dict(width=32, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                              sr=False, sr4x=True, in_c=4, out_c=1),
    "F_ane_no_sr_w8_chroma": dict(width=8, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                                  sr=False, sr4x=True, in_c=4, out_c=1),
    "F_ane_mosaic_w32_y": dict(width=32, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                               sr=True, sr4x=False, in_c=1, out_c=1),
    "F_ane_mosaic_coord_w32_y": dict(width=32, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                                     sr=True, sr4x=False, in_c=3, out_c=1),
    "F_ane_mosaic_w48_y": dict(width=48, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                               sr=True, sr4x=False, in_c=1, out_c=1),
    "F_ane_no_sr_w32_y_lk7": dict(width=32, enc=[1, 1, 1], dec=[1, 1, 1], mid=1,
                                  sr=False, sr4x=True, in_c=4, out_c=1,
                                  dw_kernel=7),
    "F_ane_chroma_corrector_w12": dict(width=12, enc=[1, 1, 1], dec=[1, 1, 1],
                                       mid=1, sr=False, sr4x=True, in_c=7,
                                       out_c=2),
})


def build_lk(tag, dw_kernel=7):
    """Build a large-kernel variant. tag is one of the standard F_ane variants;
    the dw kernel size is added separately."""
    cfg = VARIANTS[tag]
    return NAFUNetANE_LK(width=cfg["width"], enc_blocks=cfg["enc"],
                         dec_blocks=cfg["dec"], mid_blocks=cfg["mid"],
                         sr=cfg["sr"], dw_kernel=dw_kernel)
