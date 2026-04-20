# Future Architecture Ideas

Ideas for future versions. Not needed for current production release.

## GPU Acceleration
DNG I/O is 56% of encode time. The wavelet transform (8%) and ANS encode (20%) could move to Metal/CUDA. The biggest win would be GPU-accelerated DNG parsing, but that requires replacing the DNG SDK.

## Streaming Encode
Current design loads the entire image into memory before encoding. For 100MP 16-bit sensors (200MB raw), this is fine. For hypothetical 400MP+ sensors, streaming encode (process rows as they arrive) would reduce memory footprint.

## Lossless Mode
Q8 with ANS approaches lossless. A true lossless mode (quant=1 for all bands, no prescale) with ANS could achieve 2-3x lossless compression. Would need the Q6-Q8 decoder overflow to be fully fixed first.

## Direct .fff/.3FR Support
Currently some medium format raw files need dcraw or rawpy extraction before encoding. Integrating libraw into gpr_tools would allow direct `.fff` and `.3FR` input without external tools.

## NEON Vectorized rANS Decode (Investigated — Not Viable)
NEON vectorization of the 4-way interleaved rANS decoder was implemented and benchmarked. It was slower than scalar because ARM NEON lacks gather instructions — the 4 random table lookups per iteration require scalar loads into NEON registers, which costs more than the vectorized multiply saves. The scalar fast-path (packed decode table + inline renorm) remains the optimal approach. Removed and reverted.

## Context-Adaptive Tables
Different joint frequency tables per wavelet level. Level 0 bands have more nonzero coefficients (detail) than level 2 (smooth). Per-level tables would improve compression for the ISO 400-800 range where ANS barely breaks even with VLC.

## Wider Companding for 16-bit
The current cubic companding maps [0,1023] → [0,255], clipping at 255. A wider curve mapping to [0,1023] would let mode 1 work on 16-bit data, combining the benefits of companding (bounded coefficient range) with ANS's adaptive coding.
