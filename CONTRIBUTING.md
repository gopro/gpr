# Contributing

This guide applies to the [dcliftreaves/gpr fork](https://github.com/dcliftreaves/gpr).
Use the [fork issue tracker](https://github.com/dcliftreaves/gpr/issues)
for bugs and feature proposals. For vulnerabilities, follow
[SECURITY.md](SECURITY.md).

## Issues and pull requests

Search existing issues first. Include the source commit, platform, build
configuration, reproduction command, and a small shareable input when possible.
Describe expected and observed behavior. Discuss substantial API or format
changes before implementation.

Create a focused branch from the fork's current base and open a pull request
against the fork. Explain the behavior change and relevant validation.
Use [Getting Started](docs/GETTING_STARTED.md) for building and
[Testing Methodology](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/TESTING_METHODOLOGY.md) for checks appropriate to
the change. Keep documentation and examples portable.

Preserve format compatibility and existing quality thresholds. A new quality
claim needs its matching gate receipt; do not hand-edit
[quality signoffs](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/claims_log.md). Hardware, stand-in, and offline
results must retain their scope labels.

## Licensing and upstream contributions

Preserve applicable copyright notices, license terms, and third-party
attributions. Submit only material you have the right to contribute.
The repository's existing license files and source notices remain authoritative;
this guide adds no license grant or CLA requirement.

Contributing directly to [GoPro's upstream repository](https://github.com/gopro/gpr)
is a separate process governed by its contribution instructions. The original
upstream guide required a GoPro CLA; that requirement is not presented here
as a new requirement for this fork.

See the [documentation index](docs/README.md) for the current product and
integration guides.
