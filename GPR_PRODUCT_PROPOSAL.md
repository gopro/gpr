# MISSION 1 / HERO PRO — Next-Generation RAW Photo, RAW Video & Optical AI Product Proposal

> **From Engineering Primitives to Ship-Ready Customer Features.**
> Everything in this branch is 100% backwards-compatible with existing Adobe Lightroom®, Camera Raw®, and GPR/VC-5 decoders—while unlocking **eight flagship, marketable camera & desktop features** built specifically around the physics of GoPro’s fixed-lens optical stackup and 50 MP / 12 MP quad-Bayer sensor architecture.

---

## Upgrading GoPro's Multi-Resolution Wavelets & Unlocking **DualRAW™** + **HindSight RAW™**

Before forcing a user (or an auto-mode algorithm) to permanently choose between a **12MP High-SNR RAW** and a **50MP RAW**—or forcing an action photographer to press the shutter *before* the peak moment happens—we looked at what `gopro/gpr` already does well and where we could make a major engineering leap:

1. **DualRAW™: Upgrading GoPro's Existing Multi-Scale Wavelet Preview into True 14-Bit 12MP Bayer RAW Extraction + Sparse-50MP™ Compression**:
   - **What GoPro Already Has**: Stock `gopro/gpr` ([source/lib/vc5_decoder/decoder.c](source/lib/vc5_decoder/decoder.c)) already supports multi-resolution wavelet decoding (`GPR_RGB_RESOLUTION_HALF / QUARTER / EIGHTH / SIXTEENTH` via `WaveletToRGB`). However, that path only decodes **3 channels (`GS, RG, BG`, discarding Channel 3 `GD` green-difference)** and only outputs **debayered 8-bit/16-bit RGB previews**—it cannot extract a true **14-bit 12MP Bayer RAW (`.gpr`/`.dng`)** from a 50MP `.gpr` without decoding all 10 subbands (`8192 x 6144`) and downsampling in software. Moreover, in a stock 50MP `.gpr` (`9.93–14.21 MB`), ~70% of the bitstream is consumed by Stage-0 (`50MP` octave) photon shot noise.
   - **How We Meaningfully Improved It**:
     1. **True 4-Channel 14-Bit 12MP Bayer RAW Extraction (`GPR_RAW_DECODE_HALF=1`)**: Reconstructs all 4 Bayer opponent channels (`GS, RG, BG, GD`) from Subbands `0..6` (`LL1`) directly into a **native 14-bit `4096 x 3072` (12MP) RGGB Bayer RAW** with the exact $+6.02\text{ dB}$ $2\times 2$-binned shadow SNR advantage (`69.27–70.52 dB` PSNR vs. $2\times 2$-binned 50MP).
     2. **`2.1x–2.3x` Faster Decode & `-179 MB` (`-64%`) RAM**: Single-band `CreateLowpassOnlyWavelet` and zero-copy `LL1` transfer skip 100% of Stage-0 (`Subbands 7, 8, 9`) VLC decoding and Stage-0 inverse wavelets (`282 ms` vs. `649 ms` on Raspberry Pi 5; `81 ms` vs. `157 ms` on M5 Max).
     3. **Paired with Sparse-50MP™ (`GPR_SPARSE_8K=1`)**: Shrinks the 50MP `.gpr` container by **`-32.5%` to `-45.8%`** (`9.93 MB -> 5.38 MB` on `GP011689`) while keeping the embedded 12MP `LL1` base virtually lossless (`70.26 dB`) and encoding only true high-SNR 50MP optical edges.
2. **HindSight RAW™ (Continuous Pre-Shutter 14-Bit RAW Ring Buffer)**:
   - GoPro users love **HindSight** for video (capturing the action *before* pressing the shutter), but GoPro has never been able to offer **HindSight RAW Photo / Burst** because raw frames were too slow (`220–450 ms`) and too large (`14–27 MB`) for camera DRAM.
   - Because our fused ARM64 NEON pipeline encodes 12MP Optical-Cored RAW in **`18.2 ms` (`54.9 fps`, `1.12 MB/frame`)** and 50MP Sparse-50MP RAW in **`5.38 MB/frame`**, a **128 MB circular RAM ring buffer** ([tools/stock_auto_mode.py](tools/stock_auto_mode.py) `HindSightRawRingBuffer`) holds **110+ frames (2 full seconds at 55 fps) of 14-bit 12MP RAW** or **24 frames of 50MP Sparse-50MP DualRAW™** continuously in RAM *before* the shutter button is pressed—and flushes only the kept window into a crash-safe `.gvid` v3 clip or `.gpr` burst on trigger!

---

## What the Customer Experiences: 8 Marketable Product Pillars

| Marketing Feature Name | User-Facing Tagline | GoPro Labs QR / Protune Control | What It Does Under the Hood | Measured Performance (Pi 5 Stand-In & M5 Max) |
| :--- | :--- | :--- | :--- | :--- |
| **1. DualRAW™ (`50MP + 12MP` in One File)** | *"Upgraded Wavelet Pyramid: Unpack Any 50MP File as a True 14-Bit 12MP Low-Noise RAW or Full 50MP RAW."* | `*RS50=1` + `GPR_RAW_DECODE_HALF=1` | Upgrades stock 3-channel RGB preview decode to full 4-channel (`GS, RG, BG, GD`) Subband `0..6` (`LL1`) native `4096 x 3072` 14-bit Bayer RAW extraction | **`5.38 MB` single file** $\to$ **`81 ms` M5 / `282 ms` Pi 5 (`2.1–2.3x` faster, `-179 MB` RAM, `70.52 dB` PSNR)** 12MP Bayer RAW OR full 50MP RAW |
| **2. HindSight RAW™ Pre-Capture** | *"Press the Shutter Late. Still Get 2 Full Seconds of 55 fps 14-Bit RAW."* | `*RHND=2` / Protune **HindSight RAW** | Bounded `128 MB` circular RAM ring buffer (`HindSightRawRingBuffer`) continuously retains compressed VC-5 payloads and flushes to `GVID v3` on trigger | Holds **110+ frames (`2.0 s` @ `54.9 fps`) of 12MP RAW** or **24 frames of 50MP Sparse-50MP DualRAW™** in `128 MB` DRAM |
| **3. HyperBurst RAW™** | *"Zero-Lag Shutter. 100+ RAW Frames. Never Miss the Shot."* | `*RBRS=1` / Protune **RAW Burst** | Fused ARM64 NEON streaming VC-5 encoder (`GPR_FAST_ENCODE=1`, `GPR_CHANNEL_ASM=1`) + In-Camera Optical Coring (`GPR_OPTICAL_CORING=2`) | **54.9 fps (`18.2 ms`)** at 12MP (`1.12 MB/frame`) and **12.5 fps (`80.0 ms`)** at full 50MP (`5.91 MB/frame`, **`-40.5%` smaller** `.gpr`) |
| **4. SmartRAW™ Auto** (`12MP` $\leftrightarrow$ `50MP` Best-for-Scene) | *"50MP Detail in the Sun. +6 dB Pure Night & HDR Latitude in the Shadows."* | `*RAUTO=1` / Protune **SmartRAW** | `0.38 ms` strided Bayer radiometric analyzer (`capture_best_image_for_scene()` in `tools/stock_auto_mode.py`) switches between `12MP_HIGH_SNR_DR` (Q5) and `50MP_SPARSE_8K` (Q3) | Selects **High-Bitrate 12MP** on dark/high-contrast scenes (**`+6.02 dB` SNR**, **`+1.5 stops` DR**, `3.5x` faster) and **50MP Sparse-50MP** on bright low-contrast scenes |
| **5. Sparse-50MP™ Hybrid RAW** | *"Full 50MP Edge Acuity at Half the File Size—With a Near-Lossless 12MP Base Inside."* | `*RS50=1` / `GPR_SPARSE_8K=1` | Encodes the $2\times 2$-binned 12MP wavelet base (`LL1`) at **`70.3 dB`**, zeros Stage-0 $C_G$ noise, and sparsely encodes only high-SNR 50MP optical edges | Cuts 50MP `.gpr` size by **`-32.5%` to `-45.8%`** (`9.93 MB -> 5.38 MB` on `GP011689`) while keeping `70.26 dB` 12MP `LL1` fidelity |
| **6. CineRAW™ `.GVID` 4K Video** | *"True 14-Bit Bayer RAW Video in Camera. Instant Rear-LCD Preview. Native MOV Bridge."* | `*GVID=1` / `*BITR=` Tier (`Q1..Q5`) | Append-only `GVID v3` container (`O(1)` seek, `crc32_combine`, per-frame exposure/WB/gain metadata) + lossless `gpr_mov` QuickTime wrapper | **33.8 fps (`29.5 ms`)** at 4K Q4, **42.4 fps (`23.5 ms`)** at 4K Q3+Coring, and **54.9 fps (`18.2 ms`)** at 4K Q1+Coring (`67–91 MB/s` sustained write) |
| **7. Fixed-Lens Optical AI™ (`12MP -> 50MP` Stills & `4K -> 8K` Video SR)** | *"Shoot Fast in 12MP/4K. Master in 50MP/8K with Zero Color Shift."* | Desktop Quik / DaVinci / FCPX Plugin (`gpr_reconstruct`) | Analytical fixed-lens vignette/LCA/MTF inversion + Zero-DC `F_ane` (`12.6K` params) & `Mission1OpticalHybridW96` (`470K` params) on Apple Neural Engine / Metal | **14/14 win rate** across all 50MP GoPro RAWs (**`+0.91 dB` on `GP011689` in `673 ms`**, **`+1.16 dB` mean across 10 DNGs**, **`0.00 counts` DC shift**) |
| **8. TrueGrain™ Reversible Sensor Synthesis** | *"Strip Sensor Noise in Camera for 40% Smaller Files. Dial Exact Sensor Grain Back in Post."* | `*RCOR=2` (In-Camera) + Post Grain Slider (`0–100%`) | Separates deterministic optical signal from stochastic Poisson/read noise in camera; optionally re-synthesizes matched $\mathcal{N}(0, g\hat{Y}+\sigma_r^2)$ grain on decode | **0 bits/px overhead**: saves **`1.1–4.5 MB` per frame** on the SD card while letting colorists choose between ultra-clean plates or authentic sensor grain |

---

## Release Notes Style: How GoPro Can Announce This Update

### MISSION 1 / HERO PRO Firmware + GoPro Labs Update Highlights

#### 1. HyperBurst RAW™ & Zero-Lag Continuous Shooting
Holding down the shutter button now unlocks **sustained high-speed RAW burst capture** without filling the buffer after a handful of frames:
- **100+ Frame 12MP RAW Bursts at up to 55 fps (`18.2 ms/frame`)**: Powered by our new fused ARM64 NEON wavelet pipeline and **In-Camera Optical Coring**, 12MP `.gpr` files shrink to **`1.12–2.54 MB`** while preserving full 14-bit dynamic range and Adobe DNG/GPR compatibility.
- **20+ Frame 50MP RAW Bursts at 10–12.5 fps (`80.0–94.6 ms/frame`)**: Full-sensor `8192 x 6144` 50MP `.gpr` files compress **`32% to 45%` smaller** (`5.38–5.91 MB` vs `9.93 MB` stock on `GP011689`), nearly doubling how many 50MP RAW frames fit in the high-speed camera buffer and flushing to the SD card almost $2\times$ faster.

#### 2. SmartRAW™ Auto Mode (`12MP High-SNR` $\leftrightarrow$ `50MP Sparse-50MP`)
Why force users to choose between **12MP** and **50MP** in the menu before they know what the light looks like?
- When set to **SmartRAW Auto** (`*RAUTO=1`), `capture_best_image_for_scene()` evaluates a strided Bayer radiometric grid in **`0.38 ms`** before exposure/encode:
  - **Low-Light & High-Contrast Action $\implies$ High-Bitrate 12MP (`4096 x 3072`, Quality 5)**: Automatically engages $2\times 2$ Bayer binning when the scene is dark or high-contrast, delivering **`+6.02 dB` higher photon SNR**, **`+1 to +2 stops` of cleaner shadow dynamic range**, and **`3.5x` faster burst speed**. Need a 50MP still deliverable later? Desktop **Optical AI™ 2x Super-Resolution** upscales the clean 12MP RAW to 50MP (`8192 x 6144`) without amplifying sensor shot noise.
  - **Bright Daylight & Low Contrast $\implies$ 50MP Sparse-50MP™ (`8192 x 6144`)**: Automatically captures full 50MP resolution when photon flux is high, encoding the embedded 12MP wavelet base (`LL1`) at **`~70 dB`** and sparsely encoding only true 50MP optical edges.

#### 3. CineRAW™ `.GVID` 4K RAW Video & Instant Camera Preview
For creators and cinematographers who want true 14-bit Bayer latitude in motion:
- **Frame-Independent 14-Bit Bayer RAW Video (`.gvid`)**: Every frame is an independently decodable VC-5 RAW payload with hardware timestamps, exposure, white balance, and static/per-frame DNG calibration inheritance (`GVID v3`).
- **Crash-Safe Append & `O(1)` Timeline Scrubbing**: Built-in `crc32_combine` integrity verification and an `O(1)` seek index allow instant scrubbing on the rear LCD (`1024 x 768` native wavelet preview direct from the `LL2/LL3` subbands—no full-frame debayer required in camera).
- **Zero-Transcode QuickTime Bridge (`gpr_mov`) & ProRes Finishing (`stock_video`)**: Wrap `.gvid` streams directly into `.mov` containers in milliseconds without re-encoding, or transcode to 16-bit DNG sequences / Apple ProRes 4444 / 422 HQ on macOS.

#### 4. Fixed-Lens Optical AI™: Why GoPro Has an Unfair Physics Advantage Over Interchangeable-Lens Cameras
Interchangeable-lens cameras must waste bitrate encoding unknown optical aberrations and must rely on generic blind denoisers in post. Because a GoPro has a **known, factory-calibrated fixed lens + OLPF + sensor stackup**, we exploit exact optical physics at **every stage of the pipeline**:
1. **In Camera (Pre-Entropy Optical Coring)**:
   - Inside every $2\times 2$ Bayer quad, the two green pixels ($G_1, G_2$) sample the exact same optical wavefront. High-frequency differences $|G_1 - G_2| \le \tau$ in Channel 3 ($C_G$) and Stage-0 ($[0.25, 0.50]\text{ cyc/px}$) are physically blocked by the optical low-pass filter (OLPF) and lens MTF—meaning they are **100% uncorrelated sensor shot noise**.
   - Removing those noise-floor bits in ARM64 NEON assembly (`stock_unpack_arm64.S`, `forward.c`) **costs zero extra CPU cycles**, **improves** green-sum structural PSNR (`66.10 dB` vs `62.74 dB`), and shrinks `.gpr` files by **`25% to 45%`**.
2. **On Desktop / Mobile (Analytical Optical Stackup + Zero-DC Neural Restoration)**:
   - On decode, [tools/mission1_analytical_optics.py](tools/mission1_analytical_optics.py) deterministically corrects radial illumination falloff $V(r)$, sub-pixel lateral chromatic aberration $(u_R, u_B)$, and radially varying sagittal/tangential MTF blur.
   - Our **Zero-DC** neural backbones (`F_ane` with `12,672` parameters and `Mission1OpticalHybridW96` with `470,540` parameters) enforce a strict mathematical zero-mean conservation law ($\sum_{h,w} \Delta\hat{x}_c = 0$), guaranteeing **zero exposure or white-balance shift (`0.00 counts` DC bias)** across all 4 RGGB phases and beating bicubic across **14/14 full-frame 50 MP GoPro RAWs** (`+0.91 dB` on `GP011689` in **`673 ms`** via native Apple CoreML + Metal `gpr_reconstruct`, and `+1.16 dB` mean across 10 uncompressed 50 MP DNGs).

---

## End-to-End Architecture Diagram

```mermaid
flowchart LR
    subgraph Camera["In-Camera Firmware / GoPro Labs (ARM64 NEON)"]
        A["14-Bit Bayer Sensor<br/>50MP (8192x6144) or 12MP (4096x3072)"] --> B["SmartRAW™ Auto Analyzer<br/>(0.38 ms Strided Radiometry)"]
        B -- "Dark OR High-Contrast<br/>(+6.02 dB SNR, +1.5 Stops DR)" --> C["12MP High-SNR Path<br/>(Q5..Q1 + Optical Coring)"]
        B -- "Bright AND Low-Contrast<br/>(Full 50MP Edge Acuity)" --> D["50MP Sparse-50MP™ Path<br/>(12MP LL1 Base + Sparse Stage-0)"]
        C --> E["Fused VC-5 Wavelet Encoder<br/>(18.2–28.5 ms on Pi 5)"]
        D --> E2["Fused VC-5 Wavelet Encoder<br/>(80.0–94.6 ms on Pi 5)"]
        E --> F[".GPR Still / .GVID v3 RAW Video<br/>(100% Stock VC-5 Decodable)"]
        E2 --> F
        F --> G["Native Rear-LCD Preview<br/>(1024x768 from LL2 Wavelet)"]
    end

    subgraph Desktop["Quik Desktop / Pro NLE Workflow (CoreML + Metal)"]
        F --> H["Stock GPR / DNG / MOV Decode<br/>or DualRAW™ 12MP Bayer Extract"]
        H --> I["Fixed-Lens Analytical Optics<br/>(Vignette + Lateral CA + Radial MTF)"]
        I --> J["Zero-DC Neural Restoration<br/>1x Cleanup or 2x (12MP -> 50MP) SR"]
        J --> K["Master Output:<br/>16-Bit DNG / GPR 50MP / ProRes 4444<br/>(+ Optional TrueGrain™ Synthesis)"]
    end
```

---

## Proposed GoPro Labs QR Commands & Protune Mapping

To let GoPro field-test these features immediately with power users via **GoPro Labs** before promoting them to main HERO / Mission 1 UI menus, this architecture maps cleanly onto seven concise Labs QR commands:

| Labs QR Command | Protune UI Equivalent | Parameter Values | Description |
| :--- | :--- | :--- | :--- |
| `*RHND=2` | **HindSight RAW: 2s** | `0` = Off, `1` = 1s, `2` = 2s (`128 MB` Ring) | Continuously buffers up to `110` frames (`2.0 s` @ `54.9 fps`) of 12MP RAW or `24` frames of 50MP Sparse-50MP DualRAW™ in RAM before shutter press |
| `*RDUAL=1` | **DualRAW™ Extract** | `0` = 50MP Full, `1` = Instant 12MP (`GPR_RAW_DECODE_HALF=1`) | Upgrades stock RGB preview decode to extract a native `4096 x 3072` (12MP) 14-bit Bayer RAW from Subbands `0..6` (`2.1–2.3x` faster, `-179 MB` RAM, `70.5 dB` PSNR) |
| `*RAUTO=1` | **SmartRAW: Auto** | `0` = Manual Res, `1` = Auto `12MP` / `50MP` | Runs `0.38 ms` scene radiometry (`capture_best_image_for_scene()`); shoots High-Bitrate 12MP in dark/high-contrast scenes and 50MP Sparse-50MP in bright low-contrast scenes |
| `*RCOR=2` | **RAW Noise Opt** | `0` = Off (Bit-Exact), `1` = Chroma Coring, `2` = Full Optical Coring | Strips uncorrelated $C_G$ green-phase and Stage-0 OLPF shot noise in camera (`-23%` to `-40.5%` smaller `.gpr` files, faster burst buffer clearing) |
| `*RS50=1` | **50MP Mode: Sparse-50MP** | `0` = Standard 50MP, `1` = Balanced Sparse-50MP, `2` = Max Compact | Preserves the 12MP `LL1` wavelet base at `70 dB` while sparsely encoding only bright, high-gradient 50MP edges (`-32.5%` to `-45.8%` smaller 50MP `.gpr`) |
| `*RQUAL=4` | **RAW Bitrate Tier** | `1` (Ultra-Fast Burst) .. `5` (Studio Master) | Selects VC-5 quantization tier (`1.12 MB` @ `54.9 fps` in Q1+Coring up to `5.47 MB` @ `76.23 dB` in Q5 on 12MP) |
| `*GVID=1` | **Video Format: CineRAW** | `0` = MP4/HEVC, `1` = `.GVID` v3 RAW Video | Records frame-independent 14-bit Bayer `.gvid` video with `O(1)` seek table, `crc32_combine` integrity, and full DNG calibration metadata |

---

## Verified Hardware Receipts (Apple M5 Max & Raspberry Pi 5 Stand-In)

All code in this PR is backed by automated test suites (`ctest` `16/16` passed on Raspberry Pi 5, `tests/stock_auto_mode_test.py`, `tests/gvid3_test.py`, `tests/stock_coreml_test.py`, `tests/stock_metal_test.py`, `tests/stock_native_test.py`) and reproducible JSON receipts:

- ** priority Stack, High-Bitrate 12 MP vs. Sparse-8K, Reversible Noise/PSF Synthesis & Auto Mode**: [docs/stock-sparse-8k-and-auto-mode.md](docs/stock-sparse-8k-and-auto-mode.md)
- **Clean-Sheet Zero-DC Neural Restoration (`1x` Cleanup & `2x` SR) + Fixed-Lens Optical Physics**: [docs/stock-clean-sheet-m5-max.md](docs/stock-clean-sheet-m5-max.md)
- **Hardware Verification Receipts**:
  - [pi5_standin_validation_receipt.json](docs/stock-clean-sheet-m5-max-receipts/pi5_standin_validation_receipt.json) — Live Raspberry Pi 5 Cortex-A76 benchmarks across 12 MP (`GP011728`, `GPAB1094`) and 50 MP (`GP011689`, `GP010114`, `GP010130`, `GP010136`).
  - [sparse_8k_and_auto_mode_receipt.json](docs/stock-clean-sheet-m5-max-receipts/sparse_8k_and_auto_mode_receipt.json) — 14-scene GoPro 50 MP Auto-Mode classification and 12 MP `Q5..Q1` rate-distortion ladder.
  - [gopro_50mp_full_frame_verification.json](docs/stock-clean-sheet-m5-max-receipts/gopro_50mp_full_frame_verification.json) — 14/14 full-frame 50 MP GoPro RAW Super-Resolution verification.
  - [native_coreml_metal_verification.json](docs/stock-clean-sheet-m5-max-receipts/native_coreml_metal_verification.json) — Native C/Objective-C CoreML + Metal (`gpr_reconstruct`) full-frame verification (`torch_required: False`).
  - [in_camera_neon_optical_coring_benchmark.json](docs/stock-clean-sheet-m5-max-receipts/in_camera_neon_optical_coring_benchmark.json) — Bit-exact default verification and in-camera NEON optical coring benchmarks.
