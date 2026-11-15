# Stock Bayer video format adapters

The v3 stock GVID capture path now records unpacked native-endian 16-bit words
for RGGB, GBRG, GRBG and BGGR at 12, 14 or 16 sample bits. Each of the 12
header combinations selects its matching stock encoder mode. The replay app
accepts `--cfa` and `--sample-bits`, checks each source sample against that
domain and defaults to RGGB14. Its byte stream remains GVID v3 with typed
calibration carried separately. The experimental GVID v2 writer and v2 reader
remain strictly RGGB14.

The native decoder frontend accepts the same flags when decoding VC-5 payloads.
`stock_video.py extract --decoder` passes the recording's declared format,
then can package source-calibrated DNG and original-payload GPR for every
combination. `stock_preview.py` and both native preview paths accept these
formats; the calibrated plan maps red, two green and blue sites according to
the source CFA. The optional CNN restoration adapter still requires RGGB14.
The stock transform reduces input to a 12-bit log domain; 14-bit sources lose
two low bits and 16-bit sources lose four before that lookup. This is a format
and metadata port, not a higher-precision VC-5 transform.

`tests/video_format_matrix_test.py` creates a fresh 48×48 source for each
combination and checks capture, v3/end validation, native RAW decode, editable
DNG/GPR metadata and calibrated profile playback. It verifies that declared
format and exported metadata agree; it does not claim lossless pixel identity.
Existing preview, capture and v2 compatibility tests protect older paths.

On the 2026-10-29 worktree, the full Mac Release suite passed 20/20 tests,
the full Pi 5 Release suite passed 18/18, and the revised matrix with samples
above the 14-bit ceiling passed separately on both machines. All four
relocated package profiles passed their independent build/client checks.
The Pi 5 ASan/UBSan/leak-detection matrix passed in 219.60 seconds with the
longer test timeout, and a separate instrumented BGGR16 run passed using the
revised high-range fixture. The original 180-second Pi sanitizer timeout was
insufficient for twelve SDK/compiler subprocess sequences; it produced no
assertion or sanitizer diagnostic before timing out.

The added profiles require a reader that understands the expanded v3 labels;
older strict v3 readers may reject them. Physical sensor/display integration,
12 MP speed for the non-RGGB14 modes and CNN quality for those modes remain
separate validation work.
