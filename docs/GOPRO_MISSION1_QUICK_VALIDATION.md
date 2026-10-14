# GoPro Mission 1 Quick Validation

The active target is native 4096 x 3072 Bayer capture and full-frame
1024 x 768 preview at the accepted 20+ fps floor. October 13 verification of
production `fdb9171` measured 23.8818 fps buffered but 17.6722 fps SSD fsync;
this is not durable SD 20 fps proof. Actual camera sensor/DMA, storage, and
display handoff remain unproven. No CNN runs on the camera. See
[Pi Hardware](PI_HARDWARE.md) for setup and measurement scope.

## Probe the source

Run from the repository root on the development target. Set
`GPR_CAMERA_RAW_SOURCE` to the real sensor/DMA endpoint supplied by the
firmware integration, and choose an artifact directory.

```bash
export GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-artifacts}"
mkdir -p "$GPR_ARTIFACT_ROOT"
python3 tools/mission1_camera_source_probe.py \
  --target-name "Mission 1" --target-role camera \
  --raw "${GPR_CAMERA_RAW_SOURCE:?Set the real camera raw endpoint}" \
  --raw-source-kind sensor_dma_capture \
  --source-width 4096 --source-height 3072 \
  --stride-bytes 8192 --bit-depth 14 --pixel-format 1 \
  --output-json "$GPR_ARTIFACT_ROOT/source_probe.json" --require-ready
```

The example declares RGGB14 in unpacked 16-bit rows. Match the actual
sensor layout; camera-ring input uses `camera_ring_buffer`.
A successful endpoint probe does not establish storage or display execution.

## Collect hardware receipts

Integrate through `source/lib/vc5_encoder/gpr_labs_encoder.h`.
Use `tools/mission1_camera_target_preflight.py --help` and
`tools/run_labs_target_bench.py --help` for the target configuration
options. The firmware team must supply actual camera source, write-buffer,
storage, and display ownership. See [Capture API](CAPTURE_API.md).

Place the resulting target preflight, target bench, capture handoff,
preview decode, and preview UI receipts under
`$GPR_ARTIFACT_ROOT/camera-input`. These inputs must describe one
consistent run and include output hashes and recovery proof. Receipt
booleans are assertions requiring underlying hardware evidence.

## Validate aggregate closure

The retained runner can consume those existing receipts:

```bash
python3 tools/run_mission1_camera_closure.py \
  --output-dir "$GPR_ARTIFACT_ROOT/camera" \
  --target-name "Mission 1" --target-role camera --target-fps 20 \
  --raw "${GPR_CAMERA_RAW_SOURCE:?Set the real camera raw endpoint}" \
  --raw-source-kind sensor_dma_capture \
  --target-preflight-receipt "$GPR_ARTIFACT_ROOT/camera-input/target_preflight_receipt.json" \
  --target-bench-receipt "$GPR_ARTIFACT_ROOT/camera-input/labs_target_bench.json" \
  --camera-handoff-receipt "$GPR_ARTIFACT_ROOT/camera-input/camera_handoff_receipt.json" \
  --preview-receipt "$GPR_ARTIFACT_ROOT/camera-input/preview_decode_receipt.json" \
  --preview-ui-receipt "$GPR_ARTIFACT_ROOT/camera-input/preview_ui_receipt.json"
python3 tools/check_mission1_camera_closure_run.py \
  "$GPR_ARTIFACT_ROOT/camera/mission1_camera_closure_run.json"
```

The raw endpoint and source kind must match the preflight exactly. Inspect
the aggregate `production_ready` verdict and its supporting receipts;
a process exit alone is not a camera-production signoff.

Production evidence needs actual sensor/DMA, camera-storage, and rear-display
execution, consistent capture/preview identity, zero drops, valid decoded
output, 20+ fps, full-frame preview, and interrupted-tail recovery. Keep
blocked receipts with their concrete cause. File replay, simulated cadence,
and off-camera preview remain stand-in tests.

[Release Artifacts](RELEASE_ARTIFACTS.md) defines the handoff bundle.
Device-specific launch histories and superseded wrappers are preserved in the
[archive](https://github.com/dcliftreaves/gpr/tree/archive/research-and-integration-2026-09-09),
pinned at [`3d675ef`](https://github.com/dcliftreaves/gpr/tree/3d675ef).
