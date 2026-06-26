# GVID Container Sketch

`.gvid` is proposed as a simple raw-video container for Labs raw media
experiments. The intent is to store independently decodable raw Bayer frames,
not to introduce cross-frame compression or a new image development pipeline.

This document is a container sketch for review. It is not a final bitstream
standard.

## Design Goals

- One raw Bayer frame per container frame.
- No cross-frame dependency.
- Constant-time recovery after interrupted capture.
- Enough metadata to decode, preview, and export the stream on a host.
- Simple append-only writer suitable for embedded storage paths.
- Compatibility with existing GPR/VC-5 frame payloads where possible.
- Explicit 14-bit and 16-bit Bayer metadata so host tools do not infer raw
  precision from payload size alone.

## File Layout

```text
gvid_file:
  file_header
  frame_index_hint?      # optional, may be absent during capture
  frame_record[0]
  frame_record[1]
  ...
  frame_record[n-1]
  final_index?           # optional, written on clean close
```

## File Header

| field | description |
|---|---|
| `magic` | ASCII `GVID`. |
| `version` | Container version. |
| `header_size` | Bytes in the file header. |
| `width` / `height` | Stored Bayer frame dimensions. |
| `fps_num` / `fps_den` | Intended frame rate. |
| `pixel_format` | Bayer pixel format identifier. |
| `bit_depth` | Sensor precision carried by the frame payload, commonly 14 or 16. |
| `payload_kind` | Payload type, for example `gpr_vc5_frame`. |
| `metadata_size` | Bytes of JSON or CBOR stream metadata following the fixed header. |

## Frame Record

| field | description |
|---|---|
| `sync` | Frame sync word. |
| `frame_number` | Monotonic frame index. |
| `timestamp_us` | Sensor or firmware timestamp. |
| `payload_size` | Encoded payload bytes. |
| `payload_crc32` | Payload integrity check. |
| `metadata_size` | Optional per-frame metadata bytes. |
| `payload` | One independently decodable raw frame. |

If power is lost, a recovery scanner can walk records until the first invalid
sync, size, or CRC. The last complete frame remains usable.

## Metadata

Required stream metadata:

- Camera model and firmware version.
- Sensor crop/window.
- Bayer phase and bit depth.
- Black level and white level.
- Pixel format and stored bit depth, including 14-bit and 16-bit Bayer modes.
- Capture mode ID.
- Extension ID and extension manifest hash.
- Lens and color metadata needed for host-side DNG/GPR export.

Optional per-frame metadata:

- Exposure time.
- Analog and digital gain.
- White balance gains.
- Temperature.
- IMU timestamp correlation.
- Dropped-frame marker.

## Preview

The preview path should decode the same `.gvid` stream, then produce a bounded
RGB preview surface. For Mission-style camera displays, the proposed review
target is 1024 x 768 from a 4096 x 3072 Bayer stream.

Preview output is not the primary raw deliverable. It is a camera UI path and
should be validated separately from host-side ProRes or editable DNG/GPR export.

## Receipts

The writer should emit a receipt with:

- File path.
- File size.
- Frame count.
- Dropped frame count.
- Output hash when feasible.
- Sustained write throughput.
- Median and wall-clock FPS.
- Recovery result after truncating or scanning the tail.

## Open Questions

- Should metadata be JSON for debuggability or CBOR for smaller writes?
- Should the final index be optional, required, or stored in periodic chunks?
- Should frame payloads be byte-aligned for DMA/writev efficiency?
- Should preview metadata be in the same file or a sidecar receipt?
