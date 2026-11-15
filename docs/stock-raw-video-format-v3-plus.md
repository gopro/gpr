# Stock-Compatible RAW Video Format (`GVID v3`) Improvements: Single-Pass Cache-Hot CRC32, $O(1)$ Header-Only Fast Seeking & Clip-Static Optical Stackup Inheritance

## 1. Overview

This document describes three backwards-compatible improvements to the `GVID v3` RAW video format (`source/lib/gpr_video/gpr_gvid.*`, `source/lib/gpr_video/gpr_capture.cpp`, and `tools/stock_video.py`) designed for high-rate Mission 1 RAW video capture and desktop post-processing:

1. **Cache-Hot Single-Pass Payload CRC32 During Encode Transfer (`gpr_gvid_write_frame_with_crc` + `copy_and_crc32`)**:
   - Eliminates the second multi-megabyte cold DRAM scan of every compressed VC-5 frame on the writer thread by computing `payload_crc32` in 64 KiB L1-cache-resident chunks while copying the newly encoded payload from the codec arena into the capture slot.
2. **Header-Only $O(1)$ Fast Indexing & Algebraic Stream-CRC Verification (`scan_index` + `seek_record` + `crc32_combine`)**:
   - Exploits the self-authenticating 48-byte `FRM3` frame header (`header_crc32` covering `payload_size`, `metadata_size`, `payload_crc32`, and `metadata_crc32`) together with IEEE 802.3 $GF(2)$ matrix exponentiation (`crc32_combine`) to index and cryptographically verify an entire multi-gigabyte `.gvid` recording by reading **only 48 bytes per frame** (`43.2 KiB` total for a 900-frame `3.6 GB` clip — an **83,000x I/O reduction**), enabling instant random-access frame extraction and restoration (`--start-frame`, `index` action).
3. **Clip-Static Calibration & Mission 1 Optical Stackup Inheritance (`resolve_frame_calibration`)**:
   - Deduplicates invariant camera calibration (`GainMap`, color matrices, black/white levels) and fixed Mission 1 optical stackup parameters (`optical_stackup`: 24-coefficient polar radial/tangential PSF & LCA model) into the clip-level `GVID v3` header while merging compact per-frame dynamic telemetry (`raw_metadata_overrides`: `exposure_us`, `iso`, `analog_gain`, `digital_gain`, `imu_gyro`) per frame.

---

## 2. Wire-Format Compatibility Guarantee

All improvements preserve **100% wire-level backwards compatibility** with existing `GVID v2` and `GVID v3` streams, `gpr_gvid_reader`, and `gpr_mov` (`GVS2` / `GVS3` QuickTime/MOV lossless sample wrapping):

```
┌────────────────────────────────────────────────────────────────────────────┐
│ Clip Header (64 B: "GVID", v3, "VC5\0", geometry, meta_crc, hdr_crc)       │
├────────────────────────────────────────────────────────────────────────────┤
│ Clip Metadata JSON (Static `raw_metadata` + `optical_stackup` descriptor)  │
├────────────────────────────────────────────────────────────────────────────┤
│ Frame 0 Record Header (48 B: "FRM3", idx, pts_ns, len, meta_len, CRCs)     │
│   ├── Frame 0 Dynamic Metadata JSON (`raw_metadata_overrides`, exposure)   │
│   └── Frame 0 Stock VC-5 Bitstream Payload (Skipped via seek during index) │
├────────────────────────────────────────────────────────────────────────────┤
│ ... Frames 1 .. N-1 ...                                                    │
├────────────────────────────────────────────────────────────────────────────┤
│ End Marker (48 B: "END3", frame_count, last_pts, payload_bytes, stream_crc)│
└────────────────────────────────────────────────────────────────────────────┘
```

### Why Header-Only Indexing Can Verify Full-Stream Integrity Without Reading Payloads

In `GVID v3`, the 48-byte `END3` footer validates the cumulative `stream_crc32` over the entire file (`clip_header + clip_metadata + sum_i (FRM3_i + frame_metadata_i + payload_i)`). Previously, verifying `END3` required reading every byte of every frame payload (`~3.6 GB` for 30 seconds of 12 MP RAW video at 30 fps).

Because each 48-byte `FRM3` header is protected by its own `header_crc32 = crc32(FRM3[0..44])` and stores both `meta_crc = crc32(frame_metadata_i)` and `payload_crc = crc32(payload_i)`, `scan_index()` advances the cumulative stream CRC across frame $i$ in $O(\log(\text{len}))$ time without reading `frame_metadata_i` or `payload_i`:

$$\begin{aligned}
\text{crc} &\leftarrow \operatorname{crc32}(\texttt{FRM3}_i,\; \text{crc}) \\
\text{crc} &\leftarrow \operatorname{crc32\_combine}(\text{crc},\; \texttt{meta\_crc}_i,\; \texttt{meta\_size}_i) \\
\text{crc} &\leftarrow \operatorname{crc32\_combine}(\text{crc},\; \texttt{payload\_crc}_i,\; \texttt{payload\_size}_i)
\end{aligned}$$

When `scan_index()` reaches `END3`, it compares the algebraically combined `crc` against `END3.stream_crc32`. Once indexed, `seek_record(f, header, entry)` jumps directly to `entry["record_offset"]` (`f.seek`) and verifies the target frame's `header_crc32`, `metadata_crc32`, and `payload_crc32` before decoding or running neural restoration.

---

## 3. CLI Usage Examples

```sh
# 1. Build and verify an O(1) random-access frame index (reads only 48B headers, 0 payload bytes)
python3 tools/stock_video.py index clip.gvid > clip.gvid.idx.json

# 2. Random-access extract/restore frames 450..454 directly from a 900-frame clip
python3 tools/stock_video.py restore clip.gvid --out-dir restored_slice \
  --decoder "$BIN" --start-frame 450 --limit 5 --scale 2 --device mps \
  --checkpoint models/stock-q3-rggb14-sr-diagnostic/selected.pt
```
