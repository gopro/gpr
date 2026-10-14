# CNN runtime tools

This directory supports registered still restoration, offline 4K cleanup,
offline 8K SR, editable raw packaging, and desktop timelapse rendering. Live
raw4K capture and full-frame 1024 preview on Pi are native paths outside this
directory. Registry scope and ship status remain authoritative; retaining a
candidate does not promote it.

## Entry points and dependencies

- `render_gvid_sr_receipt.py`: .gvid to 8K Bayer; uses
  `mission1_sr_runtime.py` and model definitions in `mission1_sr_models.py`.
  `bench_mission1_sr_8k.py` lives in validation and uses the same runtime
  for timing receipts; it is not a production inference dependency or a
  quality gate.
- `train_bayer_rgb_target_cleanup.py apply`: registered, visually signed-off
  4K Bayer cleanup; registry scope remains offline review-only.
- `upresable_pipeline.py`, `preview_timelapse_fast.py`, and
  `preview_timelapse_fast_sota.py`: editable raw/timelapse rendering.
- Legacy routed PREVIEW, its model/feature helpers, metrics, holdout manifest,
  benchmarks, and detail-residual diagnostics live in the companion
  [validation branch](https://github.com/dcliftreaves/gpr/tree/review/product-validation/tools/cnn).
  They are not an extra stream required by the selected GVID runtime.
- Offline packaging: `package_mission1_sr_receipt.py` and
  `package_mission1_sr_sequence_receipt.py`.
- Model support: `model.py` and the model definitions used by the entry
  points above.

The runtime-only model/feature libraries use runtime names; their former
`train_*`/audit module paths are not compatibility APIs. Architecture classes,
checkpoint config dispatch, state-dict keys, and quality thresholds
are unchanged. Retained architecture branches support existing checkpoint
configs; their presence does not approve additional models. The registered
`train_bayer_rgb_target_cleanup.py apply` CLI retains its registry path.

The approved offline-production 8K model is
`mission1_native12_8k_sr_q4t2_coord_detail_alpha0p5_v1`. Model names, codec
pairings, and review-only scope are documented in
[Reconstruction](../../docs/RECONSTRUCTION.md#approval-boundary).

Outside this directory, retain `tools/gvid_metadata.py`,
`tools/gvid_pack.py`. Native decoder/encoder tools, `gpr_tools`, `gpr2prores`, and
`gpr_mov_tool` provide capture, raw packaging, and ProRes integration.
The ProRes Makefile has no Python training dependency; its weight exporters
use `model.py`. Python dependencies are listed in `requirements.txt`;
rendering also uses macOS `sips`, FFmpeg, and DNG metadata extraction uses
ExifTool.

UPRESABLE/timelapse workflows require `GPR_BUILD_CODEC_UTILITIES=ON` for
`coeff_io_tool`. The 8K GVID renderer needs `fused_decode_cli` from
`GPR_BUILD_PREVIEW_TOOLS=ON`. These desktop tools are opt-in consumers;
native capture and camera preview have no Python/CNN dependency.

The old routed PREVIEW wrapper rebuilds routing from labeled evaluation
receipts and invokes dashboard evaluators. It is preserved for reproducibility,
not presented as a standalone production renderer. Its records remain in the
validation-only `pipelines/research_registry.json`; it is unrelated to the
native camera preview path. Production renderers never merge that registry.

## Portable artifacts

`runtime_paths.py` resolves default data/output paths under
`GPR_EXTERNAL_ROOT`, falling back to the repository root. Relative external
roots are repository-relative. Existing CLI paths and model-specific
environment overrides remain available. Registry artifact paths are relative
to the repository or configured external root; checkpoint hashes, pipeline
identities, and gate baselines are preserved.

Set `GPR_EXTERNAL_ROOT` before invoking renderers to relocate default source,
checkpoint, and receipt trees. UPRESABLE also supports `GPR_MODEL_ROOT`,
and `GPR_BIBO2X_CKPT`.
Sequence packaging requires an explicit `--meta-dng`.

## Historical evidence

[Pinned runtime and research history](https://github.com/dcliftreaves/gpr/tree/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/tools/cnn)
is separate from production dependencies. Historical gate identities remain
lookup-compatible; retaining an identity does not promote its model.
