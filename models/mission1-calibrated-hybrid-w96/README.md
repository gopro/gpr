# Mission 1 Analytical-Optical + Calibrated Width-96 2x SR Hybrid (`Mission1OpticalHybridW96`)

`selected.pt` combines closed-form GoPro Mission 1 fixed-optical-chain characterization with the Width-96 / Depth-6 dilated coordinate-aware 2x SR architecture (`CoordDetailPrecleanAdapterPixelShuffleSR`), trained in full native FP32 on Apple M5 Max (`tools/m5_max_full_throttle_train.py`).

- **Architecture**: `Mission1OpticalHybridW96` (`width=96, depth=6`), uniting:
  1. **Closed-Form Mission 1 Polar PSF ($\partial_{rr}, \partial_{\theta\theta}, r^2\partial_{rr}, r^2\partial_{\theta\theta}$) + Lateral Chromatic Aberration ($r\partial_r(R-G), r\partial_r(B-G)$)** 1x optical deconvolution initialized via Cholesky ridge regression,
  2. **Analytical Box-2x2 Consistent Base** ($y_{\text{analytic}} = U_{\text{bicubic}}(\tilde{x}) + U_{\text{nearest}}(\tilde{x} - \text{AvgPool}_{2\times2}(U_{\text{bicubic}}(\tilde{x})))$, providing **`+0.2455 dB`** on Mission 1 validation patches before any neural layers),
  3. **Canonical Sensor Pedestal Normalization** (`[black, white] -> [1008, 15892]`) with canonical shadow damping (`s_floor=0.08, s_span=0.08, min_damp=0.0`).
- **SHA-256**: `bd5dc352e94b118b012eb304a41e96c9f3a559a4cceccb4309f38208f68933b7`
- **320-Patch Validation Benchmark (Nikon + GoPro Mission 1)**:
  - **Pooled MSE**: `3344.99` vs. `4787.45` (Bicubic)
  - **Pooled PSNR Gain vs. Bicubic**: **`+1.5571 dB`** (`+1.6598 dB` un-damped)
  - **Raw Regional Regressions**: **`0 / 24`**
  - **Tone-Proxy Regressions**: **`0 / 6`**

See [docs/stock-clean-sheet-m5-max.md](../../docs/stock-clean-sheet-m5-max.md) and [docs/stock-clean-sheet-m5-max-receipts/](../../docs/stock-clean-sheet-m5-max-receipts/) for full mathematical formulation and receipts.
