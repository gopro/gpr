# Quality Gates

`gates.json` defines committed STILL, VIDEO_FREEZE and PREVIEW thresholds.
`test_set.json` fixes four source images, evaluation dimensions and crops.
Every image must pass; an average cannot hide a failing image.

## Run

Source paths resolve relative to `GPR_EXTERNAL_ROOT` (the repository by default).
Place original fixtures at the paths in `test_set.json`, or supply
`GATE_SOURCE_PATH_MAP` as described by the runner. External model identities
and supported combinations live in [the registry](../../pipelines/registry.json).

```sh
python3 tests/quality_gates/check_registry_consistency.py --strict-artifacts
python3 tests/quality_gates/run_gate.py 'codec=...+cnn=...+demosaic=...'
python3 tests/quality_gates/audit_ship_pipelines.py --strict
```

Use a full registry key, not the ellipsis above. Missing fixtures or checkpoints
mean the image gate has not run. Hosted CI checks registry structure and saved
signoffs; it is not a substitute for inference on the original images.

[pipelines/preview_holdout_set.json](../../pipelines/preview_holdout_set.json)
is the canonical broader informational set. Alternate sets
cannot replace the frozen gate or authorize `--claim`. External-receipt-only
paths have a separate runtime evaluation; the registry audit checks declarations,
not the contents of unmounted external dashboards.

## Evidence

`runs/*/run.json` preserves measured metrics and original run identities.
The eight approved baseline receipts support the required signoffs. Historical
run IDs are not hashes of the current JSON bytes; consult each receipt's
provenance fields. Metrics and recorded verdicts are unchanged.
Generated crops and visual differences are not bundled with these JSON receipts.
Regenerate them for visual review rather than treating absent media as verified.

[Quality signoffs](../../docs/claims_log.md) and
[release evidence](../../docs/release_evidence.json) summarize retained results.
Experimental diagnostics and iteration reports are preserved on the
[pinned archive snapshot](https://github.com/dcliftreaves/gpr/tree/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/tests/quality_gates/runs),
including the seven removed experimental receipts. They are not current
required signoffs. The ledger separately labels two older entries lacking
committed receipts as historical, not current approval evidence.

Changes to thresholds belong in a separate, justified review, never in a cleanup
or as a way to make a failing model pass.
