# Testing Methodology

Use the test appropriate to the claim. Passing container validation, codec
regression, a rendered quality gate, or a hardware timing test establishes
different facts.

## Current entry point

From the validation checkout root, with CMake and a C/C++ toolchain:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DGPR_TEST_CODEC=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 tests/quality_gates/check_registry_consistency.py
python3 tests/quality_gates/audit_ship_pipelines.py --strict
```

No validation targets are built by default. `BUILD_TESTING=ON` enables the
test framework, not every suite. Select independent groups explicitly:

| Option | Scope | Production prerequisites |
|---|---|---|
| `GPR_TEST_CODEC` | Fused bands, edges, entropy, pool reuse | Shared codec; POSIX test drivers |
| `GPR_TEST_STILLS` | Quality comparison tool and metadata roundtrips | SDK and still apps |
| `GPR_TEST_CONTAINER` | GVID validity | Video or preview container support |
| `GPR_TEST_CAPTURE` | Queue, abort, callback and full capture chain | Video capture |
| `GPR_TEST_PREVIEW` | Small fixture encoder and RGB preview tests | Preview tools; no capture or SDK |
| `GPR_TEST_RECONSTRUCTION` | Synthetic tiled SR contracts | Python/Torch; no native test binaries |
| `GPR_TEST_RESEARCH` | Separate ANS prototype, coefficient benchmark, residual diagnostic | Shared codec; POSIX for residual tool |

CTest labels are `codec`, `stills`, `container`, `capture`, `preview`, and
`reconstruction`; for example, `ctest --test-dir build -L codec`. Tests with
Python dependencies require the packages used by the CI workflow. Contradictory
suite/feature selections fail at configure time. `GPR_BUILD_DIAGNOSTICS` and
`GPR_BUILD_CODEC_UTILITIES` independently opt into runtime evaluation tools;
the full integration script requires both. CI explicitly selects supported
runtime suites, while profile tests build and exercise codec-only and preview-only
subsets. The separate adoption test resolves the eight current production
feature commits and exercises checkpoints and cherry-picked subsets; it does
not substitute archived implementations for the current feature series.
Native research diagnostics remain off in normal CI; the manual Windows
workflow explicitly builds its ANS prototype smoke. Legacy CLI binaries are
built only in the opt-in utilities profile, not every regression job.

Desktop backend isolation is checked on macOS with
`GPR_TEST_DESKTOP_BUILD_PROFILES=1 bash tools/test/test_desktop_build_profiles.sh`: default/no-CNN, CoreML,
Metal/MPSGraph, both, then no-CNN again without cleaning. This tests linking,
selection and ordinary RAW rendering, not trained-model quality.

Inspect skips and prerequisite failures separately from passing tests.
The strict audit checks retained signoffs, not fresh model inference. Check
external production artifacts separately before making a release claim:

```bash
python3 tools/verify_production_artifacts.py --strict
python3 tests/quality_gates/audit_ship_pipelines.py --strict-artifacts
```

Artifact-backed checks require external files. Normal CI remains usable without
private weights. The legacy offline PREVIEW workflow is now research-only;
its receipt lacks a pinned hash and is not independently verified or part of
the production release audit. The production artifact verifier
and ship audit read only the supported registry. Gate/consistency/dashboard tools explicitly merge
`pipelines/research_registry.json` through `tools/validation_registry.py` for
historical lookup; production renderers never perform that merge.
The full integration suite is `bash tools/test/run_all_regressions.sh`; install
the Python and platform dependencies listed in the CI workflow first.

| Evidence | What it establishes | What it does not establish |
|---|---|---|
| `tools/test/test_gvid_conformance.py` and C stream tests | Container accept/reject and recovery behavior | Payload quality or camera integration |
| `tests/conformance/` | Manual bitstream comparison infrastructure; no current committed goldens | A passing release gate or universal perceptual quality |
| `tools/test/test_capabilities.py` | Declared codec cells and regression limits | Every end-to-end workflow or sensor |
| `tests/quality_gates/run_gate.py` | The selected pipeline/class on its specified corpus | Other codecs, models, cameras, or output objectives |
| Target capture/preview receipts | Throughput, memory, drops, storage, and identity on that setup | Camera readiness when the target is a stand-in |
| Release bundle verification | Hashes, inventory, and included stream validity | Fresh hardware execution or new quality measurements |

## Quality signoffs

Read `tests/quality_gates/gates.json` for thresholds and
`pipelines/registry.json` for exact pipeline identities.
[claims_log.md](claims_log.md) contains audited machine-written signoffs;
each current required signoff must resolve to its matching `runs/<hash>/run.json`.
Two older ledger entries without committed receipts are labeled historical
and do not establish current approval.
Do not hand-edit measurements or infer a new pass from prose.

Compare the entire stated pipeline, including codec, model, preprocessing,
demosaic, metadata, and render settings. Report worst-image behavior where
the gate requires it. Bayer PSNR evaluates editable raw reconstruction;
LPIPS and rendered metrics describe a particular render. Neither alone
establishes editing latitude or physical sensor-noise fidelity.

## Timing and reproduction

Record source commit, build configuration, input identity, dimensions, format,
quality/profile, hardware, storage/flush policy, frame count, and output hashes.
Separate end-to-end wall timing from kernel or loop timing. Best-of-three
single-image results are not sustained capture rates.

Full model-backed quality checks require external fixtures/weights and the
appropriate backend hardware. Missing prerequisites are a skip or blocker,
not a pass. Hosted CI cannot substitute for an actual camera-role
sensor/DMA, storage, and display receipt.

[Pi Hardware](PI_HARDWARE.md) separates current replay verification from
historical measurements. [GVID Conformance](GVID_CONFORMANCE.md)
lists focused container checks; [Capture API](CAPTURE_API.md)
defines hardware evidence.
