# Matched cleanup and SR training on stock inputs

Joint training improves pooled patch fidelity more than either comparison,
but all six checkpoints regress in Nikon shadows. No replacement is selected,
and the supplied weights and production inference defaults stay unchanged.

The previous frozen cleanup-to-SR composition failed camera-image checks.
This study tests a training relationship rather than another inference-strength
sweep. It uses the currently supplied four-channel BIBO architectures and
weights; the older coordinate/detail model already had preclean supervision
and is a separate architecture, not a new discovery in this comparison.

## Fixed comparison

Three arms start from the same supplied SR checkpoint: train SR alone; train
SR after frozen supplied cleanup; or train cleanup and SR together with an
additional supervised low-resolution cleanup loss. All use the same final RAW,
gradient and calibrated tone/WB proxy objectives, sampling sequence, learning
rate and 400-update budget. Each update samples two distinct Nikon and two
distinct camera training sources. Checkpoints are fixed at updates 200/400.
Batch-normalization running statistics are frozen in every arm; trainable
weights still receive gradients.

Training consumes actual stock-Q3 decoded counts. Cleanup produces rounded
14-bit intermediate counts with no codec re-encoding. Straight-through rounding
keeps that exact forward operation while permitting joint gradients.
Targets, scene splits and the 320 validation patch cores remain fixed.

Two cascaded networks need more context than the earlier single-network patch
halo. Inputs therefore expand from 128 to 256 packed-Bayer pixels using the
actual full-frame decoded data. Every original 128-pixel input matches the
existing dataset exactly across all 1,728 training/validation patches. Grid
phase and true frame edges are preserved, with independent replicated padding
for each stage. Computed cleanup pixels outside the real frame are discarded
before SR padding. The initial setup catches an edge/grid-alignment assertion
before training; its code/error are retained beside the corrected protocol.

## Patch results

Gain is pooled RAW PSNR relative to the supplied SR model, on identical cores.

| Training arm | Update 200 | Update 400 | Qualification |
|---|---:|---:|---|
| SR alone | +0.03270 dB | +0.07369 dB | Fails Nikon shadows |
| SR after frozen cleanup | +0.03983 dB | +0.10964 dB | Fails Nikon shadows; update 200 also regresses camera shadows and Nikon detail |
| Joint cleanup and SR | +0.05819 dB | +0.14394 dB | Fails Nikon shadows |

Every checkpoint must preserve all four RAW regions in every source group
against both bicubic and supplied SR, preserve each group's tone proxy, and
strictly improve pooled RAW fidelity. All six pass the tone-proxy guard but
fail the RAW guard. At update 400, joint training raises shadow MSE by 4.20%
on ForestFire and 15.67% on ColoradoSky versus supplied SR. Those failures
remain visible despite its better pooled result.

An independent evaluator reruns three controls and all six checkpoints,
reproduces every RAW regional score exactly, and checks the tone proxy with
NumPy rather than the producer's Torch implementation. Full-image, actual
rendering, final-reference, temporal, native and Pi evaluations are not run
for these disqualified candidates. Patch improvements establish none of those
capabilities. The result rejects this fixed recipe/budget, not joint training
as a general method.

[Summary](stock-cnn-joint-receipts/summary.json),
[compressed code/protocol/results](stock-cnn-joint-receipts/evidence.tar.gz)
and [member hashes](stock-cnn-joint-receipts/manifest.json) retain the evidence
without adding loose generated logs to the feature diff. External model
checkpoints and source media remain under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_cnn_joint_20261031`.
These are reused historical validation sources, with prior model pretraining
overlap and compressed camera references; no blind or sensor-truth claim follows.
