# Security Reporting

This page applies to the [dcliftreaves/gpr fork](https://github.com/dcliftreaves/gpr).
Do not publish exploit details or sensitive sample files in a public issue.

## Reporting privately

Use the fork's [Security page](https://github.com/dcliftreaves/gpr/security)
and its private vulnerability-reporting option if enabled. If no private
reporting channel is listed, request a private contact method from the fork
maintainer without including vulnerability details. The upstream author list
is attribution, not a fork security contact directory.

Include the affected commit or tag, platform/build configuration, impact,
reproduction steps, and a minimal shareable sample or failing test.
State any disclosure constraints and your credit preference.

Reports affecting the original GoPro repository should also follow that
repository's own security reporting process.

## Scope and validation

Relevant surfaces include GPR/VC5/FUSED decoding, DNG/XMP metadata,
`.gvid` and MOV container parsing, command-line file handling, and
public encoder APIs. Memory-safety failures, integer overflows, malformed-input
crashes, and resource exhaustion warrant investigation with their input and
API trust boundaries documented. Do not assume encoder inputs are safe
merely because they represent sensor pixels.

Use [GVID Conformance](docs/GVID_CONFORMANCE.md) and
[Testing Methodology](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/TESTING_METHODOLOGY.md) for focused regression
coverage. Coordinate fixes and disclosure with the maintainer; this document
does not promise response deadlines, a disclosure window, or version-specific
backports. Report the exact revision affected rather than inferring support
from a version label.
