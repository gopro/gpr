# Optional serialization of encoder and preview compute

`gpr_capture_app --preview-sync-stages` waits for the preview queue to drain
after each encoded frame is enqueued to the writer. The writer can run during
that wait; the next encode cannot. Default scheduling remains overlapped.
This is an explicit scheduling tradeoff, not a change to compression, calibrated
pixels, source pacing, attempted frame count or drop reporting.

The reusable `gpr_preview_queue_flush` drains without closing submission and
returns the sticky preview error. It does not promise presentation or storage
durability. Concurrent offers can extend its wait; cancellation can discard a
pending item, so callers needing delivery counts must inspect queue stats.

The additive `gpr_capture_set_encoder_wait` configures an encoder-thread callback
before the first successful submission. It runs outside capture/codec locks,
after input-lease release and writer enqueue, with a copied frame index and no
borrowed slot/payload pointer. The writer may finish and recycle the slot while
the callback runs. Capture flush includes an active callback even if the writer
has released every frame slot. Finish joins it. Cancellation cannot interrupt
application waits; callbacks must return cooperatively. Callback context must
outlive finish/destroy, and callbacks may query stats but must not submit,
flush, finish or destroy their capture.

## Matched Pi comparison

All four runs use the same executable, 600 attempted frames paced at 20 fps,
4096x3072 RGGB14 Q3, leased source buffers, both frozen real-source/calibration
pairs, encoder-derived quarter-scale preview, a memory CRC sink, background
preview priority, 1 MiB page-aligned rolling writeback and final fsync. Existing
background processes were left unchanged. CPU governor/clock and before/after
temperatures are in the [receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/post-enqueue-abba.json).

| Policy | Recorded / previewed | Durable fps | Encode median, ms | Preview render median, ms | Preview latency p95, ms |
|---|---:|---:|---:|---:|---:|
| Overlapped A | 505 / 505 | 16.7550 | 56.9452 | 39.3669 | 274.834 |
| Serialized B | 551 / 551 | 18.2757 | 44.6464 | 9.2060 | 221.383 |
| Serialized B | 562 / 562 | 18.6330 | 45.0585 | 8.0651 | 217.303 |
| Overlapped A | 495 / 495 | 16.4086 | 58.6286 | 41.0017 | 280.269 |

The means of the two durable-fps results improve by approximately 11.3% in this
comparison. Both serial runs outperform both controls, supporting retention as
an opt-in policy. This does not establish a universal speedup: source admission
can differ after overruns, and previous unchanged-binary runs also varied.
All four runs overrun and **every delivered preview misses 50 ms**. The strict
20 fps floor remains unmet. These are supplied-frame replays, not a sensor,
rear display, SD card, thermal soak or real-time CNN measurement.

Every accepted compressed payload matches the independent historical stock
encoder. Source calibration and all delivered preview pixels match frozen
offline playback; frame/source/timestamp associations, final framing and queue
accounting are checked. There are no preview replacements or discarded offers.
The serialization wait totals 4.79/4.16 seconds for the two serial runs. That
wait is outside the per-frame encode timer, but remains inside recording wall
time, delivery latency and durable fps. Lower encode medians alone are not the
end-to-end speedup claim.

## Earlier placement

A first prototype waited before writer enqueue. Its matched runs recorded
511/549/546/508 frames, respectively. Serial rendering was about 6 ms, but
storage could not start for that frame until preview finished. The final design
enqueues first, allowing storage to overlap preview. The two experiments ran
at different times and do not isolate a universal placement speedup.

The earlier [receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/pre-enqueue-abba.json),
[runtime patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/pre-enqueue-source.patch) against
`f34aee9`, and verified [source hashes](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/pre-enqueue-source-hashes.json)
preserve that prototype. The runtime patch covers its three modified runtime
files, not its test/benchmark additions.

## Validation and reproduction

Tests cover reusable queue draining, pending/active callbacks, sticky preview
failure, successful later offers, writer progress during the encoder wait,
flush waiting after writer completion, callback frame order, setter lifecycle,
normal recording and isolated display failure. Installed C clients link both
new functions. All 24 Mac and 21 Pi Release targets and four stripped/relocated
package profiles pass; source identities are bound in the
[validation receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/validation.json).

[Commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/commands.json), binary/source hashes,
full numerical receipts, [Mac](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/mac-release.txt)
and [Pi](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/pi-release.txt) test output and the
[comparison driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/run-post-enqueue.py) are retained.
Driver paths refer to the measured Pi installation and frozen external inputs;
adjust them for another machine. For new inputs, the general
`tools/benchmark_mission1.py --preview-sync-stages` option applies the policy
to its recording-preview run and retains independent payload/pixel checks.

## Priority control

A subsequent same-executable background/foreground/foreground/background
comparison, with serialization always enabled, records 557/560/564/565 frames
at 18.4821/18.5694/18.7114/18.7521 durable fps. Render medians are
8.442/8.501/8.406/8.500 ms. There is no consistent priority benefit; defaults
remain unchanged. All four runs overrun and every preview is late. Exact
accepted payload/pixel checks pass. The [receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/priority-abba.json),
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/priority-commands.json) and
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-stage-receipts/run-priority.py) preserve this control.

The following [direct-output change](stock-preview-direct-output.md) instead
removes a measured reconstruction copy and unused scratch bands. Its separate
comparison improves serialized replay further while retaining missed-deadline
accounting; it does not close the timing gate.
