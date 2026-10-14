# GVID Conformance

`.gvid` v1 wraps per-frame payloads with a 32-byte clip header and a
16-byte frame header. It is separate from the FUSED payload version and the
legacy DNG/GPR v2 extensions. The reference contract is
`source/lib/vc5_encoder/gpr_video_format.h`.

## Wire layout

All integers are little-endian. No clip trailer is required.

| Clip offset | Field | Contract |
|---:|---|---|
| 0 | 4-byte magic | `GVID` |
| 4 | uint8 version | 1 |
| 5 | uint8 flags | Bit 0 rate control, bit 1 denoise; other bits zero |
| 6 | uint16 pixel format | 0..5, unpacked RGGB/GBRG 12/14/16-bit |
| 8 | uint16 quality | 0..11 |
| 10 | uint16 reserved | Zero |
| 12 | uint32 width | Nonzero Bayer width |
| 16 | uint32 height | Nonzero Bayer height |
| 20 | uint32 fps_x1000 | Nonzero; 20 fps is 20000 |
| 24 | uint32 target_kbps | Zero iff rate control is disabled |
| 28 | uint32 frame_count_hint | Zero means unknown; otherwise must match |

Each frame header has `FRM\0` at offset 0, a nonzero uint32 payload
size at offset 4, and a uint64 frame tag at offset 8. Exactly that many
payload bytes follow. Tags must increase strictly. The Labs shim has the
stronger requirement of contiguous frame indices starting at zero.

Strict validation rejects unsupported versions, unknown flags, reserved
fields, invalid header values, zero-frame streams, nonmonotonic tags,
frame-count mismatches, and any truncated header or payload. Container
validation does not inspect codec internals; independently decode samples.

## Recovery and metadata

EOF after a complete payload is a valid boundary when the clip header and
frame-count hint remain consistent. EOF inside a header or payload is
corruption, not an accepted interrupted tail. Recovery tooling must identify
complete frames and handle the count hint explicitly; it must not silently
accept truncated bytes as a valid stream.

Source metadata travels in a `<clip>.gvid.meta.json` sidecar using
`gvid_source_metadata.v1`. `tools/gvid_pack.py --metadata` validates and
attaches it. Preserve source/capture dimensions, Bayer phase and bit depth,
color/WB provenance, and matching frame identities. Clip timebase and frame
tags do not carry all sensor metadata: v1 has no per-frame timestamp field.
Firmware retains timestamps, exposure, gain, and drop records in its sidecar
or target receipt. Renderers must not guess Bayer format from payload size.

This feature depends on the shared and fused codec layers. It does not
require the asynchronous capture API, preview CLI, or desktop renderer.
`bench_fused` can encode fresh Bayer inputs directly into a `.gvid` using
`GPR_BENCH_GVID` as the output path. Packing already-compressed payloads with
`tools/gvid_pack.py` is a separate operation and is not an encoding-speed test.

Structural wire changes require a version bump and migration documentation.
Reserved fields must remain zero in emitted v1 files. See the reference
header for versioning policy.

For validation procedures, see [Testing Methodology](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/TESTING_METHODOLOGY.md)
on the companion `review/product-validation` branch.
