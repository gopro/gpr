# Recovered Mission 1 calibration and matched mixed-camera training

Per-frame calibration is now recovered for the 42 archived Mission 1 count
references, with ten records independently corroborated by a separate
source-GPR parameter archive. This enables a stock-Q3 camera training dataset
without inventing CFA, black/white levels or white balance. Four matched
training cases complete 3,200 updates, but all eight checkpoints fail Nikon
shadow guards. No new model is selected.

The work also fixes a concrete still-workflow bug: the SDK CLI parameter
reader's fixed 16 KB gain-map buffer cannot hold the camera's 16,988-byte
65x65 maps. The map stream now uses storage sized to its declared capacity;
parsed JSON ownership is also released. Camera encoding and preview code,
stock payload semantics and supplied CNN weights are unchanged.

## Calibration provenance and dataset

All 42 parameter records agree with the archived count inventory on source
stem, timestamp, ISO and full 8192x6144 geometry. Their SDK pixel format 2 is
RGGB14, as verified against the enum source. Each has black level 16, white
level 16383, per-frame white balance/color matrices and four full-frame Bayer
gain maps. Ten separate parameter files previously extracted from source GPRs
have exactly matching tuning and profile dictionaries.

This recovers archived calibration, not the unavailable original GPR files.
The SDK JSON has printed numeric precision; original binary opcode/float
identity is not established. The remaining associations rely on archived
identity records. The new DNGs repackage cached camera-GPR count references,
not lossless sensor truth or freshly decoded original camera files. Archived
date/ISO checks do not imply every EXIF field is reproduced by the SDK writer.

The previous conservative venue split is retained: 28 playground references
for training, six home-interior references for validation and eight lake
references reserved outside the patch arrays. They are three coarse venues,
not 42 independent scenes. Old model pretraining overlaps this historical
corpus; no blind-test or clean generalization claim is made.

For the 34 non-test references, independent TIFF and current SDK reads of
each repackaged DNG reproduce the cached RAW bytes exactly. Same-site 2x2
integer half-up averaging generates 12 MP inputs, which pass through actual
stock Q3 encoding/decoding in the original count domain. Supported simulated
low-to-high calibration roundtrips exactly relative to the recovered archive
values. Opaque XMP follows the declared omission policy.

A tag-by-tag check also exposes a legacy SDK profile limitation: it does not
carry the archive's baseline exposure or analog balance into its generated
DNG. The corrected dataset producer restores those fields with the existing
typed metadata writer, then verifies exposure, balance, white balance and both
color matrices against every archive. This is an explicit metadata-layer
replacement for fields absent from the stock SDK profile struct.

The initial dataset and training receipts remain intact. The corrected
producer regenerates all 34 non-test pairs, and both complete patch arrays
are byte-identical to the initial arrays. Coordinates, black/white levels
and AsShotNeutral consumed by the completed training run are also unchanged.
That run's approximate loss does not consume baseline exposure or analog
balance, so its results are reused with the
[explicit carrier proof](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-calibrated-receipts/camera-data-v2/carrier-proof.json),
not relabelled as a rerun. Future actual rendering uses the corrected metadata.

Thirty-two patches per non-test source produce **896 training and 192 camera
validation patches**. An independent audit verifies every input/target crop
against the complete cached source and decoded stock low, including absolute
coordinates and all four Bayer sites. Eight historical-test sources have
zero patches; their bytes are hashed but never decoded into the patch arrays.
The [pair audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-calibrated-receipts/camera-data/pair-audit.json)
binds all 1,088 pairs. Arrays, DNGs and original metadata files remain external.
The [corrected pair audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-calibrated-receipts/camera-data-v2/pair-audit.json)
independently checks all 1,088 regenerated pairs as well.

## CLI gain-map correction and verification

The first source-repackaging attempt aborts on GP017346. A fixed-command
reproduction records exit -6 and no DNG output; it binds the original
executable, parameter and parser-source hashes. The parser advertised a
16,988-byte stream over a 16,000-byte stack array. It now allocates the
backing storage to the declared map size. No maps are dropped or truncated.

The new still-suite regression independently writes DNGs with four distinct
Bayer maps, dumps SDK parameters, reapplies those parameters to RAW, and
checks both decoded pixels and complete opcode bytes exactly. It covers
9x9, 65x65 and 97x97 map grids, including the former overflow boundary.

The five-method still suite and focused `sdk_c_api`, `raw_media`, `stock_still`
CTest targets pass on Mac and Pi 5. Final source/binary hashes and logs are in
the [validation receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-calibrated-receipts/validation/receipt.json).
The Pi capture executable remains byte-identical at `c576f566…`; only the
CLI parameter-reader source and regression test are copied, and only
`gpr_tools` is rebuilt on Pi. No capture scheduling or system setting changes.

The dataset was produced after the buffer fix and before the final JSON
ownership cleanup. That exact producer executable/source is retained
externally and hash-bound; final CLI tests rerun after cleanup. The source
failure, initial protocol/logs and completed run are separately preserved.

## Controlled calibration and tone-loss experiment

The new camera pairs are combined with the existing audited Nikon pairs:
eight Nikon training sources and four Nikon validation sources. Each update
samples four distinct training sources from each camera domain, one patch
per source. All four cases start from the same historical-width local-shadow
checkpoint and share seed 20261108, 800 updates, batch eight, AdamW learning
rate 0.00003 and checkpoint steps 400/800. There is no gate, strength sweep,
parameter interpolation or new shadow hinge in this comparison.

Two binary factors are tested:

- **Sensor canonicalization:** after actual stock decoding, map each sensor's
  black/white range to the Nikon canonical range 1008/15892 before the old
  network, then invert that affine mapping on output. The original count-domain
  codec damage is retained. Nikon input mapping is exactly identity. Independent
  float32 checks roundtrip all 16,384 counts exactly for both sensor ranges.
- **Tone/WB proxy:** add a fixed-weight 0.1 loss after black/white normalization,
  per-source white balance, mean-green Bayer RGB construction, clipping and
  square-root tone mapping. This approximates sensitivity to dark/color errors;
  it is not actual demosaicing, a full color/gain-map pipeline, CIRAWFilter or
  a perceptual-quality measure.

Both factors retain the historical Charbonnier, gradient, Laplacian,
detail-phase and supervised preclean losses. Preclean targets are expressed
in the network's input domain for canonicalized cases. The reference targets
remain archived/source counts rather than clean noise-free truth.

Every checkpoint is evaluated on all **320 validation patches**. Selection
requires unchanged pooled/per-group RAW overall/shadow/highlight/detail
nonregression and strict pooled improvement, plus nonregression of tone-proxy
MSE in each group. The two Nikon baseline groups reproduce the prior exact
baseline. These mixed-corpus gains cannot be compared directly with the
older Nikon-only or single-GoPro gains.

| Step-800 case | Pooled RAW gain | Tone-proxy gain | Nikon shadow MSE changes, forest / sky |
|---|---:|---:|---:|
| Original counts, original loss | +1.44940 dB | +0.39161 dB | +3.61% / +18.92% |
| Original counts, tone loss | +1.45025 dB | +0.39540 dB | +4.52% / +23.70% |
| Canonicalized counts, original loss | +1.50698 dB | +0.30283 dB | +2.74% / +18.14% |
| Canonicalized counts, tone loss | +1.50879 dB | +0.30338 dB | +3.17% / +22.40% |

All eight checkpoints fail the same two Nikon shadow-group checks. Every
tone-proxy group improves, and the camera validation shadow MSE is reduced,
but those pooled successes cannot erase the Nikon regressions. Canonicalization
modestly improves RAW error under this budget while giving a weaker tone-proxy
gain than the original-count control. The extra tone term slightly improves
its proxy but worsens Nikon shadows. Neither is demonstrated as a complete
quality solution.

No candidate advances to complete-image rendering, frozen selection,
historical finals or the supplied GoPro evaluation. No new standard/native
backend or Pi CNN performance is claimed. This is new training/data evidence,
not an update to the previously frozen boundary/gate diagnostic.

## Reproducibility and remaining scope

The [delivery audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-calibrated-receipts/audit.json) independently
recomputes every checkpoint's RAW/proxy verdict and gain, checks checkpoint
hashes/metadata, dataset hashes, source counts, affine roundtrips and baseline
agreement. The [manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-calibrated-receipts/manifest.json) binds
protocols, code, source-calibration summaries, training results, the actual
CLI failure and Mac/Pi validation. Source photos, arrays and model weights
are not uploaded.

The next inference/training comparison can now use per-frame sensor context
instead of treating black 16 and black 1008 as the same signal range. Shadow
control still needs to qualify across complete images, and actual rendered
appearance must be checked separately from the approximate tone proxy.
Useful restored detail, temporal behavior and broader quality approval remain
open. The main native-12 MP combined capture/preview timing gap is unchanged.
