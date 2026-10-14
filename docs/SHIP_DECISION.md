# Ship Decision

## Approved scope

| Workflow | Decision |
|---|---|
| Raw stills | Legacy VC5 q0 smallest and q3 primary with the matched still CNN; q8 archival without CNN |
| Native 4K raw-video capture | October 13 Pi 5 buffered replay exceeds 20 fps; SSD fsync is below 20 fps; SD and actual camera production remain unproven |
| Camera-back preview | Full-frame 1024 x 768 RGB from the same 4096 x 3072 stream; no camera-side CNN |
| Offline 8K SR | `mission1_native12_8k_sr_q4t2_coord_detail_alpha0p5_v1`: existing `offline_production` approval only |
| Offline 4K cleanup | `mission1_native12_4k_cleanup_rgb_cfa_w40_v1`: visually approved, registry `offline_review_only`; not promoted |
| Premium still SR | Separate research; no production promotion inferred from video approval |

The FUSED encoder serves video. The legacy VC5 encoder serves the approved
still tiers. Do not transfer quality claims between encoder families or
between source-resolution, decimated, and reconstructed workflows.

Both reconstruction entries use `codec=mission1_native12_t233` and
`demosaic=sips_via_gpr_tools` in the [registry](../pipelines/registry.json).
The PSF-focus step0075 candidate remains review-only, not the approved 8K
alpha0p5 baseline. No CNN runs on the camera; offline review playback is not
a real-time reconstruction claim.

## Current Pi verification

Production `fdb9171`, verified October 13, 2026: 120 fresh encodes of
GP017601 at 4096 x 3072, identical to the previous build. Buffered throughput
was 23.8818 fps and SSD fsync throughput 17.6722 fps (baseline 23.8236 and
17.6332 fps); peak RSS was 55,440 KiB. These are file-backed Pi stand-in
results, not SD-card 20 fps proof or sensor/DMA evidence. Strict 24 fps is a
stretch target; the accepted target remains 20+ fps with actual storage and
full-frame 1024 x 768 preview. See [Pi Hardware](PI_HARDWARE.md) for setup,
historical receipt boundaries, and storage validation.

## Audited quality evidence

[claims_log.md](claims_log.md) preserves the machine-written signoffs.
The corresponding `tests/quality_gates/runs/<hash>/run.json` is the receipt;
`tests/quality_gates/gates.json` defines the actual class thresholds.
This cleanup neither reruns quality gates nor changes thresholds or signoffs.

| Existing still signoff | Run hash | Recorded worst LPIPS |
|---|---|---:|
| q0 plus matched still CNN | `54f001717c547574` | 0.0314 |
| q3 plus matched still CNN | `b44fa841c05c9bff` | 0.0155 |
| q8 without CNN | `9837b797660627aa` | 0.0035 |

These are bounded test-set results, not universal quality guarantees.
VIDEO_FREEZE and historical UPRESABLE signoffs remain in the same ledger.
The latter gates editable raw using Bayer PSNR; its recorded rendered LPIPS
is not a finished-render approval. Historical desktop full-resolution
VIDEO_FREEZE results do not establish embedded capture throughput.

## Promotion and release rules

The approved raw-video reconstruction baseline is frozen. Reopen it when a
locked gate, receipt, artifact hash, CI guard, or manual review fails, or when
a replacement already beats the baseline with equivalent quality, timing,
memory, packaging, review, and hash evidence. Optional PSF/blur modeling is
future research, not a requirement for the approved baseline.

Premium still SR needs its own detail, noise, metadata, and editor-latitude
evidence. A synthetic noise texture, crop-only win, or output file that opens
does not establish restored source detail or editing latitude.

A camera-ready claim additionally requires actual sensor/DMA, storage, and
display execution at the accepted floor with zero drops, a valid stream,
and recovery proof. [Capture API](CAPTURE_API.md) defines ownership;
[Release Artifacts](RELEASE_ARTIFACTS.md) defines delivery.

Archived documents at the preserved parent revision retain prior narratives:
[Video Status](https://github.com/dcliftreaves/gpr/blob/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/docs/VIDEO_STATUS.md),
[Premium Still SR](https://github.com/dcliftreaves/gpr/blob/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/docs/PREMIUM_STILL_SR.md).

Detailed older matrices and decisions remain in the
[archive](https://github.com/dcliftreaves/gpr/tree/archive/research-and-integration-2026-09-09),
pinned at [`3d675ef`](https://github.com/dcliftreaves/gpr/tree/3d675ef).
