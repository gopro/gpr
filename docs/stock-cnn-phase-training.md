# Matched phase-consistency training

Training reduces the supplied 2x model's synthetic translation error, but none
of the six checkpoints preserves the required RAW fidelity. Supplied weights
and production inference remain unchanged. These results do not qualify a new
restored tier or actual-video quality.

## Fixed comparison

Three arms start from the same supplied stock-Q3 SR checkpoint. Each receives
600 AdamW updates, learning rate 1e-5, no weight decay, with snapshots at updates
200 and 600. All use the same source/patch/shift sequence: two distinct Nikon
and two distinct camera training sources per batch. Batch-normalization running
statistics remain frozen; convolution and affine parameters train normally.

The common fidelity loss averages four training-reference regions. Each
region's rounded-count MSE is divided by the frozen supplied model's MSE in
that same region, with denominator at least one count squared. This gives
low-error regions explicit weight rather than letting large absolute errors
dominate. Exact half-up count rounding has a straight-through derivative.

The phase term compares two predictions of the same input under a random
0..7 packed-pixel translation, excluding zero/zero. Packed Bayer channels are
not permuted. Interior crops keep wrapped edges outside the receptive field.
The three weights are **0, 0.1 and 1**; every arm computes the same shifted
forward pass. Inputs are actual stock-decoded counts with the existing extended
context. No strength sweep, checkpoint interpolation, gating or architecture
change is introduced.

The protocol is saved before training. Qualification retains all pooled and
per-group RAW shadow/highlight/detail guards against both bicubic and supplied
SR, tone/WB proxy nonregression and positive pooled RAW gain. Additional phase
guards require nonregression in every group and improvement on both pooled
axes. Every qualifier would then need all ten complete validation images and
actual calibrated rendering before final/reference/backend evaluation.

## Results on 320 validation patches

RAW gain is versus supplied SR. Phase reductions refer to integer-count MSE
under one-packed-pixel horizontal/vertical shifts on fixed interior cores.

| Phase weight | Update | RAW gain dB | Horizontal phase-MSE reduction | Vertical reduction | Qualified |
|---|---:|---:|---:|---:|---|
| 0 | 200 | +0.01271 | 15.44% | 26.33% | No |
| 0 | 600 | +0.02567 | 5.37% | 25.90% | No |
| 0.1 | 200 | -0.00442 | 68.75% | 75.38% | No |
| 0.1 | 600 | -0.01061 | 82.78% | 86.61% | No |
| 1 | 200 | -0.00573 | 69.70% | 76.15% | No |
| 1 | 600 | -0.01499 | 84.70% | 88.10% | No |

All six fail RAW guards, including Nikon shadows. The regularized checkpoints
also regress the Nikon tone proxy. The unregularized arm improves pooled RAW
scores but regresses Nikon phase consistency despite its pooled phase gains.
Every eight-packed-pixel aligned control remains exact. No candidate advances
to whole-image, rendered, final-reference, native or Pi evaluation.

This broader validation set also finds the supplied baseline's camera-interior
shadow MSE 0.249% above bicubic. Its existing narrower validation claims do not
approve this entire set. The new candidates must satisfy both controls; that
requirement is not relaxed to make a replacement qualify.

A separate post-disqualification diagnostic measures correction energy relative
to bicubic. At update 600 the 0.1/1 phase settings retain 70.3%/33.7% of the
supplied model's residual energy, while the zero-weight control grows to 4.96
times that energy. Those descriptive changes are not preserved-detail scores.
Reducing a shift metric or correction amplitude alone is insufficient evidence
of better reconstruction. Actual motion, occlusion, exposure variation and
appearance remain untested by this synthetic patch task.

## Independent checks and evidence

The study completes 1,800 updates in 90.64 seconds. A separate audit repeats
eight fidelity settings, reproducing every prediction hash and RAW/proxy score.
It repeats all seven neural phase settings with **edge replication instead of
wrapped input edges** and independent integer error aggregation. Every phase
delta hash, MSE and maximum matches exactly, confirming that wrapped boundary
pixels do not drive the measured core differences. BN buffers and checkpoint
metadata remain unchanged. Guard decisions are independently reconstructed.

The residual-energy audit separately reconstructs all seven settings using
expanded integer cross terms; every per-group energy/projection value matches.
Predicted count arrays stay external and reproduce original prediction hashes.
No post-disqualification metric selects a model or changes a threshold.

[Summary](stock-cnn-phase-receipts/summary.json),
[offline archive](stock-cnn-phase-receipts/evidence.tar.gz) and
[member hashes](stock-cnn-phase-receipts/manifest.json) retain 21 code/protocol/
result/audit/log files. Source media, checkpoints and count arrays remain at
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_cnn_phase_training_20261031`.
Existing historical pretraining/validation overlap and compressed-camera
reference limitations remain; these are not blind or sensor-truth results.
