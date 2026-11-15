# Optional stock-Q3 RGGB14 1x cleanup candidate

`selected.pt` is an explicitly selected research checkpoint for offline cleanup
after historical stock-Q3 VC-5 decoding. It uses the bundled `F_ane_no_sr`
architecture, width 16/depth 3, normalization 16383 and residual scale 0.005.
It preserves Bayer dimensions and supported source calibration. It changes
decoded counts and does not make stock compression lossless.

SHA-256: `9de03f8203f4cb1bb264087cd922b04c1b0e1169684a064d0aeb3a2a6c97f48a`.
Size: 1,312,812 bytes. No model is downloaded or selected automatically.
This file is separate from the installed camera/SDK packages.

The candidate passes the declared validation and four-reference numeric RAW
nonregression guards. On the four historical Nikon references, active-area
PSNR improves 0.328–0.854 dB; shadow, highlight and detail MSE improve on every
reference. The pooled active-area gain is 0.639 dB. The full-strength candidate
failed validation; half strength was selected using validation pixels only,
then the checkpoint was frozen before final evaluation.

This is not broader-camera, sensor-noise, rendered visual, temporal, 2x SR or
production-quality approval. The checkpoint retains `quality_approval=false`.
Its trained/evaluated domain is RGGB14 and stock Q3; other codec formats and
presets do not inherit these results. Pi replay checks use still-derived
inputs, not new lossless sensor references.
Measured backend agreement covers Torch CPU/MPS, FP32 Core ML and the
MPSGraph/hybrid Metal exports on two supplied 12 MP inputs. FP32 Core ML
differs by at most one count; Metal differs by at most two in these checks
(within its existing 32-count bound). FP16 Core ML differs by 3–4 counts and
fails the predefined two-count budget. Use FP32 for the validated Core ML
configuration. See the [native checks](../../docs/stock-cleanup-native.md);
backend agreement does not independently establish model quality.

From the repository root, with optional Torch/numpy/tifffile dependencies:

```sh
python3 tools/stock_still.py restore input.gpr --sample-bits 14 \
  --sdk build-runtime/source/app/gpr_tools/gpr_tools \
  --checkpoint models/stock-q3-rggb14-cleanup/selected.pt \
  --device mps --output restored.dng
```

Use `--device cpu` for CPU inference. Inputs must belong to the declared domain.
See the [study and receipts](../../docs/stock-cleanup-strength.md) for provenance,
source separation, selection criteria, measurements and remaining limits.

A later [supplied-GoPro check](../../docs/stock-cnn-gopro.md) tests this exact
checkpoint on GP011728/GPAB1094 after stock-Q3 re-encoding. All four RAW
regions improve on both files (+0.498/+0.549 dB active PSNR), supported editable
calibration is exact, and matched 16-bit Rec.709 renders improve RGB fidelity
(+0.481/+0.200 dB). References are decoded camera-GPR counts, already compressed;
this is not sensor-noise reduction or broad visual/temporal approval. Weights
and `quality_approval=false` remain unchanged.
