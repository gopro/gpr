# Review plan: eight feature commits

## Why reshape this PR

The older runtime PR #95 presents eight feature-ordered commits and explains
what users can do. The stock rebuild reached the same broad areas
through 122 development commits before this cleanup: integration, fixes, experiments and receipts
are interleaved. The PR needs a readable product narrative and commit sequence,
while retaining the evidence and current acceptance failures.

That snapshot runs from foundation `d3d96ec` to published head `2741ad8`.

This is the proposed cleanup sequence, **not a claim that history has already
been rewritten**. This documentation pass changes no runtime, test, fixture,
model, build or workflow files. Implementation experiments remain paused.

## Proposed commit sequence

| # | Proposed commit | Reviewer question and contents |
|---|---|---|
| 1 | **Support editable high-bit-depth Bayer stills and metadata** | Can supported still inputs round-trip with the declared calibration? SDK formats, metadata/ownership, presets, still tools and their tests. |
| 2 | **Preserve stock VC-5 compatibility and add encoder preview hooks** | Are stock payloads and decoder counts preserved where claimed? Encoder/decoder portability, bounds/alignment, optimized paths, row/coefficient APIs and independent comparisons. |
| 3 | **Add GVID framing and bounded RAW-video capture** | Are frames, leases, timestamps, errors and completion handled correctly? GVID/readers, copy/lease capture, replay frontend, recovery and lifecycle tests. |
| 4 | **Add calibrated native preview and bounded streaming reconstruction** | Can a recording be previewed without a camera CNN? Calibration/orientation, queue ownership, streaming inverse, bounded scratch, row work sharing and pixel/concurrency tests. |
| 5 | **Add editable video extraction, MOV interchange and ProRes outputs** | Can recordings reach desktop tools while preserving the declared payload/timing/metadata? Editable export, MOV, PCM/timecode, renderer/ProRes and integration tests. |
| 6 | **Add optional FFmpeg decoding for declared stock video profiles** | Can a pinned patched FFmpeg decode the supported formats? Adapter, build script, configuration/format/corruption tests and limitations. |
| 7 | **Add optional offline cleanup and 2x reconstruction workflows** | Do explicit model contracts and backends work, and what quality is actually validated? Portable/native engines, checkpoint assets, still/sequence/upres orchestration and model/media tests. |
| 8 | **Package and document the stock RAW-media suite** | Can others build, install, reproduce and review it? Profiles, relocated public consumers, hosted CI, user docs, feature accounting and indexed evidence. |

Tests belong with the feature they establish. CMake changes must be divided by
the feature targets they enable, rather than deferred wholesale to the last
commit. Shared helpers need to precede their consumers. The final packaging
commit ties the components together; it must not hide functional code changes.

Measured production optimizations belong inside their owning encoder/preview
commit. Failed proposals remain clearly labelled evidence. Their chronology
should not become dozens of separate product commits, and their results must
not be silently removed or relabelled as successful measurements.

## Safe history cleanup procedure

1. Freeze and preserve the current published head on a backup ref before any
   history replacement. Leave fork #95/#96 and upstream #71/#72 untouched.
2. Build the eight-commit series on an isolated review branch. Split shared
   build changes with their features and keep source/tests together.
3. Compare the final runtime, tests, fixtures, models, build and CI files against
   the frozen head byte for byte. Presentation changes must be separately
   identifiable. Preserve historical commit links and evidence archives.
4. Verify intermediate build dependencies and the final package/public-client
   checks. The final tree needs current CI; old green checks remain bound to
   their original revision.
5. Replace the existing fork PR branch only after the candidate series and its
   comparison are concrete and verified. Use a lease so a concurrent remote
   update cannot be overwritten. Keep one replacement PR, not another feature PR.

The eight subjects above are a review plan, not eight new implementation tasks.
The goal is to make already-written work understandable without changing its
behavior or concealing the remaining timing/model-quality gates.

## Keep four things separate

- **Delivered behavior:** the three workflows in the [feature overview](stock-raw-media-overview.md).
- **Acceptance:** compatibility, correctness, native Pi deadlines and model-quality requirements in the [ledger](stock-rebuild-requirements.md).
- **Evidence:** reproducible receipts, rejected trials and revision-specific measurements in the [evidence guide](stock-evidence/README.md).
- **Future implementation:** unpublished prototypes and additional optimization work. These stay outside this presentation cleanup.

Do not use feature coverage or a percentage estimate as proof that all
acceptance criteria pass. The native combined timing and useful broad 2x /
temporal/restored-tier work remain open until verified or explicitly rescoped
by the user.
