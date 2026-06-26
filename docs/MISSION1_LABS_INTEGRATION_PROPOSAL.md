# Mission 1 Labs Raw Video Integration Proposal

This document describes how a raw Bayer video experiment could be integrated
into Labs firmware without requiring a general-purpose firmware plugin system.

The proposal is intentionally conservative: GoPro can adopt the command,
manifest, receipt, and resource-limit contracts first, then decide later
whether signed extension bundles are appropriate.

## Broader Fork Context

The upstream-facing proposal is a narrow Labs integration path, but the fork it
comes from is broader than raw video alone:

- GPR stills work now includes production-gated 50 MP tiers around 9.8 MB,
  15.1 MB, and 27.2 MB per frame.
- The stills path uses a matched Bayer-domain 1x CNN to preserve visual quality
  at lower file sizes, while the highest-quality still tier passes without CNN
  restoration.
- The raw format work includes 14-bit and 16-bit Bayer handling, so the same
  family of tools can cover newer high-resolution sensors instead of being
  limited to older 12-bit still paths.
- The video work adds an independently decodable `.gvid` container, camera
  display preview target, host-side editable DNG/GPR export, and offline 4K/8K
  CNN reconstruction experiments.

Those pieces do not all need to land in one upstream change. The proposed first
step is the small firmware contract that would let GoPro evaluate raw video,
preview, storage, and receipts with normal Labs safety boundaries.

## Proposed First Milestone

Compile a GPR/GVID raw-video experiment into Labs firmware and expose it through
Labs/Open GoPro commands:

```text
mode: gvid_4k_bayer_20
input: sensor Bayer frame source
output: .gvid file on camera storage
preview: 1024 x 768 RGB camera display surface
target: 20 fps or higher on Mission 1 class hardware
```

The feature should remain hidden unless explicitly enabled by Labs/Open GoPro.

## Host-To-Camera Flow

1. User installs Labs firmware.
2. User sends a Labs QR/script command or Open GoPro request selecting
   `org.gpr.gvid`.
3. Firmware validates that the extension mode is supported on the camera.
4. Firmware starts raw frame capture and writes `.gvid`.
5. Firmware renders a preview from the same `.gvid` stream or from the same raw
   frame source.
6. Firmware writes a capture receipt.
7. Host tools download `.gvid` and receipts over USB/Wi-Fi for review/export.

## Why Not A General Plugin Loader First?

Native downloadable modules create security, reliability, and support issues:

- Signature and rollback policy.
- ABI compatibility across firmware versions.
- Power-loss safety during install/update.
- Resource containment for CPU, RAM, storage, and thermal budget.
- Crash isolation from capture and media indexing.

A statically compiled Labs feature with a manifest-like contract gives most of
the review value without those risks.

## Extension Bundle Future

If GoPro later wants installable extensions, the same manifest can become the
bundle descriptor. The firmware should only load signed bundles whose manifests
declare supported camera models, ABI version, required capabilities, and
resource limits.

Recommended bundle install path:

```text
USB/Wi-Fi upload -> staging area -> signature validation -> compatibility
check -> inactive install slot -> explicit user activation
```

Recommended failure behavior:

- Never activate an unverified bundle.
- Keep the previous working bundle.
- Emit an install receipt with a human-readable rejection reason.

## Acceptance Receipts

For a raw-video Labs feature, production review should require receipts for:

| receipt | proves |
|---|---|
| `capture_receipt.json` | Sensor/DMA frame source, output dimensions, pixel format, frame count, dropped frames, write throughput, and FPS. |
| `preview_receipt.json` | Preview dimensions, preview FPS, and UI/display presentation status. |
| `recovery_receipt.json` | The `.gvid` stream can recover the last complete frame after interrupted capture. |
| `extension_receipt.json` | Manifest hash, ABI version, camera model, firmware version, and resource limits. |

The capture receipt should also record raw bit depth, Bayer phase, black/white
levels, and whether the source is 14-bit or 16-bit so host tools can export
editable DNG/GPR without guessing camera metadata.

## Minimal Upstream PR Contents

This PR intentionally proposes the contract rather than a complete camera
firmware feature:

- `docs/LABS_RAW_MEDIA_EXTENSION_API.md`
- `docs/GVID_CONTAINER.md`
- `docs/MISSION1_LABS_INTEGRATION_PROPOSAL.md`
- `source/lib/gpr_sdk/public/gpr_labs_extension_api.h`
- `examples/labs/raw_media_extension_gvid.json`
- `tools/validate_labs_extension_manifest.py`

## Open Questions For GoPro

- Should Labs extensions be controlled through QR/Labs commands, Open GoPro
  HTTP, or both?
- Should signed bundles ever include native code, or should bundles only carry
  data/configuration for code already present in Labs firmware?
- Which receipt fields are required for a Labs feature to graduate from
  experiment to supported mode?
- Should `.gvid` be a Labs-only experimental container name, or should it be
  formalized as part of the GPR project?
