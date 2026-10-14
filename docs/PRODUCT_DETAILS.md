# Product Details

## Supported workflows

| Product area | Current scope | Boundary |
|---|---|---|
| Raw stills | Legacy VC5 DNG/GPR conversion; q0 smallest, q3 primary, q8 archival tiers | CNN-assisted tiers require separately supplied matched models |
| Raw video | FUSED frame encoding and `.gvid` packaging; native Mission 1 4096 x 3072 Bayer target | Pi 5 stand-in clears the accepted 20+ fps floor for encoding with buffered writes, not durable SD-card recording; camera production remains unproven |
| Camera-back preview | Full-frame 1024 x 768 RGB decoded from the same 4K stream | No camera-side CNN; actual rear-display integration remains open |
| Raw-video reconstruction | Offline-production 8K alpha0p5 SR; visually approved, review-only 4K cleanup; editable DNG/GPR packaging and ProRes review | Visual signoff does not auto-promote registry scope; review frame rate is not processing throughput |
| Premium still SR | Separate work on detail, noise, and editing latitude | Research, not a promoted replacement for approved video reconstruction |

The live FUSED/`.gvid` interface supports unpacked RGGB and GBRG at
12/14/16 bits, pixel-format tags `0..5`. The standard stills path separately
supports normal unpacked RGGB/GBRG/GRBG/BGGR. Do not infer live-video four-phase
support from stills support. Raw dimensions describe Bayer samples; rendered
UHD, 4K, or 8K output dimensions depend on the selected export mode.

The exact 8K model is `mission1_native12_8k_sr_q4t2_coord_detail_alpha0p5_v1`
with `production_scope=offline_production`. The 4K model is
`mission1_native12_4k_cleanup_rgb_cfa_w40_v1` with
`production_scope=offline_review_only` and visual signoff. Both pair with
`mission1_native12_t233` and `sips_via_gpr_tools` in the
[registry](../pipelines/registry.json). Neither status establishes live-camera
reconstruction or automatic promotion of another candidate.

## Noise and editing

Camera-noise sidecars must describe their source camera, ISO/exposure,
calibration provenance, units, and validation status. A sidecar schema pass
does not prove physical sensor-noise calibration. Mission 1/iPhone
camera-specific noise closure remains separate from ordinary codec support.

Preserve source metadata and Bayer phase through reconstruction and packaging.
A high Bayer PSNR or an editable DNG does not prove unchanged texture,
noise, color, or exposure latitude. Use the matching raw and rendered gates,
plus editor review, before promoting a model or noise policy.

## Evidence and delivery

[Ship Decision](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/SHIP_DECISION.md) defines the approved boundary;
[Pi Hardware](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/PI_HARDWARE.md) records the stand-in and storage boundaries.
[Reconstruction](RECONSTRUCTION.md) explains model availability.
[Release Artifacts](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/RELEASE_ARTIFACTS.md) defines what an external reviewer
needs to reproduce and inspect a release.

The [archive](https://github.com/dcliftreaves/gpr/tree/f13dbe95d3e97c400e8d493ba178dbb037ff3c96)
at [`f13dbe9`](https://github.com/dcliftreaves/gpr/tree/f13dbe95d3e97c400e8d493ba178dbb037ff3c96)
contains prior scorecards, calibration reports, and experiment history.
