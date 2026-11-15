# SDK parameter import: explicit JSON string boundary

The hosted 19b5dda LinuxSanitizers job fails the new large-gain-map import
regression for all three grid sizes. Its trace identifies a heap read past
the input file buffer inside `cJSON_ParseWithOpts`, before gain-map parsing.
The binary file reader returns exactly the file bytes, while cJSON expects
a null-terminated C string.

The JSON consumer now constructs `std::string(buffer, size)` and passes its
`c_str()` to cJSON. This supplies the required terminator without changing
the binary reader or its RAW/payload byte counts. The previous capacity-matched
gain-map storage and JSON cleanup remain in place.

Fresh `sdk_c_api`, `raw_media` and `stock_still` CTests pass on Mac and Pi.
The existing regression covers 9x9, 65x65 and 97x97 maps with exact pixel and
opcode roundtrips; no assertions or hosted checks are disabled. Only the CLI
is rebuilt on Pi, and the capture executable remains byte-identical.

[Failed job excerpt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sdk-json-boundary-receipts/failed-ci-excerpt.log),
[source/binary validation](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sdk-json-boundary-receipts/receipt.json) and
[manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sdk-json-boundary-receipts/manifest.json) retain the specific
failure and correction. All seven hosted jobs subsequently pass at `170802d`
([run 41175893067](https://github.com/dcliftreaves/gpr/actions/runs/41175893067)).
This is a bounded fix to the observed import path; no broader
sanitizer/fuzz campaign is part of this work.
