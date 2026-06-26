# Labs Raw Media Extension API

This proposal defines a small, reviewable extension contract for experimental
raw media features in Labs firmware. It is intentionally scoped so it can start
as a statically compiled Labs feature and later evolve into a signed extension
bundle delivered over USB, Wi-Fi, or SD card.

The motivating example is a compact raw Bayer video path that stores frames in a
`.gvid` container, offers a camera-display preview path, and preserves an
editable GPR/DNG-oriented post workflow.

## Why This Fork Is Interesting

This PR is intentionally small: it proposes a firmware-facing contract rather
than asking GoPro to review the entire experimental fork at once. The fork
behind this proposal contains a broader raw-media suite that may be useful to
Labs firmware and SDK maintainers:

- **Production-gated stills improvements.** The fork keeps three GPR still tiers
  for 50 MP images: a smallest tier around 9.8 MB/frame, a primary tier around
  15.1 MB/frame, and an archival tier around 27.2 MB/frame, all gated against
  visual metrics.
- **CNN-assisted stills latitude.** A matched 1x Bayer-in/Bayer-out CNN lets the
  smaller still tier stay inside the visual gate, while the archival tier is
  tight enough to need no CNN restoration. That gives users a practical
  size/headroom choice instead of a single fixed quality point.
- **Raw Bayer video.** The `.gvid` path stores independent GPR/VC-5-style raw
  Bayer frame payloads with no cross-frame dependency, so interrupted capture
  recovery and host-side DNG/GPR export stay simple.
- **Camera display preview.** The same raw stream can drive a bounded
  screen-resolution preview path, with receipts for FPS, dimensions, drops, and
  UI presentation.
- **Higher bit-depth raw support.** The fork work includes 14-bit and 16-bit
  Bayer paths, which matters for modern larger sensors and for preserving raw
  editing latitude beyond 12-bit action-camera stills.
- **Offline 4K/8K reconstruction.** Host-side CNN cleanup and super-resolution
  experiments target editable Bayer outputs and ProRes review media without
  changing the camera-side capture contract.

The ask here is for the reviewable interface that lets those ideas be evaluated
inside Labs-style firmware constraints: explicit frame source, storage writer,
preview surface, resource limits, and machine-readable receipts.

## Goals

- Keep the camera firmware in control of capture, storage, UI, power, and
  resource limits.
- Allow experimental raw media features to declare their ABI, capabilities,
  resource budget, and supported camera models before activation.
- Support Labs and Open GoPro control surfaces without requiring new physical
  camera UI.
- Make every capture mode produce machine-readable receipts for timing, drops,
  storage, frame dimensions, and preview/display status.
- Allow a first integration with no dynamic native-code loading.

## Non-Goals

- This is not a request for a general-purpose unsigned plugin loader.
- This does not require third-party code execution on production cameras.
- This does not change existing GPR still-photo compatibility.
- This does not require changing the VC-5 bitstream format.

## Deployment Model

### Phase 0: Static Labs Feature

The safest first step is to compile the extension implementation into Labs
firmware and expose it only through Labs/Open GoPro commands. The manifest is
still useful in this phase because it documents the mode contract and resource
limits.

### Phase 1: Signed Extension Bundle

A later implementation can accept a signed bundle copied over USB, Wi-Fi, or SD
card. The bundle should contain:

- `manifest.json`
- Optional static configuration and tuning tables.
- Optional signed native module, if GoPro chooses to support native modules.
- Test vectors and expected receipt schema versions.
- A detached signature or signature block over the complete bundle.

The camera should reject any bundle with an unsupported ABI, missing signature,
unsupported camera model, excessive resource budget, or unknown required
capability.

## Manifest

The extension manifest is JSON. A sample lives at
`examples/labs/raw_media_extension_gvid.json` and is validated by
`tools/validate_labs_extension_manifest.py`.

Required top-level fields:

| field | description |
|---|---|
| `schema` | Must be `gopro.labs.raw_media_extension.v1`. |
| `id` | Reverse-DNS extension identifier. |
| `name` | User-facing name. |
| `abi` | Integer ABI version. |
| `delivery` | How the extension is supplied: `static_labs_feature`, `signed_bundle`, or `firmware_builtin`. |
| `camera_models` | Supported camera model identifiers. |
| `requires` | Firmware capabilities the extension needs. |
| `media_types` | Output media extensions. |
| `capture_modes` | Declared capture/preview modes. |
| `resource_limits` | Hard memory, timing, and write-rate budgets. |
| `control` | Labs/Open GoPro control command surface. |
| `receipts` | Receipt schemas emitted by capture and preview paths. |
| `security` | Signature and execution policy. |

## Firmware Capabilities

The current proposal uses capability names rather than private implementation
details:

| capability | meaning |
|---|---|
| `raw_bayer_frame_source` | Firmware can provide a raw Bayer frame source without routing through encoded GPR files. |
| `sd_writer` | Firmware can write the extension output through the normal camera storage path. |
| `preview_surface` | Firmware can present a low-resolution preview surface on the camera UI path. |
| `media_indexer` | Firmware can index the output file so users can browse, download, or delete it. |
| `receipt_writer` | Firmware can write JSON receipts next to the captured media or expose them over the API. |

## Control Surface

The control surface should be compatible with both Labs scripting and Open
GoPro HTTP/USB control.

Suggested HTTP endpoints:

```text
GET  /gopro/labs/extensions
GET  /gopro/labs/extensions/{id}
POST /gopro/labs/extensions/{id}/configure
POST /gopro/labs/extensions/{id}/capture/start
POST /gopro/labs/extensions/{id}/capture/stop
GET  /gopro/labs/extensions/{id}/status
GET  /gopro/labs/extensions/{id}/receipt/latest
```

Suggested Labs commands:

```text
EXT?                         # list installed extensions
EXT=org.gpr.gvid             # select extension
EXTC=mode:gvid_4k_bayer_20   # configure mode
EXTR=1                       # start extension recording
EXTR=0                       # stop extension recording
EXTS?                        # query extension status
```

Exact command names are placeholders; the important part is that the command
surface maps to the same manifest-defined mode IDs and receipt schemas.

## Receipts

Each capture should emit a compact JSON receipt. Minimum fields:

```json
{
  "schema": "gopro.labs.raw_media_capture_receipt.v1",
  "extension_id": "org.gpr.gvid",
  "mode_id": "gvid_4k_bayer_20",
  "output": "GOPR0001.GVID",
  "frames_written": 420,
  "frames_dropped": 0,
  "width": 4096,
  "height": 3072,
  "pixel_format": "rggb16",
  "bit_depth": 16,
  "fps_target": 20,
  "fps_wall": 20.5,
  "write_mbps": 109.5,
  "storage_medium": "sd",
  "source_kind": "sensor_dma_capture",
  "preview": {
    "width": 1024,
    "height": 768,
    "fps_wall": 24.2,
    "ui_presented": true
  },
  "verdict": {
    "capture_ready": true,
    "preview_ready": true,
    "no_drops": true
  }
}
```

Receipts are the main handoff point between firmware, Labs users, and host-side
validation tools.

## C ABI Sketch

The C ABI is specified in
`source/lib/gpr_sdk/public/gpr_labs_extension_api.h`. The header is a proposal
surface only; this PR does not wire it into the SDK build.

The ABI is designed around five operations:

1. Query static capabilities.
2. Open a capture session.
3. Encode one raw frame into the extension output.
4. Decode or render one preview frame.
5. Close the session and return a receipt summary.

The firmware owns memory, source buffers, storage, and UI surfaces. The
extension receives bounded descriptors and returns status codes.

## Upstream Review Questions

- Should this live in Labs only, Open GoPro only, or both?
- Should Phase 1 allow native modules, or only signed data/tuning bundles that
  activate code already compiled into Labs firmware?
- Which camera-model identifiers and capability strings should be canonical?
- Should receipts be stored as sidecar JSON files, exposed through Open GoPro,
  embedded in media metadata, or all three?
- Which resource limits should be mandatory for embedded capture acceptance?

## References

- GoPro Labs: https://gopro.com/en/us/info/gopro-labs
- Labs QR control: https://gopro.github.io/labs/control/
- Open GoPro: https://gopro.github.io/OpenGoPro/docs/
