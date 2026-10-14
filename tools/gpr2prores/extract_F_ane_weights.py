"""Weight extractor for F_ane (BatchNorm + SiLU + optional large kernel).

Differences from extract_F_weights.py (which targets the legacy LN+SimpleGate F):

1. BN is folded into the following Conv1x1 at extraction time.
   bn1 + conv1 (c→2c)  →  effective Conv1x1 with adjusted weight & bias.
   bn2 + mlp1 (c→2c)   →  effective Conv1x1.
   The kernel doesn't need BN/LN at all — just Conv1x1.

2. SimpleGate (chunk+multiply halving 2c→c) is GONE. The block uses SiLU
   (single op) and the channel-halving moves to proj1/mlp2 convs:
     proj1.weight: (c, 2c, 1, 1)   [vs (c, c) in legacy F]
     mlp2.weight:  (c, 2c, 1, 1)   [vs (c, c) in legacy F]

3. dw kernel size may be 3 or 7 (depending on LK variant). Stored as
     dw.weight: [2c, k*k]   where k ∈ {3, 7}

Output file naming (per NAFBlock prefixed `{name}_`):
  {name}_conv1_weight.bin    [2C, C]      BN1 folded in
  {name}_conv1_bias.bin      [2C]         BN1 folded in
  {name}_dw_weight.bin       [2C, k*k]
  {name}_dw_bias.bin         [2C]
  {name}_proj1_weight.bin    [C, 2C]
  {name}_proj1_bias.bin      [C]
  {name}_mlp1_weight.bin     [2C, C]      BN2 folded in
  {name}_mlp1_bias.bin       [2C]         BN2 folded in
  {name}_mlp2_weight.bin     [C, 2C]
  {name}_mlp2_bias.bin       [C]

Plus the intro/down/up/middle/subpixel(or outro) layers, same convention as
extract_F_weights.py but ALL bn* parameters folded into adjacent convs.

Run:
  python3 extract_F_ane_weights.py \\
      --ckpt "$GPR_CHECKPOINT_ROOT/BayInBayOut_2x_AAon_w16_ANE.pt" \\
      --out "$GPR_ARTIFACT_ROOT/weights/F_ane_weights_metal" \\
      --dw-kernel 3
"""
import argparse
import os
import numpy as np
import torch


def f16_save(arr: np.ndarray, path: str) -> int:
    arr = np.ascontiguousarray(arr.astype(np.float16))
    arr.tofile(path)
    return arr.nbytes


def fold_bn_into_conv1x1(bn_weight, bn_bias, bn_mean, bn_var, eps,
                         conv_weight, conv_bias):
    """Fold BatchNorm2d (γ, β, μ, σ²) into the FOLLOWING Conv1x1 (W, b).

    Original computation:   out = W @ ((x - μ)/σ * γ + β) + b
    Folded:                 out = W_fold @ x + b_fold
      where W_fold[i,j] = W[i,j] * γ[j] / σ[j]
            b_fold[i]   = b[i] + Σ_j W[i,j] * (β[j] - μ[j]*γ[j]/σ[j])

    Args:
      bn_weight, bn_bias, bn_mean, bn_var: tensors of shape (C,)
      conv_weight: tensor of shape (Cout, Cin, 1, 1)  with Cin == C
      conv_bias: tensor of shape (Cout,) or None

    Returns:
      (W_fold [Cout, Cin], b_fold [Cout]) as numpy fp32 arrays.
    """
    gamma = bn_weight.detach().cpu().numpy().astype(np.float32)
    beta = bn_bias.detach().cpu().numpy().astype(np.float32)
    mu = bn_mean.detach().cpu().numpy().astype(np.float32)
    var = bn_var.detach().cpu().numpy().astype(np.float32)
    sigma = np.sqrt(var + eps)
    scale = gamma / sigma                       # (C,)
    shift = beta - mu * scale                   # (C,)

    W = conv_weight.detach().cpu().numpy().squeeze(-1).squeeze(-1).astype(np.float32)   # (Cout, Cin)
    b = (conv_bias.detach().cpu().numpy().astype(np.float32)
         if conv_bias is not None else np.zeros(W.shape[0], dtype=np.float32))

    # Apply per-input-channel scale
    W_fold = W * scale[None, :]                 # (Cout, Cin)
    # Adjust bias by passing the BN shift through Conv1x1
    b_fold = b + (W * shift[None, :]).sum(axis=1)
    return W_fold, b_fold


def extract_naf_ane(sd_prefix: str, C: int, sd: dict, out_dir: str, name: str,
                    dw_kernel: int, bn_eps: float = 1e-5) -> int:
    """Extract one F_ane NAFBlock's weights, with BN folded into the
    following Conv1x1 layers."""
    total = 0

    # bn1 → conv1 (c → 2c)
    W_fold, b_fold = fold_bn_into_conv1x1(
        sd[f"{sd_prefix}.bn1.weight"], sd[f"{sd_prefix}.bn1.bias"],
        sd[f"{sd_prefix}.bn1.running_mean"], sd[f"{sd_prefix}.bn1.running_var"],
        bn_eps,
        sd[f"{sd_prefix}.conv1.weight"], sd[f"{sd_prefix}.conv1.bias"],
    )
    assert W_fold.shape == (2*C, C), f"conv1 folded: {W_fold.shape} expected {(2*C, C)}"
    total += f16_save(W_fold, os.path.join(out_dir, f"{name}_conv1_weight.bin"))
    total += f16_save(b_fold, os.path.join(out_dir, f"{name}_conv1_bias.bin"))

    # dw (no BN before it)
    dw_w = sd[f"{sd_prefix}.dw.weight"].detach().cpu().numpy()  # (2C, 1, k, k)
    dw_b = sd[f"{sd_prefix}.dw.bias"].detach().cpu().numpy()
    k = dw_w.shape[-1]
    if k != dw_kernel:
        raise ValueError(f"{name}: dw kernel size {k} mismatches --dw-kernel {dw_kernel}")
    dw_w = dw_w.squeeze(1).reshape(-1, k*k)  # (2C, k*k)
    total += f16_save(dw_w, os.path.join(out_dir, f"{name}_dw_weight.bin"))
    total += f16_save(dw_b, os.path.join(out_dir, f"{name}_dw_bias.bin"))

    # proj1 (2c → c) — no BN before it (just SiLU which is folded into the kernel as activation)
    pj1_w = sd[f"{sd_prefix}.proj1.weight"].detach().cpu().numpy().squeeze(-1).squeeze(-1)
    pj1_b = sd[f"{sd_prefix}.proj1.bias"].detach().cpu().numpy()
    assert pj1_w.shape == (C, 2*C), f"proj1: {pj1_w.shape} expected {(C, 2*C)}"
    total += f16_save(pj1_w, os.path.join(out_dir, f"{name}_proj1_weight.bin"))
    total += f16_save(pj1_b, os.path.join(out_dir, f"{name}_proj1_bias.bin"))

    # bn2 → mlp1 (c → 2c)
    W_fold, b_fold = fold_bn_into_conv1x1(
        sd[f"{sd_prefix}.bn2.weight"], sd[f"{sd_prefix}.bn2.bias"],
        sd[f"{sd_prefix}.bn2.running_mean"], sd[f"{sd_prefix}.bn2.running_var"],
        bn_eps,
        sd[f"{sd_prefix}.mlp1.weight"], sd[f"{sd_prefix}.mlp1.bias"],
    )
    assert W_fold.shape == (2*C, C)
    total += f16_save(W_fold, os.path.join(out_dir, f"{name}_mlp1_weight.bin"))
    total += f16_save(b_fold, os.path.join(out_dir, f"{name}_mlp1_bias.bin"))

    # mlp2 (2c → c)
    mlp2_w = sd[f"{sd_prefix}.mlp2.weight"].detach().cpu().numpy().squeeze(-1).squeeze(-1)
    mlp2_b = sd[f"{sd_prefix}.mlp2.bias"].detach().cpu().numpy()
    assert mlp2_w.shape == (C, 2*C)
    total += f16_save(mlp2_w, os.path.join(out_dir, f"{name}_mlp2_weight.bin"))
    total += f16_save(mlp2_b, os.path.join(out_dir, f"{name}_mlp2_bias.bin"))

    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dw-kernel", type=int, default=3, choices=[3, 7])
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ckpt = torch.load(args.ckpt, map_location="cpu", weights_only=False)
    sd = ckpt["backbone_state"]
    variant = ckpt.get("variant", "F_ane")
    print(f"  loaded {args.ckpt}")
    print(f"  variant={variant}  dw_kernel={args.dw_kernel}")
    print(f"  val_psnr_base/model: {ckpt.get('val_psnr_base')}/{ckpt.get('val_psnr_model')}")

    total = 0

    # intro
    iw = sd["intro.weight"].detach().cpu().numpy()
    ib = sd["intro.bias"].detach().cpu().numpy()
    total += f16_save(iw, os.path.join(args.out, "intro_weight.bin"))
    total += f16_save(ib, os.path.join(args.out, "intro_bias.bin"))

    # detect width from intro
    width = iw.shape[0]
    print(f"  width={width}")

    # encoders C = width, 2*width, 4*width
    widths = [width, 2*width, 4*width]
    for k, C in enumerate(widths):
        total += extract_naf_ane(f"encoders.{k}.0", C, sd, args.out, f"enc{k}",
                                  dw_kernel=args.dw_kernel)

    # downs (Conv stride 2, kernel 2x2)
    for k, (cin, cout) in enumerate([(width, 2*width), (2*width, 4*width), (4*width, 8*width)]):
        w = sd[f"downs.{k}.weight"].detach().cpu().numpy()
        b = sd[f"downs.{k}.bias"].detach().cpu().numpy()
        total += f16_save(w, os.path.join(args.out, f"down{k}_weight.bin"))
        total += f16_save(b, os.path.join(args.out, f"down{k}_bias.bin"))

    # middle (one NAFBlock at C = 8*width)
    total += extract_naf_ane("middle.0", 8*width, sd, args.out, "middle",
                              dw_kernel=args.dw_kernel)

    # ups (Conv 1x1, no bias, then PixelShuffle)
    for k, cin in enumerate([8*width, 4*width, 2*width]):
        w = sd[f"ups.{k}.0.weight"].detach().cpu().numpy().squeeze(-1).squeeze(-1)
        total += f16_save(w, os.path.join(args.out, f"up{k}_weight.bin"))

    # decoders C = 4*width, 2*width, width (reverse order of encoders)
    for k, C in enumerate([4*width, 2*width, width]):
        total += extract_naf_ane(f"decoders.{k}.0", C, sd, args.out, f"dec{k}",
                                  dw_kernel=args.dw_kernel)

    # Head: subpixel (sr=True) or outro (sr=False)
    if "subpixel.0.weight" in sd:
        sub_w = sd["subpixel.0.weight"].detach().cpu().numpy()
        sub_b = sd["subpixel.0.bias"].detach().cpu().numpy()
        total += f16_save(sub_w, os.path.join(args.out, "subpixel_weight.bin"))
        total += f16_save(sub_b, os.path.join(args.out, "subpixel_bias.bin"))
        head_kind = "subpixel"
    elif "outro.weight" in sd:
        out_w = sd["outro.weight"].detach().cpu().numpy()
        out_b = sd["outro.bias"].detach().cpu().numpy()
        total += f16_save(out_w, os.path.join(args.out, "outro_weight.bin"))
        total += f16_save(out_b, os.path.join(args.out, "outro_bias.bin"))
        head_kind = "outro"
    else:
        raise RuntimeError("no subpixel.* or outro.* found in checkpoint")

    # Manifest
    with open(os.path.join(args.out, "MANIFEST.txt"), "w") as f:
        f.write(f"variant={variant}\n")
        f.write(f"width={width}\n")
        f.write(f"depth=3\n")
        f.write(f"residual_scale={ckpt.get('residual_scale', 0.01)}\n")
        f.write(f"raw_norm={ckpt.get('raw_norm', 16383.0)}\n")
        f.write(f"dw_kernel={args.dw_kernel}\n")
        f.write(f"head={head_kind}\n")
        f.write(f"bn_folded=true\n")
        f.write(f"activation=silu\n")
        f.write(f"total_bytes={total}\n")

    print(f"  wrote {total/1024:.1f} KiB to {args.out}")


if __name__ == "__main__":
    main()
