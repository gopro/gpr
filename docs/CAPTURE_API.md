# Raw Video Capture

The firmware-facing C entry point is
[gpr_labs_encoder.h](../source/lib/vc5_encoder/gpr_labs_encoder.h). Supply unpacked
uint16 Bayer rows, their stride, pixel format, quality, and clip rate. The
encoder emits GVID headers and freshly compressed frames through a writer
callback. It does not wrap previously compressed camera GPR files.

Configure fixed dimensions, stride, bit depth, pixel format, quality,
`fps_x1000`, target bitrate, and maximum in-flight frames before submission.
Live formats are unpacked RGGB/GBRG at 12/14/16 bits (`0..5`). Packed input
and other Bayer phases require conversion. Use the header for exact
structures, signatures, and return codes.

| Object | Ownership |
|---|---|
| Input frame | Caller-owned; valid through synchronous submit |
| Encoder state | Created and destroyed through the shim |
| Callback bytes | Encoder-owned until callback returns; consume or copy immediately |
| Storage and durability | Caller-owned, including synchronization and error handling |
| Display buffers | Firmware owns allocation, handoff, presentation, and release |

## Integration Contract

- Serialize calls to submit. Frame indices begin at zero and are contiguous.
- Submit copies the input before returning; the camera may then reuse its
  buffer. Padded rows are repacked. This is not a zero-copy DMA API.
- Bounded queues apply backpressure. Do not call blocking submit from a sensor
  interrupt handler; use a camera worker with an explicit overrun policy.
- The writer runs on a background thread. Return zero only after accepting
  every byte; a failure aborts pending work. Do not destroy the encoder from
  its writer callback.
- Flush reports writer errors or incomplete delivery. It drains the encoder,
  not the filesystem: the application owns storage synchronization.
- Keep timestamps, exposure, gain, and dropped-frame records separately.
  GVID v1 does not serialize the submitted timestamp.

Reject unsupported configurations before capture. Preserve the complete-frame
prefix after writer failure for recovery analysis; see
[GVID Conformance](GVID_CONFORMANCE.md) for stream validity.

This layer requires the shared codec, fused codec, and GVID format layers.
Build it with `GPR_ENABLE_VIDEO=ON`. Disable `GPR_BUILD_SDK` and
`GPR_BUILD_STILL_APPS` for a camera-only integration. The optional evaluation
binary `labs_encoder_bench_cli` requires `GPR_BUILD_DIAGNOSTICS=ON`; it is not
part of the firmware API. See [build profiles](GETTING_STARTED.md#native-build-options).
It does not require preview, a desktop renderer, or CNN models. It is an
integration API, not a downloadable Mission 1 firmware plugin; sensor access,
threading, storage, and camera shutdown must be connected and tested by the
firmware owner.

## Camera integration boundary

The evaluation target is native 4096 x 3072 capture and full-frame
1024 x 768 preview from the same stream. The accepted Pi 5 stand-in floor
is 20+ fps with buffered encoding writes, not durable SD-card recording.
Actual sensor/DMA input, storage synchronization, display presentation,
and frame pacing must be validated on the camera. See
[Product Details](PRODUCT_DETAILS.md) for scope and
[Native Preview](NATIVE_PREVIEW.md) for the display-side interface.

For validation procedures, see [Testing Methodology](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/TESTING_METHODOLOGY.md)
on the companion `review/product-validation` branch.
