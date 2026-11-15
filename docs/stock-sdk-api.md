# Public SDK C entry points and parameter ownership

The API audit found two historical declarations in `gpr.h` without matching
definitions: `gpr_parameters_construct_copy(source, destination)` was implemented
as a three-argument C++ overload, and `gpr_check_vc5(input, Alloc, Free)` as a
two-argument C++ overload. A C client using the shipped header compiled but
failed to link both symbols. The implementations now match the published C
signatures. The `gpr_parameters` layout and existing declarations are retained.

## Copying calibration/parameters

Construct copies into an uninitialized or empty destination, distinct from the
source. Destroy any destination-owned storage before reconstructing it.

```c
gpr_parameters copy;
gpr_allocator allocator = {malloc, free};
if (!gpr_parameters_construct_copy_with_allocator(&allocator, &source, &copy)) {
    /* Destination contains defaults; source is unchanged. */
    return false;
}
/* use copy */
gpr_parameters_destroy(&copy, allocator.Free);
```

The new allocator variant returns false for invalid arguments, inconsistent
GPMF pointer/size, GainMap pointers with zero size or allocation failure. It
deep-copies the GPMF payload and each present Bayer GainMap. Failure at any
allocation releases all earlier allocations through the supplied Free and
leaves the destination at defaults. A null destination cannot be initialized;
source/destination aliasing is rejected without modifying the source.

The historical two-argument void constructor uses malloc/free and the same
implementation; failure produces defaults but cannot be reported through its
void result. Use the allocator variant when failure must be observed. Free
with the matching allocator. Destroy clears owned pointers/sizes so repeated
cleanup is safe. The JPEG preview remains borrowed from the source, as in the
historical structure/destructor contract; keep that external storage alive
while using either parameter object. No other metadata allocation is shared.

## Identifying VC-5 containers

`gpr_check_vc5(input, Alloc, Free)` reads DNG metadata and returns whether a valid
DNG declares VC-5 compression for its main image. It returns false for other
compression, invalid/null/short input, absent allocation callbacks or parser
exceptions. Input remains borrowed and unchanged during this synchronous call.
Inputs larger than UINT32_MAX bytes are rejected before creating the SDK's
32-bit memory view. The DNG parser uses its normal internal allocator; this
metadata-only path does not allocate caller-owned image buffers.

This is format classification, not compressed payload validation. A valid
container can still return true after its image bytes have been damaged. Use
actual decoding and the recording's framing/checksums where payload validity
is required. The checker does not decode, build a RAW image or require the
encoder/decoder entry points. Read-only and write-only SDK builds can identify
VC-5 containers without the opposite image codec.

## Evidence and remaining audit scope

`sdk_c_api_test` is compiled as C and linked through `gpr_sdk`. It exercises
legacy and explicit-allocator copies, independent owned bytes, all five
allocation failure points, source preservation, borrowed JPEG lifetime,
repeated cleanup, alias/invalid arguments, valid compressed and uncompressed
DNG, short/malformed input, missing callbacks and the documented metadata-only
behavior after image payload damage. Installed C/C++ clients call the repaired
symbols too. A symbol inventory of 28 SDK/common public declarations found no
remaining missing C symbols in the full build; this does not establish runtime
correctness for every possible argument to every SDK function.

Revision-specific native, sanitizer, package and Pi evidence is in
`stock-sdk-api-receipts/`. Broader geometry/buffer boundary auditing and full
still-SDK concurrency remain separate work. The metadata classifier does not
make the vendored DNG SDK generally safe for concurrent still processing.
