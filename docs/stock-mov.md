# Stock GVID / MOV interchange

`stock_mov.py` wraps complete stock GVID recordings in MOV and recovers the
**original GVID file byte for byte**, including compressed payloads, calibration,
opaque application metadata, absolute nanosecond timestamps and finalization.
It uses an optional native FFmpeg transport tool and invokes no image encoder,
decoder or CNN. The primary capture format remains GVID.

## Build and use

```sh
cmake -S . -B build-mov -DCMAKE_BUILD_TYPE=Release \
  -DGPR_BUILD_SDK=OFF -DGPR_BUILD_STILL_APPS=OFF -DGPR_BUILD_LEGACY_CLI=OFF \
  -DGPR_BUILD_ENCODER=OFF -DGPR_BUILD_DECODER=OFF \
  -DGPR_BUILD_MOV_TOOLS=ON -DGPR_BUILD_MOV_TESTS=ON
cmake --build build-mov --parallel 4
ctest --test-dir build-mov --output-on-failure

python tools/stock_mov.py wrap recording.gvid --bridge build-mov/gpr_mov \
  --output recording.mov
python tools/stock_mov.py inspect recording.mov --bridge build-mov/gpr_mov
python tools/stock_mov.py unwrap recording.mov --bridge build-mov/gpr_mov \
  --output recovered.gvid

# Optional caller-synchronized PCM and nominal-rate timecode, without video re-encoding.
python tools/stock_mov.py wrap recording.gvid --bridge build-mov/gpr_mov \
  --audio synchronized.wav --timecode '01:00:00:00' \
  --ffmpeg /path/to/ffmpeg --ffprobe /path/to/ffprobe --output with-audio.mov
python tools/stock_mov.py unwrap with-audio.mov --bridge build-mov/gpr_mov \
  --output recovered.gvid --audio-output recovered.wav \
  --ffmpeg /path/to/ffmpeg --ffprobe /path/to/ffprobe
```

The opt-in POSIX target needs CMake >=3.6, pkg-config, FFmpeg development
libraries (`libavformat`, `libavcodec`, `libavutil`) and zlib. Tests also need
Python 3, `ffprobe` and `ffmpeg`; the wrapper itself uses the Python standard library.
Video-only wrapping/recovery still needs only the native bridge. Optional
audio/timecode attachment or WAV extraction additionally uses the explicitly
selected FFmpeg/FFprobe executables.
Default capture/preview/SDK builds have no new FFmpeg dependency. MOV transport
can build without either image codec or the DNG/XMP SDK.

`gpr_mov --version` reports the linked libavformat version and its build's
license. These are external shared-library dependencies, not bundled libraries
or part of the camera encoder. MOV-enabled archives use a `-mov` profile suffix
(or standalone `interchange`) and declare the optional tool in build info.
Relocation still requires compatible FFmpeg libraries on the destination host.

## Explicit private profile

The earlier PR's `GPRr`/`GPR1` MOV profiles carry fused payloads. This port uses
distinct **`GVS2` / `GVS3` private video sample tags**:

- One video track; each sync sample contains one exact original `FRM2` or
  `FRM3` record, including its header, metadata bytes and stock payload.
- File-level `mdta` keys declare `gpr.stock.profile=framed_gvid.v1`, the exact
  original header/clip metadata and end marker as hex, original SHA-256, and
  the MOV video-track clock unit in nanoseconds.
- Prefix + ordered samples + ending reconstruct the original recording.
  No metadata sidecar is needed. The adjacent JSON receipt is optional evidence,
  not an input to recovery.
- This is a MOV compatibility wrapper for these tools. It is **not standard
  CFHD**, fused GPRr/GPR1, ProRes or a promise of playback in generic editors.
  Unwrap for the existing RAW extraction/reconstruction tools; calibrated
  rendered ProRes uses the separate [calibrated renderer](stock-prores.md).

The native transport validates GVID framing/CRC/end-marker/whole-file identity,
not opaque compressed-image syntax. The Python wrapper additionally validates
unambiguous finite JSON objects and typed source calibration where supplied.
The format retains the existing GVID v2/v3 CFA/domain/geometry boundary; adding
still formats does not silently extend video formats. V2 has physical EOF but
no verified end marker; V3 requires its checked END3 record.

FFmpeg writes and reads the MOV sample tables; the tool does not implement a
second MOV muxer. The design retains the earlier PR's libavformat approach,
using [MOV metadata/timescale options](https://ffmpeg.org/ffmpeg-formats.html)
and a version-specific custom-AVIO callback signature. Primary format handling
is tested against libavformat 59 on Pi and 62 on macOS.

## Timing, limits and failures

MOV packet PTS/DTS are derived from actual source timestamps relative to the
first frame, with no nominal-CFR rewrite. Track clocks start at nanosecond
precision. If a long frame gap would overflow MOV's 32-bit sample-duration
field, the tool chooses the smallest power-of-ten clock unit that fits.
Original absolute timestamps still remain exact inside each record. Receipts
report the selected track tick and whether presentation timestamps were
quantized. A timing pattern that cannot represent both very large gaps and
very fine spacing is rejected, rather than shifting source records to fit.
The final sample uses the rounded nominal frame period, with a one-tick minimum.

The native operational limits are one million frames, the reader's 128 MiB
payload limit, 64 KiB clip/frame metadata, and a 256 MiB per-allocation FFmpeg
limit. One video track plus at most one declared PCM audio and one `tmcd`
track is accepted. Arbitrary edited/remuxed layouts, additional tracks and
interrupted-MOV recovery remain outside this profile.

## Optional PCM and timecode

`gpr.media.aux_profile=pcm_tmcd.v1` identifies the optional tracks separately
from the stock video profile. WAV input supports little-endian RIFF PCM16,
PCM24 and float32, mono/stereo, with explicit format/sample-count/hash metadata.
Audio starts at video time zero. The caller supplies synchronization; samples
are copied without resampling, trimming, padding or drift correction. Full
audio can therefore extend beyond the video. `--audio-output` recovers the
exact PCM samples into a new WAV container, not the original RIFF/BWF metadata
or original WAV file bytes. Plain GVID recovery preserves the original GVID
and reports the auxiliary track count/timecode; GVID itself gains no audio track.

Timecode uses the declared nominal recording rate. Supported rates are
20/24/25/30/50/60 and 24000/1001, 30000/1001, 60000/1001. Twenty-fps non-drop
labels support the focused handoff replay; broader editor compatibility for
that nonstandard rate remains unproven. Semicolon drop-frame labels require
30000/1001 or 60000/1001; nonexistent skipped labels fail instead of being
silently normalized. The label does not replace original frame timestamps.

The native bridge creates a small timecode carrier by copying one encoded
video packet, with an independent frame-compatible clock. The final remux
copies only its timecode track and the original full video/audio. This avoids
FFmpeg's whole-clip timecode-sample overflow at the nanosecond video clock.
The timecode duration rounds within half its own tick; receipts report that
error, while all video packets and nanosecond timestamps remain exact.
The generated `tmcd` drop flag is explicitly retained when FFmpeg copies the
track, then its sample description, label, frame counter and duration are
independently read back. This is a bounded sample-description update, not a
replacement movie muxer. FFmpeg still writes all packet/sample tables.

Native recovery checks audio codec/rate/channels, continuous sample timestamps,
total sample count and PCM SHA-256, plus the declared timecode counter/rate/clock.
Unknown, duplicate, missing-declared or damaged auxiliary tracks fail before
publishing recovered GVID. Optional wrapping also compares every copied video
packet's hash, size, PTS/DTS and duration, verifies audio samples and completes
the ordinary full-GVID inverse check. Output GVID, optional WAV and receipt
retain exclusive local publication/rollback behavior.

The ordinary Release suites now pass 13 MOV and 11 rendering cases, including
all six PCM-kind/channel combinations, real WAV sample recovery, invalid drop
labels, damaged audio/timecode, protected outputs, long independent-clock
20-fps/drop-frame tracks and automatic ProRes inheritance. The 120-frame core
recording also round-trips with 288,241 synthetic PCM24 stereo samples at 48 kHz
and a 20-fps label. Its ProRes export retains every earlier video packet,
timestamp, decoded pixel and RAW/DNG/TIFF/calibration hash. This tests offline
desktop interchange, not camera audio delivery or actual A/V synchronization.
The new auxiliary profile has local FFmpeg 8.1.1 evidence; the earlier Pi/
FFmpeg-5 cross-version receipts below describe the preceding video-only profile.
[Full 120-frame A/V verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-av-receipts/real-core.json) and
[24 ordinary Release cases](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-av-receipts/release-tests.txt) bind the
implementation, source/PCM/video hashes and tested binaries. Camera audio
capture and physical synchronization remain separate hardware work.
All four MOV-enabled relocated profiles also pass installed auxiliary wrap/
recovery, C/C++ clients and the applicable SDK workflow: [package receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-av-receipts/packages.json),
[installed gates](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-av-receipts/validation.json).

Strict outer-atom extent checks reject truncation even when libavformat could
salvage an incomplete final metadata atom. Import checks the declared profile,
dimensions, track clock, packet PTS/DTS, durations and sync flags against the
original records, verifies source SHA-256, then validates the recovered GVID
again. Additional data references/network I/O are disabled. This is integrity
checking, not authenticated provenance.

Inputs must be regular local files; devices and FIFOs fail before reads. Native
output files are created exclusively and removed on failure. The Python
wrapper stages privately, verifies a complete inverse round trip before wrapping
publication, validates recovered JSON/calibration before unwrapping publication,
and publishes output/receipt exclusively with owned-partial rollback. Existing
files and dangling symlinks are refused. Publication has the still workflow's
filesystem-copy fallback; it does not add fsync or group-atomic durability.

## Validation and Pi 5 measurements

Eight stdlib contract/integration tests cover actual stock payload fixtures,
both GVID versions, changing typed calibration, Unicode and maximum 64 KiB
metadata, original timestamps near UINT64_MAX, adaptive clocks, impossible
clock patterns, missing END3, trailing/corrupt data, malformed JSON, damaged
MOV samples/digests/tags/timing/size tables, truncated containers, protected
outputs and receipt-free recovery. Independent `ffprobe` checks every sample's
hash, PTS and sync flag, plus dimensions and private codec tag.

All eight cases pass locally in Release and ASan/UBSan. With MOV enabled, all
eleven local native/media CTest targets pass. Pi Release passes the transport
suite using system FFmpeg 5/libavformat 59. The eight-case Pi ASan/UBSan/leak
suite also passes in 128.71 s at `5824f81`; subsequent Release/report changes
are source-verified separately at `e2065cb`. CI adds standalone Linux Release/sanitizer transport builds with SDK and
both image-codec source directories physically absent.

A new 12-frame Pi replay alternates the supplied GP011728/GPAB1094-derived
4096x3072 RAWs with their per-frame calibration. Three warm-cache attempts:

| Offline workflow | Median wall time |
|---|---:|
| Verified GVID to MOV wrapper | **1.074 s** |
| Verified MOV to original GVID | **0.602 s** |

Wrapping includes source JSON/calibration checks, hashes, native muxing and a
complete inverse readback/hash check. Unwrapping includes native demuxing,
GVID checksums, JSON/calibration validation and hashes. Timings end before final
publication/fsync. Capture and subsequent independent SDK/RAW verification are
outside these times. No CNN or image conversion runs in the timed transport.
These are still-derived replays on USB SSD, not live sensor/card or real-time
capture measurements.

All source GVID bytes are exact after every attempt. Independent `ffprobe`
verifies all twelve packet hashes and timestamps. Every recovered frame exports
editable DNG/GPR with its exact typed calibration; SDK decoding of all twelve
GPRs matches the two independent historical RAW references. Source/binary/library
identity, per-frame hashes and revision-bound test/measurement receipts accompany
the PR. Cross-version exchange is checked in both directions between Pi/FFmpeg
5 and Mac/FFmpeg 8, without copying a metadata sidecar.

Reproduce the measured wrapper path on an existing complete recording:

```sh
python tools/benchmark_stock_mov.py --input recording.gvid \
  --bridge build-mov/gpr_mov --ffprobe /path/to/ffprobe --repeats 3 \
  --output-dir /new/mov-benchmark
```

The benchmark independently verifies all packet hashes/timestamps and stream
properties after timing. Measurement tools/runtime files match Git `e2065cb`;
415 source/test/tool/build/fixture files on Pi were checked against that revision.
The initial sanitizer/SDK-pixel validation snapshot matches `5824f81` (414 files).
The independent capture/SDK/decoder binaries retain their verified `31274ea`
identity. All four MOV-enabled relocated package profiles pass C/C++ clients,
installed wrapping/recovery, guide presence and external-dependency declarations.

Receipts: [current Pi benchmark](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-mov-receipts/pi5-current.json),
[initial tests and SDK pixel/calibration checks](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-mov-receipts/pi5-initial-validation.json),
[cross-version exchange](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-mov-receipts/cross-version.json),
[relocated packages](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-mov-receipts/packages-mac.json),
[final Pi Release input guards](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-mov-receipts/pi5-final-guards.json).
The source recordings remain outside the repository.

Broader rendering validation, actual camera audio/display integration and
integrated capture/display performance remain part of the active rebuild.

## Local large-file boundary check

The current installed macOS bridge also passes a **1300-frame / 4,422,645,224-byte**
GVID round trip through a 4,422,672,510-byte MOV. All recovered GVID bytes and
all 1300 independently referenced stock payloads match. Atom inspection confirms
an extended-size `mdat` and **37 `co64` chunk offsets above UINT32_MAX**, with
the largest at 4,419,481,925 bytes. This exercises the actual 64-bit file-offset
path rather than a small file with large timestamps.

[Round-trip receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/large-mov-mac.json) and
[independent payload/atom validation](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/large-mov-validation.json)
record the check. Capture was unpaced, with backpressure and actual timestamps;
its completion times are not evidence for a 20 fps deadline or thermal gate.
This particular large-file check is video-only; the separate 120-frame auxiliary
receipts establish PCM/timecode behavior at their measured size and version.
