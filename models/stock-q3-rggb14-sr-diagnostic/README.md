# Optional stock-Q3 RGGB14 2x Zero-DC SR candidate

`selected.pt` provides the Clean-Sheet **Algebraic Zero-DC `2x2` Super-Pixel Projected** 2x CNN reconstruction checkpoint for stock-Q3/RGGB14 inputs, trained in full native FP32 on Apple M5 Max (`tools/m5_max_full_throttle_train.py`).

- **Architecture**: Bundled `F_ane`, width 16 / depth 3, normalization `16383.0`, residual scale **`0.000203125`** ($\eta = 0.65$), with exact zero-DC super-pixel projection on `backbone.subpixel[0]` ($\sum_{k=0}^3 W_{4c+k} = 0$, $\sum_{k=0}^3 b_{4c+k} = 0$).
- **SHA-256**: `cf6e428f4f33eb4284c4e6665b8b4ab94f5b0d8e649d0da82dfe6ee4f6fc9dc2`
- **320-Patch Multi-Domain Validation (Nikon + GoPro Mission 1)**:
  - **Pooled PSNR Gain vs. previous `selected.pt`**: **`+0.8786 dB`** (`+1.1073 dB` at unit scale $\eta = 1.0$)
  - **Raw Regional Regressions**: **`0 / 24`**
  - **Tone-Proxy Regressions**: **`0 / 6`**
- **4-Reference Full-Frame 8K Nikon Quality Gate (`8256 x 5504`)**:
  - Eliminates the full-frame local DC drift that previously caused negative active PSNR on `Z8Z_5323` and `Z8Z_6693`:
    - `Z8Z_0001`: **`+0.66089 dB`** (Standalone 2x SR) / **`+0.60049 dB`** (1x $\to$ 2x Cascade)
    - `Z8Z_0067`: **`+0.37340 dB`** (Standalone 2x SR) / **`+0.47259 dB`** (1x $\to$ 2x Cascade)
    - `Z8Z_5323`: **`+0.00405 dB`** (Standalone 2x SR) / **`+0.08956 dB`** (1x $\to$ 2x Cascade)
    - `Z8Z_6693`: **`+0.00591 dB`** (Standalone 2x SR) / **`+0.06396 dB`** (1x $\to$ 2x Cascade)

From the repository root:

```sh
python3 tools/stock_still.py restore input.gpr --sample-bits 14 \
  --sdk build-runtime/source/app/gpr_tools/gpr_tools \
  --checkpoint models/stock-q3-rggb14-sr-diagnostic/selected.pt \
  --device mps --scale 2 --output restored-2x.dng
```

See [docs/stock-clean-sheet-m5-max.md](../../docs/stock-clean-sheet-m5-max.md) and [docs/stock-clean-sheet-m5-max-receipts/clean_sheet_results.json](../../docs/stock-clean-sheet-m5-max-receipts/clean_sheet_results.json) for full provenance and receipts.
