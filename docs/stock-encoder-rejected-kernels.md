# Rejected encoder load and small-code-pair experiments

Neither proposal is retained in production. Both preserve checked numeric
output, but both are slower in their isolated Pi 5 comparisons. No capture
executable is rebuilt or replaced, and no sustained-floor approval follows.
The existing encoder/preview binary remains SHA-256 `9fc7b823…`.

The current direct-output [profile](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/direct-profile-flat.txt)
attributes about 20.8% of sampled user cycles to channel unpacking and 23.0%
to incremental highpass encoding. These samples motivate the targets; they
are not wall-time savings. Both experiments below are separate unprofiled,
CPU-2-pinned processes and change no persistent scheduling or governor policy.

## Paired 64-bit input loads

The RGGB14 channel-row kernel currently uses four paired 32-bit loads per
four output samples. The prototype uses two paired 64-bit loads and extracts
both green indices before reusing source registers for red or blue. It retains
independent lookup loads, packed NEON arithmetic, caller-saved registers,
lookup values and stock rounding. It accesses the same input bytes and adds
no alignment requirement. The [generated instructions](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/wide/wide-disassembly.txt)
confirm the changed load/extract sequence against the
[baseline](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/wide/baseline-disassembly.txt).

The same-binary harness compares both assembly functions against an independent
scalar formula using the **actual stock log table**. All 27,712 row/channel
comparisons pass, including both real 4096x3072 inputs, multiple row offsets,
output canaries and page-end guards. Zero-width calls also return without access.

Eight baseline/candidate/candidate/baseline/candidate/baseline/baseline/candidate
runs each process sixteen real frames' worth of unpacking. All four channels
run serially in this harness, with a reused row buffer. These are isolated
unpack times, **not elapsed encoding or camera-frame times**:

| Kernel | Median ms per frame's unpack work | Range, ms |
|---|---:|---:|
| Baseline | 22.0876 | 22.0690–22.1399 |
| Wide loads | 22.1906 | 22.1690–22.2209 |

The prototype is about **0.47% slower**, with every candidate run slower than
every control. Fewer input-load instructions do not establish a useful speedup.
No full capture comparison is justified by this result; the proposal is discarded.

[Harness](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/wide/kernel.c),
[baseline assembly](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/wide/baseline.S),
[candidate assembly](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/wide/wide.S),
[original-symbol candidate](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/wide/candidate-original-symbols.S),
[measurements](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/wide/kernel.json), and
[identities/inputs](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/wide/metadata.json) are retained.

## Precombined signed-nibble code pairs

The second prototype prepares a 256-entry, 2 KiB table for pairs of coefficients
in [-8, 7], appended to a private copy of the existing signed-code table. An
exact packed-lane range check precedes two pair lookups in place of four
individual lookups. The existing nonzero endpoint bounds literal-zero runs;
combined lengths above 32 bits and other values retain the original fallback.
Pending runs, reservoir updates and final markers keep their stock semantics.
This table extension exists only in the standalone harness.

All **2,570 byte-stream comparisons** pass against the original extracted
function, including whole-band/incremental rows, odd pitches, zeros/long runs,
small coefficients, values outside the pair range and large signed magnitudes.
A separate **262,144-case** check exhausts the signed-word range in each packed
lane. The experiment does not establish exhaustive codec correctness.

Longer matched runs generate synthetic signed-nibble inputs **before** timing,
then encode 50,000 three-row streams per run. Stream setup, incremental encoding
and final flush remain timed; codebook/table setup is excluded. Output bytes
match in every run:

| Packing path | Median seconds per 50,000 streams | Range, s |
|---|---:|---:|
| Baseline | 0.263368 | 0.263300–0.263435 |
| Pair table | 0.281721 | 0.279910–0.281735 |

The candidate is about **6.97% slower** on this intended small-code workload.
It is discarded. This is synthetic packing behavior, not a measured whole-frame
regression or a claim about other coefficient distributions. Neither proposal
changes the production pipeline or the unresolved Pi deadline gate.

[Harness](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/pair/test.c),
[extracted baseline/candidate functions](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/pair/functions.inc),
[candidate block](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/pair/candidate-block.c),
[measurements](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/pair/receipt-long.json), and
[identities](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/pair/metadata.json) retain the experiment.

## Reproduction and limits

[Compiler commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/compile.json) identify the
measured source/library paths. Copy the harness directory, adjust repository
paths and build against the matching stock libraries before running the command
in its metadata receipt. Binary hashes identify measured artifacts; binaries
and source RAW media remain external. The
[summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/summary.json) records the median calculations.

Baseline extracted assembly/function text is checked against `8e1c2b6` source.
The entropy candidate differs only by the archived fast block; the harness
constructs its additional table separately. Production codec/model sources and
the existing Pi capture executable remain unchanged. No new production tests
or package rebuild are needed for these rejected, standalone experiments.
The recorded temperature/frequency/governor readings are **post-experiment**,
not a thermal-control claim. Sensor/display/SD/thermal and sustained capture
performance are outside these isolated measurements.

All seven hosted checks for source/model revision `8e1c2b6` pass, including
Mac's optional CNN/full-image selector tests and native/package contracts.
The [hosted receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-encoder-kernel-receipts/hosted-8e1c2b6.json) is separate
from these isolated performance measurements. This evidence-only addition
does not change the validated production source.
