# Reconstruction and Model Availability

Raw-video reconstruction includes offline-production 8K alpha0p5 SR and
visually approved, review-only 4K cleanup on desktop hardware.
These paths can produce editable Bayer DNG/GPR
outputs and ProRes review media. It does not run on the camera and does not
establish real-time 8K processing.

## What is available

The checkout contains codec and reconstruction integration code, pipeline
definitions, and evidence references. Trained checkpoints, exported model
blobs, large camera fixtures, and full review movies are external artifacts.
A registry entry or archived receipt is not a downloadable model package;
this documentation does not establish a public weight download.

Without a matching model package, use the codec-only `--no-cnn` review
command in [Renderer Usage](../tools/gpr2prores/USAGE.md). Do not substitute arbitrary
weights and carry over the approved pipeline's quality claim.

## Model package contract

A reproducible model delivery must include:

- Checkpoint/export hashes, model identity, license, and source provenance.
- Architecture and backend/export version, expected Bayer phase and bit depth,
  normalization, codec profile, scale, and supported dimensions.
- Preprocessing, tiling/overlap, metadata, and color/render settings.
- Matched source/output evidence, quality receipt, timing and peak-memory
  receipt, editable-packaging checks, and review assets.

The historical F and BIBO family names alone do not identify an approved
Mission 1 candidate. Select the exact codec/model pairing from the evidence
and pipeline definitions. A raw gate, rendered gate, and editor-latitude test
measure different outcomes.

## Desktop integration example

For a supplied compatible Metal 1x model package and matching source metadata:

```bash
make -C tools/gpr2prores CNN_BACKENDS=metal
tools/gpr2prores/gpr2prores \
  --meta-dng data/sample.dng \
  --ckpt "${GPR_MODEL_ROOT:?Set the matching exported model directory}" \
  --cnn-backend metal --cnn-scale 1x \
  --demosaic core-image --out-resolution uhd \
  "$GPR_ARTIFACT_ROOT/clip.gvid" "$GPR_ARTIFACT_ROOT/cleanup-review.mov"
```

This illustrates the CLI integration, not reproduction of a locked receipt.
Use `--cnn-scale 2x --out-resolution 8k` only with a compatible 2x export.
Set `GPR_ARTIFACT_ROOT` to the output directory you want to use. Review media is
rendered output; editable raw requires the matching raw packaging workflow
and metadata validation.

## Approval boundary

The [pipeline registry](../pipelines/registry.json) distinguishes:

- `mission1_native12_8k_sr_q4t2_coord_detail_alpha0p5_v1`:
  `production_scope=offline_production`, approved for offline reconstruction.
- `mission1_native12_4k_cleanup_rgb_cfa_w40_v1`:
  `production_scope=offline_review_only`, with status
  `review_only_visual_signed_off_camera_handoff_separate`.

Both use codec `mission1_native12_t233` and demosaic `sips_via_gpr_tools`.
Visual approval does not automatically promote registry scope. Historical and
unselected candidates live in the validation-only `research_registry.json`,
not the production renderer's registry. Their identities and evidence remain
lookup-compatible through validation tooling. Neither path is live-camera SR.
The
[compact release evidence index](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/release_evidence.json) identifies
existing receipts and review artifacts. Architecture/load tests do not
replace those trained-model quality measurements.

The Python 8K renderer shares checkpoint loading and tiled inference through
`tools/cnn/mission1_sr_runtime.py`, with architecture/config construction in
`mission1_sr_models.py`; it does not import the benchmark CLI. The legacy
routed PREVIEW evaluator and its model/input helpers live on the validation
branch; native camera preview does not use them.
These runtime module names do not change approved model identities, hashes,
checkpoint state dictionaries, config interpretation, or quality thresholds.
The registered 4K cleanup `apply` entry point is unchanged. See the
[CNN runtime tools](../tools/cnn/README.md) for the supported dependency graph.

Artifact paths in that index are relative to the user-selected
`GPR_EXTERNAL_ROOT`. This locates existing evidence; `GPR_ARTIFACT_ROOT`
chooses where new local outputs are written. Set each
explicitly for the workflow rather than assuming a shared filesystem layout.

Premium still SR is not a promoted replacement for these video models.
[Historical model evidence](https://github.com/dcliftreaves/gpr/blob/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/docs/RECONSTRUCTION.md)
is preserved separately from the runtime contract.
