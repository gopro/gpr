"""Extract F (NAFNetTiny SR) checkpoint weights to fp16 binary blobs.

The Metal/MPSGraph host code consumes:

  intro.weight           (16, 4, 3, 3)         conv3x3
  intro.bias             (16,)
  encoders.k.0.*         NAFBlock at C=16/32/64 (k=0,1,2)
    norm1.weight, norm1.bias               (C,)
    conv1.weight (2C, C, 1, 1)              -> stored as [2C, C]
    conv1.bias   (2C,)
    dw.weight    (2C, 1, 3, 3)              -> stored as [2C, 9]
    dw.bias      (2C,)
    proj1.weight (C, C, 1, 1)               -> stored as [C, C]
    proj1.bias   (C,)
    norm2.weight, norm2.bias               (C,)
    mlp1.weight  (2C, C, 1, 1)              -> [2C, C]
    mlp1.bias    (2C,)
    mlp2.weight  (C, C, 1, 1)               -> [C, C]
    mlp2.bias    (C,)
  downs.k.weight  (C_out, C_in, 2, 2)       conv2x2 stride 2
  downs.k.bias    (C_out,)
  middle.0.*    NAFBlock at C=128
  ups.k.0.weight  (2*C_in, C_in, 1, 1)      conv1x1 no bias -> [2*Cin, Cin]
  decoders.k.0.*  NAFBlock at C=64/32/16
  subpixel.0.weight (16, 16, 3, 3)        [variant F only]
  subpixel.0.bias   (16,)                  [variant F only]
  outro.weight      (4, 16, 3, 3)          [variant F_no_sr only — BIBO_1x]
  outro.bias        (4,)                   [variant F_no_sr only — BIBO_1x]

Output convention:
  fp16 little-endian raw bytes. Files named to match the host loader.
  All matrices/tensors are flattened in PyTorch's native (row-major) order.

The full directory is small (~2 MB total).

Variant dispatch is based on the checkpoint's `variant` field:
  - "F"       → 2× super-res. Emits subpixel_weight.bin/subpixel_bias.bin.
  - "F_no_sr" → 1× (no super-res). Emits outro_weight.bin/outro_bias.bin.
"""
import argparse
import os
import sys
import tempfile
from pathlib import Path
import numpy as np
import torch


def f16_save(arr: np.ndarray, path: str) -> int:
    arr = np.ascontiguousarray(arr.astype(np.float16))
    arr.tofile(path)
    return arr.nbytes


def extract_naf(sd_prefix: str, C: int, sd: dict, out_dir: str, name: str) -> int:
    """Extract one NAFBlock's weights to {out_dir}/{name}_*.bin"""
    total = 0
    for tag, expected_shape in [
        ("norm1.weight", (C,)),
        ("norm1.bias",   (C,)),
        ("conv1.weight", (2*C, C, 1, 1)),
        ("conv1.bias",   (2*C,)),
        ("dw.weight",    (2*C, 1, 3, 3)),
        ("dw.bias",      (2*C,)),
        ("proj1.weight", (C, C, 1, 1)),
        ("proj1.bias",   (C,)),
        ("norm2.weight", (C,)),
        ("norm2.bias",   (C,)),
        ("mlp1.weight",  (2*C, C, 1, 1)),
        ("mlp1.bias",    (2*C,)),
        ("mlp2.weight",  (C, C, 1, 1)),
        ("mlp2.bias",    (C,)),
    ]:
        key = f"{sd_prefix}.{tag}"
        if key not in sd:
            raise KeyError(f"missing {key}")
        t = sd[key].detach().cpu().numpy()
        if tuple(t.shape) != expected_shape:
            raise ValueError(f"{key}: got {t.shape}, expected {expected_shape}")
        # Squeeze 1x1 dims for compact layout
        if tag.endswith(".weight") and (tag.startswith("conv1") or tag.startswith("proj1")
                                         or tag.startswith("mlp1") or tag.startswith("mlp2")):
            t = t.squeeze(-1).squeeze(-1)  # [Cout, Cin]
        elif tag == "dw.weight":
            t = t.squeeze(1).reshape(-1, 9)  # [2C, 9]
        out_path = os.path.join(out_dir, f"{name}_{tag.replace('.', '_')}.bin")
        total += f16_save(t, out_path)
    return total


def default_external_root() -> Path:
    return Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())) / "gpr_work"


def main():
    external_root = Path(os.environ.get("GPR_EXTERNAL_ROOT", default_external_root()))
    ckpt_root = Path(os.environ.get("GPR_CHECKPOINT_ROOT", external_root / "checkpoints"))
    tmp_root = Path(os.environ.get("TMPDIR", external_root / "tmp"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", default=str(ckpt_root / "F_aa_off.pt"))
    ap.add_argument("--out", default=str(tmp_root / "F_weights"))
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ckpt = torch.load(args.ckpt, map_location="cpu", weights_only=False)
    sd = ckpt["backbone_state"]
    print(f"  loaded {args.ckpt}")
    print(f"  variant={ckpt['variant']} width={ckpt['width']} depth={ckpt['depth']}"
          f" residual_scale={ckpt['residual_scale']} raw_norm={ckpt['raw_norm']}")
    print(f"  params={ckpt['params']}")

    total = 0

    # intro
    iw = sd["intro.weight"].detach().cpu().numpy()  # [16, 4, 3, 3]
    ib = sd["intro.bias"].detach().cpu().numpy()    # [16]
    assert iw.shape == (16, 4, 3, 3), f"intro.weight: {iw.shape}"
    assert ib.shape == (16,), f"intro.bias: {ib.shape}"
    total += f16_save(iw, os.path.join(args.out, "intro_weight.bin"))
    total += f16_save(ib, os.path.join(args.out, "intro_bias.bin"))

    # encoders
    widths = [16, 32, 64]
    for k, C in enumerate(widths):
        total += extract_naf(f"encoders.{k}.0", C, sd, args.out, f"enc{k}")

    # downs
    down_pairs = [(16, 32), (32, 64), (64, 128)]
    for k, (cin, cout) in enumerate(down_pairs):
        w = sd[f"downs.{k}.weight"].detach().cpu().numpy()  # [cout, cin, 2, 2]
        b = sd[f"downs.{k}.bias"].detach().cpu().numpy()    # [cout]
        assert w.shape == (cout, cin, 2, 2), f"downs.{k}.weight: {w.shape}"
        assert b.shape == (cout,), f"downs.{k}.bias: {b.shape}"
        total += f16_save(w, os.path.join(args.out, f"down{k}_weight.bin"))
        total += f16_save(b, os.path.join(args.out, f"down{k}_bias.bin"))

    # middle (one NAFBlock at C=128)
    total += extract_naf("middle.0", 128, sd, args.out, "middle")

    # ups
    up_widths = [128, 64, 32]  # c_in at each up
    for k, cin in enumerate(up_widths):
        w = sd[f"ups.{k}.0.weight"].detach().cpu().numpy()  # [2*cin, cin, 1, 1] no bias
        assert w.shape == (2*cin, cin, 1, 1), f"ups.{k}.0.weight: {w.shape}"
        w = w.squeeze(-1).squeeze(-1)
        total += f16_save(w, os.path.join(args.out, f"up{k}_weight.bin"))

    # decoders (at C = 64, 32, 16)
    dec_widths = [64, 32, 16]
    for k, C in enumerate(dec_widths):
        total += extract_naf(f"decoders.{k}.0", C, sd, args.out, f"dec{k}")

    # Head: variant-dependent.
    #   F       → subpixel head (16, 16, 3, 3) + PixelShuffle(2) → 4 channels @ 2x
    #   F_no_sr → outro head    (4, 16, 3, 3)  → 4 channels @ 1x
    variant = str(ckpt.get("variant", "F"))
    if variant == "F_no_sr":
        ow = sd["outro.weight"].detach().cpu().numpy()  # [4, 16, 3, 3]
        ob = sd["outro.bias"].detach().cpu().numpy()    # [4]
        assert ow.shape == (4, 16, 3, 3), f"outro.weight: {ow.shape}"
        assert ob.shape == (4,), f"outro.bias: {ob.shape}"
        total += f16_save(ow, os.path.join(args.out, "outro_weight.bin"))
        total += f16_save(ob, os.path.join(args.out, "outro_bias.bin"))
    else:
        # Default: "F" variant (subpixel 2× SR head).
        sw = sd["subpixel.0.weight"].detach().cpu().numpy()  # [16, 16, 3, 3]
        sb = sd["subpixel.0.bias"].detach().cpu().numpy()    # [16]
        assert sw.shape == (16, 16, 3, 3), f"subpixel.0.weight: {sw.shape}"
        assert sb.shape == (16,), f"subpixel.0.bias: {sb.shape}"
        total += f16_save(sw, os.path.join(args.out, "subpixel_weight.bin"))
        total += f16_save(sb, os.path.join(args.out, "subpixel_bias.bin"))

    # Write a tiny manifest for sanity at load time
    with open(os.path.join(args.out, "MANIFEST.txt"), "w") as f:
        f.write(f"variant={ckpt['variant']}\n")
        f.write(f"width={ckpt['width']}\n")
        f.write(f"depth={ckpt['depth']}\n")
        f.write(f"residual_scale={ckpt['residual_scale']}\n")
        f.write(f"raw_norm={ckpt['raw_norm']}\n")
        f.write(f"total_bytes={total}\n")
    print(f"  wrote {total/1024:.1f} KiB across {len(os.listdir(args.out))} files to {args.out}")


if __name__ == "__main__":
    main()
