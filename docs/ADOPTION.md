# Review and Adoption

Review aid only, not product status. The contribution
was split into two reviews on this fork:

- `review/product-runtime`, based on GoPro `446c736`: SDK, recording,
  preview, desktop reconstruction, examples, and usage documentation.
- `review/product-validation`, based on the production branch: tests,
  fixtures, diagnostics, quality gates, and hardware validation tooling.

Review and merge production first. Then retarget the validation PR to the
same integration branch; its diff should continue to contain only validation.
Confirm the current PR base and diff before merging; branch tips and commit
maps may change during cleanup.

## Production Features

Each production commit includes its final fixes and build controls, not an
early implementation that needs later cleanup commits. Commit messages carry
a unique `GPR-Feature` trailer; adoption tests resolve these identities from
the current series rather than pinning an archived implementation.

| Order / Feature ID | Feature and prerequisite | Build control | Matching validation |
|---|---|---|---|
| 1 `stills` | High-bit-depth stills, metadata, shared codec; GoPro baseline | `GPR_BUILD_SDK`, `GPR_BUILD_STILL_APPS`; legacy CLI opt-in | `GPR_TEST_STILLS` |
| 2 `fused` | Fused Bayer codec; shared codec | Core library; `GPR_BUILD_CODEC_UTILITIES`; `GPR_BUILD_PREVIEW_TOOLS` for `fused_decode_cli` | `GPR_TEST_CODEC` |
| 3 `capture` | GVID framing and bounded capture; fused codec | `GPR_ENABLE_VIDEO`; `GPR_BUILD_DIAGNOSTICS` opt-in | `GPR_TEST_CONTAINER`, `GPR_TEST_CAPTURE` |
| 4 `preview` | Full-frame native preview; codec and GVID | `GPR_BUILD_PREVIEW_TOOLS`; capture can be OFF | `GPR_TEST_PREVIEW` |
| 5 `desktop` | MOV and ProRes; codec and GVID, not native preview | Separate Makefiles; `CNN_BACKENDS=none/coreml/metal/both` | Desktop output and backend tests |
| 6 `ffmpeg` | Optional FFmpeg decoder; codec and MOV interchange | Separate `configure_and_build.sh` | FFmpeg integration tests |
| 7 `reconstruction` | Selected still/4K/8K reconstruction; fused tools and GVID; UPRESABLE timelapse/both modes also require desktop MOV tools | Explicit Python entry points and dependencies | `GPR_TEST_RECONSTRUCTION`, registry and model contracts |
| 8 `integration` | Packaging, examples, visual overview | Composition of preceding features | CI, adoption, release evidence; research diagnostics separately opt-in |

Shared prerequisites are intentional: a flag selects compiled implementation,
not necessarily one isolated source file. GVID readers keep framing support
when capture is disabled. Optional desktop backends arrive with the desktop
feature, not with a later model or cleanup commit.

Native choices are documented in [Getting Started](GETTING_STARTED.md);
desktop choices are in [Renderer Usage](../tools/gpr2prores/USAGE.md).
No test suite is enabled by default; see [Testing Methodology](TESTING_METHODOLOGY.md).

Validation covers Linux, macOS, and Pi, not native MSVC. The fused codec still
has POSIX dependencies with video/preview disabled; those flags are not a
Windows portability switch.

From the validation branch, `bash tools/test/test_feature_adoption.sh` checks
the new checkpoints and partial combinations. It uses current test drivers
in disposable worktrees, never archived runtime implementations or files
copied from the complete production tree to fill missing dependencies.
Temporary data stays under `TMPDIR`. These are synthetic/build regressions,
not fresh trained-model quality gates or camera firmware evidence.

The pre-consolidation tips remain at
`archive/before-feature-series-2026-10-14-runtime` and
`archive/before-feature-series-2026-10-14-validation`. Older experiments remain
on `archive/research-and-integration-2026-09-09`.

## Feature Cohesion

The pre-cleanup tree is preserved at
`archive/before-feature-cohesion-2026-10-13` (`a956fcd`).

| Area | Current disposition | Verification boundary |
|---|---|---|
| Still SDK, metadata and high-bit-depth Bayer | Production | Synthetic roundtrips and metadata tests; camera corpus is separate |
| Fused codec, GVID, capture and native preview | Production | Native, container and source-simulator tests; no Mission 1 firmware claim |
| Selected restoration and SR | Production inference helpers separated from training/benchmark names | Synthetic model contracts do not replace approved checkpoint gates |
| MOV and ProRes | Production; portable reader shared by both frontends | Cross-frontend payload tests and macOS rendering |
| FFmpeg decoder | Retained optional integration pinned to FFmpeg n8.0 | Fresh Bayer encode, MOV wrap, native-vs-FFmpeg decode, image/ProRes export and malformed headers |
| Standalone CoreML polish API | Archived; no selected model or production caller | Removed rather than implying its availability test exercised inference |
| Minimal fast TIFF still writer | Archived; the retired switch warns and uses the SDK writer | SDK readback regression; separate fast decoder retained |
| Detail-residual sidecar experiment | Validation only | Not part of selected GVID or reconstruction formats |
| Separate-stream ANS prototype | Validation-only diagnostic and fuzzer | No production callers; active codec uses joint ANS |
| Legacy routed offline PREVIEW | Validation-only evaluator workflow | Rebuilds routing from labeled receipts; not the native camera preview |
| Historical/unselected model records | Validation-only research registry | Original identities/configs/hashes preserved; explicit merged lookup |

`tools/test/audit_feature_coverage.py` inventories changed source against exact
commits. LinuxRelease collects native and Python execution coverage in the
existing CI job and uploads `coverage/feature-coverage.json`. Static caller
references are hints, not execution evidence. Uninstrumented backends remain
UNKNOWN; zero coverage is a reason to investigate, not automatic permission to
delete a public API. Instrumented timings are not performance receipts.

Read coverage by scope: LinuxRelease measures CPU C/C++ and Python execution;
macOS rendering and the patched FFmpeg integration run in the other jobs without
line instrumentation. Metal/ARM assembly, supplied model exports, and camera
hardware are not covered by that Linux report. Declaration-only headers and
build scripts can also be UNKNOWN without being abandoned. The capability
report marks instrumented timing/RSS as PARTIAL while still enforcing quality
and size; uninstrumented jobs retain their normal performance criteria.

The shared Python GVID indexer scans headers without loading payloads; preview
uses the same parser and v1 checks. Cross-frontend MOV tests cover sparse EXIF,
truncated packets and sidecars, and stale metadata on overwrite. None of these
synthetic regressions substitutes for the external trained-model gates or a
new Pi/Mission 1 performance run.
