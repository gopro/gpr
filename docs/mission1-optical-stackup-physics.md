# Mission 1 Optical Stackup Physics, Ultra-Fast Backwards-Compatible VC-5 Compression, and In-Camera Execution Feasibility

## 1. Executive Summary: Can All New Improvements Run In-Camera on Standard Mission 1 Hardware?

**Yes for Tier 1 (Ultra-Fast Backwards-Compatible VC-5 Compression) and Tier 2 (Analytical Optical Stackup & Sensor Noise Removal); Tier 3 (Zero-DC Deep Neural Super-Resolution) runs on Desktop/Host Post-Decode (or requires NPU offload).**

| Improvement Tier | Physical / Algorithmic Operation | Backwards Compatible with Stock GPR/VC-5 Decoders? | Runs In-Camera on Standard Mission 1 HW (4x Cortex-A76 @ 2.4 GHz)? | Measured / Projected Latency (`12 MP` / `50 MP`) | Measured Quality / Speed Impact |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **Tier 1: Ultra-Fast Streaming NEON VC-5 Encoder** (`forward.c`, `stock_unpack_arm64.S`) | Fused NEON `Table_14_to_12` log-LUT + opponent transform + streaming 4-row ring-buffer Stage-0 wavelet + adaptive channel/thread scheduling | **100% Byte-for-Byte Identical** stock VC-5 bitstream (`11/11` regression tests pass) | **YES — Runs In-Camera Today** | **28.9 ms** (`12 MP`, >34 fps)<br>**95.7 ms** (`50 MP`, >10 fps) | **6.2x faster** than stock encoder (`594.1 ms` $\to$ `95.7 ms` at 50 MP); memory reduced **62%** (`417 MB` $\to$ `160 MB`) |
| **Tier 2: Closed-Form Analytical Optical Stackup & Sensor Noise Filter** | Fused opponent-space Green-Phase ($C_G$) CRA + shot-noise soft-coring, 24-coeff Polar Radial/Tangential PSF + LCA correction, and `same_color_box2` $2\times 2$ MTF inversion | **100% Compatible** (operates in-register during unpack or post-decode; zero metadata/format breaks) | **YES — In-Camera Capable** (`0` neural weights; `6` NEON FMAs/pixel fused into `stock_unpack_arm64.S`) | **~1.4 ms** fused NEON (`12 MP`)<br>**~5.5 ms** fused NEON (`50 MP`)<br>(`10.65 ms` standalone 4T CPU) | **`+0.2455 dB`** standalone raw PSNR gain; **-31.4% Stage-0 highpass symbol thrashing** in Channel 3 ($C_G$) |
| **Tier 3: Algebraic Zero-DC Neural Restoration (`F_ane` / `Mission1OpticalHybridW96`)** | 1x Raw Compression Cleanup + 2x Super-Resolution (`4096x3072` $\to$ `8192x6144`) with exact zero-DC $2\times 2$ super-pixel projection | **100% Compatible** (runs post-decode on standard `.gpr` / `.gvid` / `.mov` files) | **Desktop / Host Post-Decode** (`~86 s` on 4x Cortex-A76 CPU vs. **45–185 ms** on Apple M5 Max GPU / ANE) | **45 ms** (`12 MP` $\to$ `50 MP` on M5 Max GPU / ANE) | **`+0.8786 dB` to `+1.5571 dB`** on 320-patch Mission 1 benchmark; **0 raw/tone regressions**; **4/4 Pass** on 8K full-frame gate |

---

## 2. How the Mission 1 Optical Stackup Maps Directly onto VC-5's Opponent Wavelet Channels

A key insight of this work is that the Mission 1 camera has a **fixed, invariant optical and sensor stackup**, and the VC-5 codec's internal color transform in [`source/lib/vc5_encoder/stock_unpack_arm64.S`](../source/lib/vc5_encoder/stock_unpack_arm64.S) (`lines 60–70`) naturally decomposes the Bayer quad $(R, G_1, G_2, B)$ into **four physical eigen-channels** where each optical and sensor noise source is isolated:

In [`stock_unpack_arm64.S`](../source/lib/vc5_encoder/stock_unpack_arm64.S), after non-linear 14-to-12-bit log companding (`Table_14_to_12`), the NEON kernel computes:

$$\begin{aligned}
\text{Channel 0 } (\texttt{q4} \to \texttt{[x3]}): \quad Y_G &= \frac{1}{2}(G_1 + G_2) \quad &\text{(Green Luminance)} \\
\text{Channel 1 } (\texttt{q0} \to \texttt{[x4]}): \quad C_R &= \frac{1}{2}(R - Y_G + 2048) \quad &\text{(Red–Green Opponent Chroma)} \\
\text{Channel 2 } (\texttt{q3} \to \texttt{[x5]}): \quad C_B &= \frac{1}{2}(B - Y_G + 2048) \quad &\text{(Blue–Green Opponent Chroma)} \\
\text{Channel 3 } (\texttt{q5} \to \texttt{[x6]}): \quad C_G &= \frac{1}{2}(G_1 - G_2 + 2048) \quad &\text{(Green-Phase Diagonal Difference)}
\end{aligned}$$

### The 5 Physical Stackup Sources & Why They Can Be Removed Ultra-Quickly

```
[Wide-Angle Objective Lens] ──> (1) Radial/Tangential Astigmatic PSF + Vignetting (cos⁴θ)
                            ──> (2) Lateral Chromatic Aberration (LCA: Δr_R(r), Δr_B(r))
        │
        ▼
[IRC / OLPF + Microlens Array] ──> (3) Oblique Chief Ray Angle (CRA) G_r / G_b Crosstalk
        │
        ▼
[CMOS Photodiode + 2x2 Readout] ──> (4) Same-Color 2x2 Binning Sinc² MTF Rolloff (50MP -> 12MP)
                                ──> (5) Poisson Shot Noise + Read Noise + 14→12-bit Log Companding
```

1. **Source 1 — Oblique Chief Ray Angle (CRA) Microlens Crosstalk $\implies$ Isolated in Channel 3 ($C_G$)**:
   - **Physics**: On a real CMOS Bayer sensor behind a compact wide-angle lens, chief rays strike peripheral pixels at oblique angles $\theta(r)$. Although $G_1$ ($G_r$, on the Red row) and $G_2$ ($G_b$, on the Blue row) share identical green color filters, their horizontal neighbors have different spectral filters ($R$ vs. $B$) and different photodiode/metal-grid optical symmetry.
   - **Impact on VC-5 Compression**: In an ideal optical system, $G_1 \approx G_2$ everywhere except sharp $45^\circ$ diagonal edges, so Channel 3 ($C_G = \frac{1}{2}(G_1 - G_2)$) should quantize to long runs of `0` in Stage-0 horizontal/vertical highpass bands. In practice, CRA crosstalk + uncorrelated photon shot noise between $G_1$ and $G_2$ injects a non-zero noise floor across **25.60%** of Stage-0 symbols (`HP RMS = 55.36` ADC counts), breaking VC-5 run-length zero runs into expensive $\pm 1$ VLC codewords.
   - **Ultra-Fast Fix**: Applying closed-form Anscombe/Poisson shot-noise soft-coring in opponent space,
     $$C_G^{\text{clean}} = \operatorname{sign}(C_G)\cdot\max\!\left(0,\; |C_G| - \tau\sqrt{Y_G}\right),$$
     suppresses **31.4% of highpass symbol thrashing** in Channel 3 (`25.60%` $\to$ `17.57%`) while preserving every true diagonal edge where $|G_1 - G_2| \gg \sqrt{Y_G}$.

2. **Source 2 — Lateral Chromatic Aberration (LCA) & Radial Shading $\implies$ Isolated in Channels 1 & 2 ($C_R, C_B$)**:
   - **Physics**: Wavelength-dependent refractive magnification shifts the Red ($620\text{ nm}$) and Blue ($460\text{ nm}$) images radially relative to Green ($530\text{ nm}$) by $\Delta r_{R,B}(r) = \alpha_1 r + \alpha_2 r^3$. At high-contrast luminance edges, a radial shift $\Delta r$ produces a spurious first-derivativespike in the opponent chroma channels:
     $$\Delta C_R(r,\theta) \approx \Delta r_R(r)\,\frac{\partial Y_G}{\partial r}, \qquad \Delta C_B(r,\theta) \approx \Delta r_B(r)\,\frac{\partial Y_G}{\partial r}.$$
   - **Impact on VC-5 Compression**: On the 192 Mission 1 validation patches, Channels 1 and 2 exhibit Stage-0 non-zero symbol rates of **48.18%** and **50.74%** (`HP RMS = 63.38` and `63.53` counts), largely driven by unaligned chroma edges and radial lens shading (`GainMap` variance up to `1.19x` in Green / `1.14x` in Blue).
   - **Ultra-Fast Fix**: Our 24-coefficient polar radial/tangential basis ($\{1, r^2, r^4\} \times \{\nabla_r Y_G, \Delta_r Y_G, \Delta_t Y_G\}$) subtracts the radial gradient coupling $\Delta r(r)\,\partial_r Y_G$ directly from $C_R$ and $C_B$ using 2 FMA operations per pixel.

3. **Source 3 — Radial/Tangential Astigmatic PSF + Same-Color $2\times 2$ Binning MTF $\implies$ Isolated in Channel 0 ($Y_G$)**:
   - **Physics**: Standard Mission 1 `12 MP` (`4096 x 3072`) readout averages $2\times 2$ same-color Bayer pixels from the `50 MP` (`8192 x 6144`) array:
     $$x_{\text{12MP}}[c, y, x] = \frac{1}{4}\sum_{u\in\{0,2\}}\sum_{v\in\{0,2\}} x_{\text{50MP}}[c, 2y+u, 2x+v].$$
     In Fourier space, this box filter multiplies spatial frequencies by $\operatorname{sinc}(\omega_x)\operatorname{sinc}(\omega_y)$, attenuating Nyquist contrast by **-3.9 dB ($0.64\times$)** on top of the objective lens's radially varying sagittal ($\Delta_t$) and meridional ($\Delta_r$) PSF blur.
   - **Impact on VC-5 Compression**: Because $Y_G$ carries almost all surviving fine spatial structure (`HP RMS = 210.29` counts, **74.33%** non-zero Stage-0 symbols — $3\times$ higher than $C_G$), Channel 0 takes **2.05x longer** to entropy-encode than Channels 1–3 (`8.67 ms` vs `4.22 ms` in [`docs/stock-channel-balance.md`](stock-channel-balance.md)).
   - **Ultra-Fast Fix**:
     - For **in-camera encoding**, our adaptive channel/work-unit scheduler ([`docs/stock-channel-balance.md`](stock-channel-balance.md)) overlaps Channel 0's heavy Stage-0 entropy pass across worker threads while fusing Stage-0 horizontal+vertical wavelet lifting into a cache-resident 4-row ring buffer ([`docs/stock-encoder-sub100.md`](stock-encoder-sub100.md)).
     - For **optical restoration**, our closed-form Wiener box-$2\times 2$ polyphase deconvolver + 24-parameter polar PSF inverse restores **`+0.2455 dB`** on Mission 1 raw patches in **6.87 ms** on GPU (or **~1.4 ms** in-register for 1x cleanup) with **zero neural weights**.

4. **Source 4 — Poisson Shot Noise + `Table_14_to_12` Companding Quantization Interaction**:
   - **Physics**: Raw sensor electrons obey Poisson statistics $\sigma_{\text{raw}}^2(I) = g\cdot\max(I - B, 0) + \sigma_{\text{read}}^2$. VC-5's `Table_14_to_12` log-like companding maps 14-bit linear counts (`0..16383`) into 12-bit code values (`0..4095`) specifically to approximate a Variance-Stabilizing (Anscombe) Transform where $\sigma_{\text{companded}} \approx \text{const}$.
   - **Why This Matters**: Any post-decode neural network that adds even a tiny uncalibrated DC bias (`1e-4` in normalized `[0,1]` units $\approx 1.6$ ADC counts) shifts every pixel across companding bin boundaries and destroys full-frame PSNR (causing the `-1.78 dB` regression seen in earlier unprojected SR checkpoints). By enforcing **Algebraic Zero-DC $2\times 2$ Super-Pixel Projection** on the final subpixel convolution kernel:
     $$W_{\text{proj}}[4c+q] = W[4c+q] - \frac{1}{4}\sum_{k=0}^{3} W[4c+k], \qquad b_{\text{proj}}[4c+q] = b[4c+q] - \frac{1}{4}\sum_{k=0}^{3} b[4c+k],$$
     every $2\times 2$ super-resolution quad is mathematically guaranteed to average to the exact Mission 1 input pixel for *any* input image, achieving **0 regressions** across all 320 Mission 1 patches and **4/4 Pass** on full-frame 8K (`8256 x 5504`) evaluation.

---

## 3. Verification Receipts

All machine-readable benchmark and calibration receipts are committed in [`docs/stock-clean-sheet-m5-max-receipts/`](stock-clean-sheet-m5-max-receipts/):
- [`mission1_in_camera_feasibility.json`](stock-clean-sheet-m5-max-receipts/mission1_in_camera_feasibility.json) — Opponent-channel highpass symbol rates, CRA/Poisson soft-coring measurements, and 4-thread CPU / M5 Max GPU latency benchmarks.
- [`mission1_analytical_optics_receipt.json`](stock-clean-sheet-m5-max-receipts/mission1_analytical_optics_receipt.json) — Closed-form 24-coefficient polar PSF/LCA calibration receipt (`+0.2455 dB` zero-neural gain).
- [`clean_sheet_results.json`](stock-clean-sheet-m5-max-receipts/clean_sheet_results.json) — Full-throttle native FP32 training & 8K full-frame gate verification results.
