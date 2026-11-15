# Native preview of stock RAW video

`gpr_preview` renders stock four-channel, three-level VC-5 RAW payloads into
caller-owned RGB24 rows. The full frame is reduced by 4, 8 or 16 on each axis;
4096×3072 input produces 1024×768 at the default reduction. It does not
materialize a full-resolution Bayer image or run a CNN. The display application
owns its buffers and scheduling. Row padding is preserved; unaligned payloads
are copied to aligned scratch. See `gpr_preview.h` for the C interface.
Both sensor dimensions must be at least 48; dimensions/rendering reject
smaller input before decoding. The framing reader retains its separate
container contract. See [encoder bounds and alignment](stock-encoder-bounds.md).

`gpr_gvid_reader` supplies bounded, callback-based reads of stock GVID v2/v3.
Short reads are supported. Metadata is opaque, frame views expire on the next
read, and read/format/checksum failures latch. V3 requires a valid END3 and EOF;
v2 reports clean EOF without claiming a verified end marker. Default payload
limit is 128 MiB, further bounded by geometry; clips are limited to one million
frames. This reader does not perform interrupted-prefix recovery.

Build and run:

```sh
cmake -S . -B build-preview -DCMAKE_BUILD_TYPE=Release \
  -DGPR_BUILD_PREVIEW_TOOLS=ON -DGPR_BUILD_PREVIEW_TESTS=ON
cmake --build build-preview --target gpr_preview_app preview_contract_test -j4
ctest --test-dir build-preview -R preview_contract --output-on-failure
build-preview/gpr_preview_app --input clip.gvid --downsample 4 --runs 5
```

Use `--output-dir new_directory` to write PPM frames. `--limit N` limits
rendering per pass, while the reader still verifies the entire recording.
Capture is a separate optional library; preview links the decoder and GVID
reader without the encoder or capture library. Complete SDK-independent build
profiles remain part of the feature rebuild.

The optional [prepared calibrated path](stock-calibrated-preview.md) now adds
source black/white/color/GainMap processing and correct crop/orientation through
an immutable native plan. It uses reduced linear RGB and preserves this legacy
API's exact output contract. See that guide for SDK-free camera usage, host
profile preparation, frame bindings, NEON processing and current Pi measurements.

## Correctness and decoder changes

Checked-in fixtures were encoded and decoded with an independent stock
`6b22975` build, using the public SDK and the standalone reference probe.
Tests compare RGB bytes at all three reductions, including odd wavelet widths,
unaligned input and padded output. Concurrent cold-start tests cover table
initialization. The native reader is tested against independently constructed
framing, every fixture truncation and single-byte corruption, I/O failure and
payload limits. Fixture provenance is in `tests/fixtures/stock_preview`.

The port imports the stock VLC prefix-table optimization, fixes its consumed
run bookkeeping, and rejects oversized/early-terminating runs. It uses actual
wavelet pitch for RGB conversion, initializes shared curves once, replaces
RGB8 per-pixel square roots with an exact lookup, and avoids reconstructing
the unused green-difference channel for reduced RGB. Full Bayer decoding
retains all four channels. The wrapper checks the supported RAW header and
section/codeblock bounds; it is not a complete entropy validator or a
sandbox for hostile VC-5 streams.

Default gains match the historical SDK (R=30/16, G=1, B=7/4). They are not
camera-calibrated color processing. No exposure, tone mapping, CNN or quality
approval is implied. A real 12 MP Q3 frame also matched independent stock
quarter-resolution output exactly (RGB SHA-256
`3bf5e715fb80f9f20ea4aaa75949f015766beb506389fbad9b4872463bc5f360`).
The legacy `WaveletToRGB` also mirrors horizontally and does not apply the
source TIFF Orientation. This API currently preserves that convention;
the separate prepared path corrects orientation and applies source calibration.
Editable DNG export preserves the source Orientation
and Bayer layout for the RAW renderer.

## Pi 5 measurement, 2026-10-28

The same Pi 5/USB SSD environment as the capture receipts played a 12-frame
4096×3072 Q3 recording five times, yielding 60 full-frame 1024×768 previews.
`stock-preview-pi5/preview-1024x768.jsonl` contains each timing and RGB checksum:

| Measurement | Result |
|---|---:|
| Decode and RGB median | 52.9769 ms |
| Decode and RGB p95 | 53.4989 ms |
| Read and checksum verification median | 2.32936 ms |
| Complete wall throughput | 18.0463 previews/s |
| Total elapsed | 3.32478 s |

All 60 frames were rendered and all five end markers verified. This is warm
filesystem playback with per-frame JSON logging, no PPM writes. Input is
still-derived replay, not sensor footage. This does not establish simultaneous
capture/display throughput, on-camera display integration or 30 fps preview.
Local Release, ASan/UBSan and TSan suites and Pi Release and ASan/UBSan suites
cover the preview alongside the capture regressions.
