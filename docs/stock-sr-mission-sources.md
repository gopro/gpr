# Mission 1 training-source availability and scene grouping

The archived Mission 1 corpus contains 42 distinct 8192x6144 little-endian RAW
count references. Every file is present, has the expected byte count and fits
the declared 14-bit domain. New SHA-256 values bind the exact cached arrays.
Their original GPR paths are currently unavailable, so this audit does not
claim a fresh independent decode of those original files.

The archived decode manifest records 40 successful decodes and two cached
entries. The first audit incorrectly assumed every entry had a fresh success
record and stopped at a cached entry; the corrected audit retains that distinction
and validates every cached array's current bytes/range. Archived timestamps,
model and ISO records are historical metadata, not newly extracted originals.

A contact sheet of sampled grayscale count previews was inspected for repeated
content. The 42 files represent approximately three coarse venues, with bursts
and repeated playground/home dates; they are not 42 independent scenes. A
conservative split for stock-pair training groups all playground dates
together, all home-interior dates together, and the lake sequence together:

| Group | Proposed split | References |
|---|---|---:|
| Playground across June 4/6/11/12 | Train | 28 |
| Home interiors across June 1/16 | Validation | 6 |
| Lake sequence on June 7 | Test | 8 |

All count hashes are unique; eleven conservative dHash similarity hints remain
inside these splits. This is not exhaustive near-duplicate exclusion. The same
people can appear across venues, and the contact sheet/count ranges were
inspected before the split; no pristine blind-test claim is made. The [calibrated mixed-camera follow-up](stock-sr-calibrated-mixed.md) now
uses this split for actual stock-Q3 pairs and controlled training; none of its
eight checkpoints qualifies for promotion.

The three actually supplied GPRs separately have freshly extracted RGGB14
metadata with black level 16 and white level 16383. The recent Nikon-only
training/validation sources all have black 1008 and white 15892. That actual
camera-domain difference is relevant to future calibration-aware training;
source calibration equality within the Nikon set did not explain its own
shadow failures. The freshly supplied metadata does not establish per-frame
calibration for all 42 cached arrays. CFA/phase and calibration provenance
must be bound explicitly before future camera-quality claims.

[Count/provenance audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-mission-source-receipts/audit.json),
[similarity hints](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-mission-source-receipts/similarity-hints.json),
[proposed groups](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-mission-source-receipts/scene-groups.json) and
[manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-mission-source-receipts/manifest.json) preserve this evidence.
Photos and the visually inspected contact sheet stay external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_mission_sources_20261030`.
Camera GPR-derived arrays are compressed count references, not lossless sensor
truth. The existing supplied models are unchanged.

The [calibration recovery and CLI correction](stock-sr-calibrated-mixed.md) now binds per-frame archived CFA/black/white/WB/profile records for all 42 references, corroborates ten against separate source-GPR parameter archives, and independently audits all 1,088 regenerated non-test patches. This is recovered archived calibration at printed precision, without fresh original-file decoding or original binary-opcode identity.
