# Native Camera Preview

The native preview path reads the low-frequency bands of the same GVID used
for full-resolution recording. No alternate reduced-resolution recording or
CNN is required. A 4096 x 3072 capture produces a full-frame 1024 x 768 RGB24
preview, not a center crop.

```sh
build/bin/gvid_preview_rgb_cli clip.gvid 4096 3072 preview 120 1
```

The last argument enables raw RGB frame files. Set it to zero for timing
without output-frame writes. The output directory contains a JSON timing
summary and, when enabled, `frames_rgb/frame_000000.rgb` and subsequent frames.
The native look uses gray-world white balance, percentile contrast, a tone
curve, and saturation. It is a lightweight preview, not a camera-specific
color-calibrated still renderer. Each reduced Bayer cell supplies one RGB
color replicated across its 2 x 2 output block.

This feature depends on the shared codec, fused decoder, and GVID format.
It can be adopted without the capture API or desktop/CNN renderer. The CLI
is a reference implementation; firmware must provide display presentation,
buffer lifetime management, frame pacing, and camera-specific color metadata.

Timing covers decode, preview ISP, and optional frame writes. It does not
include physical display presentation; median processing FPS is not proof of
sustained screen playback.

For validation procedures, see [Testing Methodology](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/TESTING_METHODOLOGY.md)
on the companion `review/product-validation` branch.
