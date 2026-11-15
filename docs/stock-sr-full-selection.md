# Full-image selection before final-reference evaluation

`tools/select_stock_sr.py` adds an explicit full-image selection stage for
stock-Q3/RGGB14 scale-two checkpoints. It uses only a completed audited
`train_stock_sr` dataset's **validation sources**. Train/validation/final group
and byte-identity checks remain enforced. Final sources are hashed for split
auditing but are not decoded or evaluated here; final-test patches must be absent.
The input/target array hashes must also match the dataset manifest.

The caller supplies every candidate explicitly. Before inference, the tool
writes a protocol binding all candidate hashes, validation sources, dataset,
SDK, code, tile/halo and device. It uses the existing stock simulation, source
SDK/independent-decoder agreement, tiled inference, editable pixel identities
and calibration checks. Each source is encoded/decoded once and its bicubic
reference is reused across candidates. Candidate inference is not approximated
by blending rounded output counts.

A candidate must avoid increased active, shadow, highlight and detail RMSE
on **every complete validation image**. Among those passing, the lowest
sample-weighted active MSE wins, with a strict improvement over bicubic required.
An exact tie retains the first explicit candidate. No qualifying candidate
returns `selected: null`; an unchanged bicubic model does not qualify merely
by matching the baseline. Receipts always retain `quality_approval=false`.
The tool does not train, alter or promote weights. Patch guards and final
quality evaluation remain separate requirements.

## Use

From the repository root with optional Torch/numpy/tifffile/imagecodecs:

```sh
python tools/select_stock_sr.py \
  --dataset /path/to/completed-scale-two-dataset \
  --sdk build-runtime/source/app/gpr_tools/gpr_tools --device mps \
  --checkpoint /path/to/explicit-candidate-a.pt \
  --checkpoint /path/to/explicit-candidate-b.pt \
  --output-dir /new/full-validation
```

Candidates must use the supported bundled scale-two architecture/count
contract. Run patch selection first, freeze the explicit candidate family,
then apply this full-image stage. If a candidate qualifies, freeze its hash
before evaluating final references. A validation result does not establish
broader-camera, rendered, sensor-noise or temporal quality. Group labels are
caller supplied; byte-identity checks do not exclude perceptual near-duplicates.
A failure leaves incomplete evidence marked `complete: false`; ordinary
numeric rejection is a completed selection with no chosen checkpoint.

## Ten-candidate follow-up

The [fixed study protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/study-protocol.json)
chooses one candidate from each of the ten snapshots in the previous
[relative-region experiment](stock-sr-relative.md): the lowest-MSE strength
passing the unchanged pooled/per-group patch guards at that snapshot. This
rule and all ten candidate hashes are recorded before full-image evaluation.
These are existing trained snapshots, not another training run. Their parent
family has already been tested on historical final references; this is an
adaptive follow-up, not a claim of newly unseen final data.

All **40 full-image inferences** complete in 72.76 s on Mac MPS. Every candidate
fails at least one full-image regional guard; all ten fail ForestFire_010.
Accordingly, **no checkpoint is selected and final evaluation is skipped**.
No new weights are supplied. The optional 1x checkpoint and camera runtime
are unchanged.

The step-3000 full-strength control exactly reproduces all four previous
validation outputs, decoded inputs and metrics, despite the benchmark's
additional perfect-PSNR handling. This checks the new selection path against
the independently recorded prior evaluation rather than treating successful
command execution as proof of equivalent results.

## Tests and evidence

All nine optional CNN tests pass on Mac (Torch 2.8.0, 2.406 s) and Pi 5
(Torch 2.14.1+cpu, 8.704 s). The new SDK-backed synthetic test selects an exact
known correction, rejects a harmful correction and unchanged bicubic, and
checks rejection of the entire candidate set, duplicate checkpoint bytes and
leaked final patches. Only the validation source is evaluated. Existing
recording/editable/CNN contracts remain in the suite.

The exact-correction case also exposed and now covers a bug in
`benchmark_stock_sr`: perfect reconstruction has infinite PSNR represented by
JSON null, so subtracting PSNR values crashed. Undefined infinite gains now
remain null (both perfect yields zero); model selection uses MSE and remains
well-defined. Existing finite-error calculations are unchanged.

Receipts:
[selector protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/protocol.json),
[selection](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/selection.json),
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/command.json),
[disposition](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/disposition.json),
[prior control](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/prior-control.json),
[Mac tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/mac-tests.json),
[Pi tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/pi-tests.json), and
[source 0](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/validation-0.json),
[source 1](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/validation-1.json),
[source 2](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/validation-2.json),
[source 3](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-full-selection-receipts/validation-3.json).

Photos, checkpoints, full-image outputs and detailed test logs remain external
under `/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_full_selection_20261030`.
The selector is an analysis command in the repository; it adds no ML dependency
to the installed camera/SDK libraries and does not change capture performance.

The later [source-wide loss study](stock-sr-source-regions.md) retains the
original guards and checks all 22 patch-qualified combinations on full images.
Its selected 1/32-strength model passes all four historical final references
on MPS/CPU and Pi execution/count checks. An explicit diagnostic checkpoint is
supplied; pooled gain is small (+0.00729 dB), and broader/restored-tier quality
approval remains open. Older failures above remain revision-bound.
