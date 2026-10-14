# Manual Bitstream Conformance

This directory retains manual generation/comparison infrastructure, not a
passing current CI gate. No current goldens are committed. A check requires
a deliberately reviewed baseline; generating expected bytes from the same
encoder being checked does not establish compatibility.

The historical 24 goldens predated the prescale/filter change at `948f715`.
The earlier audit recorded 12 mismatches per level against those old bytes;
that is historical evidence, not the result of today's baseline-free checkout.
The original inputs and goldens remain in the
[pinned archive](https://github.com/dcliftreaves/gpr/tree/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/tests/conformance).

## Build and Compare

Build the top-level CMake libraries first, then run from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
export CONFORMANCE_BIN_DIR="${TMPDIR:-/tmp}/gpr-conformance"
BUILD_DIR=build bash tests/conformance/build.sh
```

The script builds `conformance_check_L1/L2` and
`conformance_generate_L1/L2`. Each pins its runtime wavelet mode and
`FUSED_THREADS=1`. The matrix is four synthetic RGGB14 inputs, qualities
0/3/5, and levels 1/2. MD5 checks byte identity, not perceptual quality.

For an explicitly historical comparison, extract the preserved baseline to
scratch space without repopulating the current checkout:

```bash
BASELINE=$(mktemp -d "${TMPDIR:-/tmp}/gpr-conformance-history.XXXXXX")
git archive f13dbe95d3e97c400e8d493ba178dbb037ff3c96 tests/conformance/inputs tests/conformance/golden | tar -x -C "$BASELINE"
"$CONFORMANCE_BIN_DIR/conformance_check_L1" "$BASELINE/tests/conformance"
"$CONFORMANCE_BIN_DIR/conformance_check_L2" "$BASELINE/tests/conformance"
```

The pinned revision must be available locally. Inspect each exit status and
report mismatches as differences from that historical baseline, not a current
release failure. Missing inputs or goldens are errors, never passes.

For a proposed new baseline, create scratch `inputs/` and `golden/`
directories and pass their parent to each generator. Review the producing
revision, settings, byte changes, decoder compatibility, and independent
quality evidence before adopting it. Do not generate or commit goldens merely
to clear a cleanup review.

Use [Testing Methodology](../../docs/TESTING_METHODOLOGY.md) for current
validation and [GVID Conformance](../../docs/GVID_CONFORMANCE.md) for container
accept/reject and recovery checks.
