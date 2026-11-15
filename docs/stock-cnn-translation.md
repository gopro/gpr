# Bayer translation consistency diagnostic

The supplied stock 2x model changes its reconstructed counts when the same
scene shifts by one packed-Bayer pixel. Bicubic interpolation stays exact
after alignment, and an eight-packed-pixel shift stays exact for both. A CPU
control reproduces the CNN difference within the existing one-count MPS
agreement budget. This identifies a concrete spatial-consistency problem to
investigate; it does not approve or measure real-video appearance.

## Frozen synthetic transform

Two native 4096x3072 decoded RAWs from the
[selected-model recording](stock-selected-video-chain.md) use their exact
source/checkpoint hashes. Each source produces four input variants: original,
two sensor pixels right, two sensor pixels down, and sixteen sensor pixels
right. All shifts preserve Bayer phase; uncovered edges replicate the source
edge. The existing decoded counts move together, without new codec encoding,
noise, exposure change, blur or occlusion.

The standard supplied 2x runtime uses tile 256, halo 64, four-channel RGGB14,
and MPS. A separate bicubic control uses the identical Bayer packing, tiling,
rounding and output-count convention. Outputs are 8192x6144. The protocol is
saved before inference; no model training, strength search or final-reference
selection follows the diagnostic.

Outputs are aligned using the known integer transform, without optical flow
or interpolation. A fixed 512-output-sensor-pixel border excludes actual frame
edges. Each comparison covers 36,700,160 samples, including all four Bayer
phases. Both sources' bicubic controls and both sixteen-pixel CNN controls
are **byte-exact after alignment** in this region.

## Measured count changes

Differences compare the aligned shifted output with that model's unshifted
output. They are not errors against a high-resolution ground truth.

| Source | Low-input sensor shift | CNN RMS difference, counts | 99th percentile absolute difference | Maximum absolute difference |
|---|---|---:|---:|---:|
| GP011728 | 2 right | 1.86253 | 5 | 148 |
| GP011728 | 2 down | 2.27878 | 6 | 207 |
| GPAB1094 | 2 right | 2.12803 | 6 | 83 |
| GPAB1094 | 2 down | 2.62165 | 7 | 120 |
| Both | 16 right | 0 | 0 | 0 |

The rare maxima should not be described as a typical change or visible
flicker. Actual calibrated rendering, motion sequences, temporal/perceptual
scoring and human appearance review remain unperformed. These are compressed
camera references, not sensor truth.

An independent audit reconstructs every input with clipped source indices
rather than the producer's padding operation. It hashes all saved outputs and
recomputes every score from independently indexed count arrays and integer
squared-error sums. A separately declared CPU follow-up runs the first source
at offsets zero/two-right: both CPU/MPS full outputs differ by at most one
count. CPU aligned translation has maximum 148 counts and RMS 1.86252, so
backend roundoff cannot explain the observed maximum.

## What this changes next

The shift-size pattern is consistent with sensitivity to the network's
sampling grid; it does not isolate a particular layer as the cause. The
[ICML 2019 anti-aliasing study](https://proceedings.mlr.press/v97/zhang19a.html)
shows why downsampling can impair CNN shift consistency and investigates
low-pass filtering before subsampling. That research motivates a matched
architecture comparison here; it supplies no quality result for our editable
Bayer model. Phase-consistency training is another testable hypothesis.

Any candidate must still pass the existing per-camera RAW shadow/highlight/
detail and calibrated rendered guards, then actual sequence evaluation.
Reducing this diagnostic's count changes alone is insufficient: smoothing
or weakening useful detail could also reduce them. The supplied model and
its existing scoped quality approval remain unchanged.

[Independent audit summary](stock-cnn-translation-receipts/summary.json),
[offline evidence](stock-cnn-translation-receipts/evidence.tar.gz) and
[member hashes](stock-cnn-translation-receipts/manifest.json) retain the ten
protocol/code/result/log files. Source media, input variants and full output
arrays remain external at
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_cnn_translation_20261031`.
The archive excludes photos, RAW arrays and checkpoint weights.
