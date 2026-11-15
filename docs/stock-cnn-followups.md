# Sensor-context gating and frozen cleanup-to-SR composition

Neither follow-up qualifies a replacement model. Sensor-aware gating leaves
the newly trained models' Nikon shadow regressions unresolved, and the only
complete-image survivor renders slightly worse than the supplied model.
Naively chaining the existing cleanup and SR models improves rendered Nikon
fidelity but regresses all six camera validation images. Production weights
and inference defaults remain unchanged.

## Sensor-context gate

The fixed comparison covers the eight calibrated mixed-camera checkpoints
at strengths 1 and 1/2, using the previous strong gate after mapping the input
bicubic counts from each sensor's black/white range into canonical 1008/15892
counts. Two controls use the earlier checkpoint with its absolute-count and
canonical-count gates. There are 18 settings, no new thresholds or training.
Inference uses only the input and declared sensor calibration.

All settings run on all 320 validation patches, with unchanged RAW/proxy
guards. The eight newly trained checkpoint controls exactly reproduce their
parent study's ungated scores. An independent all-14-bit-count check shows
that the canonical gate is exactly the old gate for Nikon calibration.

Only the two older-checkpoint controls qualify at patch level. Both run on
all ten complete validation images, producing 20 count outputs. The absolute
gate passes every RAW/proxy check at +0.95279 dB pooled active gain versus
bicubic. The sensor-aware gate gains +0.91549 dB but fails camera-image shadow
and tone-proxy checks. Changing coverage does not establish that the newly
activated corrections are reliable.

The sole full-image survivor then undergoes actual rendering on all ten
sources, compared with the supplied BIBO model. All reference, bicubic,
supplied-model and candidate renders are fresh 16-bit RGBA Rec.709 TIFFs
with matched metadata/settings. Selection requires nonregression against the
supplied model's RGB and Y-prime MSE on every source, followed by strict pooled
RGB improvement. The candidate fails eight source/metric comparisons and has
**-0.01727 dB pooled RGB gain versus the supplied model**. None is selected,
frozen or sent to final-reference evaluation.

## Existing cleanup followed by existing SR

This separately frozen experiment composes unchanged checkpoints:
`9de03f82…` stock 1x cleanup, then `f7bda044…` stock 2x SR. Both use their
existing whole-frame tiled runtime. The cleaned low is rounded uint16 RAW;
there is no intermediate codec encode/decode, model tuning or strength search.
The comparison includes all four Nikon and six camera validation sources.

Every output must retain RAW regional fidelity against both original bicubic
and SR-only, then pass actual rendered RGB/Y-prime checks against SR-only.
The composition fails RAW checks on all ten images. Active gain versus SR-only
is +0.00544 to +0.04384 dB on Nikon, but -0.02149 to -0.09048 dB on the six
camera images. Shadow/highlight/detail regressions remain explicit.

Diagnostic rendering is completed despite that disqualification; it cannot
override RAW failures. Fresh cascade renders use hash-verified reference and
SR-only TIFFs from the same renderer/settings. Rendered RGB improves by
+0.19196 to +0.33632 dB on the four Nikon images, but regresses by
-0.02922 to -0.37117 dB on every camera image. Pooled RGB gain is +0.04383 dB,
which does not erase the per-image failures. No pipeline is promoted.

The initial metadata assertion stops after first-image inference because
simulating half size and restoring full size can reduce EXIF rational
numerator/denominator representations. The values are unchanged. The corrected
check compares exact fractions for rational records and requires identical
code/type/count and all other bytes. Both first-image inference outputs remain
byte-identical to the initial attempt. Every completed editable DNG matches
its declared derived metadata and independently read output counts exactly.

## Evidence and implications

The [audit](stock-cnn-followup-receipts/audit.json) verifies qualified-family
coverage, output hashes and RAW/proxy decisions. It independently recomputes
30 rendered RGB/Y-prime scores using separate per-channel sums and expanded
cross terms; all agree within 1e-10 relative tolerance. The
[compressed evidence](stock-cnn-followup-receipts/evidence.tar.gz) contains
protocols, code, full numeric reports and renderer receipts, indexed by the
[member manifest](stock-cnn-followup-receipts/manifest.json). Photos, RAWs,
TIFFs and model weights remain external. The archive keeps this diagnostic
material compact in the feature diff.

These are reused validation images and archived camera references, not blind
or lossless sensor truth. Actual rendering uses the corrected camera metadata
carrier, including recovered exposure and analog balance. No new Pi CNN timing,
native backend, temporal or restored-detail approval follows.

The results distinguish single-model validation from compositional validity.
If cleanup-before-SR is pursued further, training must explicitly evaluate
the cleaned-input distribution and resulting complete pipeline. More isolated
RAW/proxy gains or changing gate coverage alone are not evidence of better
rendered quality. The original supplied models and their existing scoped
validation remain available individually.
