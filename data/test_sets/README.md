# Test Fixtures

This directory is a local fixture location, not a bundled camera corpus or
a promise of fixed test durations. Acquire original camera files from their
owner or an authorized artifact bundle; this checkout does not establish
public download availability. Preserve source identity and metadata.

The frozen quality corpus is defined by
[tests/quality_gates/test_set.json](../../tests/quality_gates/test_set.json):
Z8Z_0001, Z8Z_0067, Z8Z_5323, and Z8Z_6693. Place those original DNGs at the
listed relative paths under `GPR_EXTERNAL_ROOT`, or use the runner's
`GATE_SOURCE_PATH_MAP` prefix mapping. Do not substitute images or alter crops
to obtain a passing result. See [Quality Gates](../../tests/quality_gates/README.md)
for model and execution prerequisites.

The broader informational preview set is
[pipelines/preview_holdout_set.json](../../pipelines/preview_holdout_set.json).
Capability and real-fixture tests may require other camera sources; inspect
the selected test's input paths and environment options before running it.
Large sources and trained weights are external and must be supplied separately.

Record fixture provenance, hashes, dimensions, bit depth, and acquisition
conditions with new results. Missing fixtures mean skipped or blocked work,
not a pass. Synthetic conformance/fuzz inputs exercise parser and codec
behavior; they do not replace real-camera quality evidence. Historical
smoke/medium/full counts and duration estimates are not current guarantees.
