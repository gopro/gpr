#!/usr/bin/env python3
"""Run the Mission 1 camera-side closure sequence.

This is the executable path for the two remaining numbered-list blockers:

1. RAW 4K Bayer -> 4K `.gvid` camera handoff receipt.
2. 4K `.gvid` -> camera-back preview UI receipt.

The tool can run the bench and preview decode itself or consume existing
receipts. It validates both normalized receipts and writes a compact run
summary. It does not convert stand-in evidence into camera evidence; camera
production requires explicit camera-role handoff/UI flags and receipts that
pass the existing validators.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


SCHEMA = "gpr.mission1_camera_closure_run.v1"
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGET = "mission1_preview_4x_1024x768"
CAMERA_RAW_SOURCE_KINDS = {"sensor_dma_capture", "camera_ring_buffer"}


PROFILE_ENV = {
    "FUSED_PIN": "1",
    "FUSED_PIN_P2": "1",
    "GPR_INCLUDE_LL": "1",
    "FUSED_RAW_LL": "1",
    "FUSED_LL_PREDICT": "1",
    "FUSED_LL_PREDICTOR": "avg",
    "FUSED_LL_RICE_KS": "7,5,5,5",
    "FUSED_LL_RICE_FAST": "1",
    "FUSED_LL_ASSUME_U16": "1",
    "FUSED_INLINE_TOKENIZE": "1",
    "FUSED_DEFER_RANS": "1",
    "GPR_BENCH_GVID_SCATTER": "1",
    "FUSED_REFERENCE_HORIZONTAL": "1",
    "FUSED_STRIPE_ROWS": "384",
    "GPR_INLINE_DENOISE_HARD": "1",
    "GPR_INLINE_DENOISE_T_LH": "2",
    "GPR_INLINE_DENOISE_T_HL": "3",
    "GPR_INLINE_DENOISE_T_HH": "3",
}


def fail_before_run(summary_path: Path, reason: str, detail: str) -> int:
    write_failure_summary(
        summary_path,
        [
            {
                "name": "preflight",
                "cmd": [],
                "returncode": 1,
                "elapsed_s": 0.0,
                "stdout_tail": [],
                "stderr_tail": [detail],
            }
        ],
        reason,
    )
    print(detail, file=sys.stderr)
    return 1


def run(cmd: list[str], *, env: dict[str, str] | None = None, cwd: Path = ROOT) -> dict[str, Any]:
    started = time.time()
    proc = subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return {
        "cmd": cmd,
        "returncode": proc.returncode,
        "elapsed_s": round(time.time() - started, 3),
        "stdout_tail": proc.stdout.splitlines()[-40:],
        "stderr_tail": proc.stderr.splitlines()[-40:],
    }


def read_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"{path} must contain a JSON object")
    return data


def target_role(path: Path) -> str | None:
    data = read_json(path)
    target = data.get("target")
    if isinstance(target, dict) and isinstance(target.get("role"), str):
        return target["role"]
    return None


def target_bench_is_simulated(path: Path) -> bool:
    data = read_json(path)
    return data.get("simulated") is True


def sha256_value(data: dict[str, Any], *keys: str) -> str | None:
    current: Any = data
    for key in keys:
        if not isinstance(current, dict):
            return None
        current = current.get(key)
    if isinstance(current, str) and len(current) == 64:
        return current
    return None


def aggregate_consistency(
    target: dict[str, Any],
    handoff: dict[str, Any],
    preview_ui: dict[str, Any],
) -> tuple[bool, list[str]]:
    failures: list[str] = []
    if target.get("schema") != "gpr_labs_target_bench.v1":
        failures.append("target_bench schema must be gpr_labs_target_bench.v1")
    if target.get("simulated") is True:
        failures.append("target_bench must not be simulated")
    target_capture = target.get("capture") if isinstance(target.get("capture"), dict) else {}
    target_verdict = target.get("verdict") if isinstance(target.get("verdict"), dict) else {}
    if target_verdict.get("target_evidence") is not True:
        failures.append("target_bench.verdict.target_evidence must be true")

    handoff_input = handoff.get("input_frame") if isinstance(handoff.get("input_frame"), dict) else {}
    handoff_capture = handoff.get("capture") if isinstance(handoff.get("capture"), dict) else {}
    preview_source = preview_ui.get("source") if isinstance(preview_ui.get("source"), dict) else {}
    comparisons = (
        ("source width", target_capture.get("source_width"), handoff_input.get("width")),
        ("source height", target_capture.get("source_height"), handoff_input.get("height")),
        ("capture width", target_capture.get("capture_width"), preview_source.get("width")),
        ("capture height", target_capture.get("capture_height"), preview_source.get("height")),
        ("pixel format", target_capture.get("pixel_format"), handoff_input.get("pixel_format")),
        ("preview pixel format", target_capture.get("pixel_format"), preview_source.get("pixel_format")),
        ("frames written", target_capture.get("frames_written"), handoff_capture.get("frames_written")),
        ("preview frame count", target_capture.get("frames_written"), preview_source.get("frame_count")),
        ("dropped frames", target_capture.get("dropped_frames"), handoff_capture.get("dropped_frames")),
    )
    for label, left, right in comparisons:
        if left != right:
            failures.append(f"{label} mismatch: target_bench={left!r} receipt={right!r}")

    target_sha = sha256_value(target, "gvid", "sha256")
    handoff_sha = sha256_value(handoff, "output", "sha256")
    preview_sha = sha256_value(preview_ui, "source", "gvid_sha256")
    if handoff_sha != preview_sha:
        failures.append("camera_handoff output.sha256 must match preview_ui source.gvid_sha256")
    if target_sha is not None and target_sha != handoff_sha:
        failures.append("target_bench gvid.sha256 must match camera_handoff output.sha256 when present")

    target_source_sha = sha256_value(target, "source_provenance", "sha256")
    handoff_source_sha = sha256_value(handoff, "source_provenance", "sha256")
    preview_source_sha = sha256_value(preview_ui, "source_provenance", "sha256")
    if handoff_source_sha != preview_source_sha:
        failures.append("camera_handoff and preview_ui source_provenance.sha256 must match")
    if target_source_sha is not None and target_source_sha != handoff_source_sha:
        failures.append("target_bench source_provenance.sha256 must match receipt source provenance")
    return not failures, failures


def validate_camera_preflight(args: argparse.Namespace, path: Path | None) -> tuple[str, str] | None:
    if path is None:
        return (
            "camera_role_requires_target_preflight",
            "camera-role Mission 1 closure requires --target-preflight-receipt",
        )
    if not path.exists():
        return (
            "camera_role_target_preflight_missing",
            f"camera-role Mission 1 closure target preflight does not exist: {path}",
        )
    try:
        preflight = read_json(path)
    except Exception as exc:
        return (
            "camera_role_target_preflight_invalid_json",
            f"camera-role Mission 1 closure target preflight is not valid JSON: {path}: {exc}",
        )
    if preflight.get("schema") != "gpr.mission1_camera_target_preflight.v1":
        return (
            "camera_role_target_preflight_wrong_schema",
            "camera-role Mission 1 closure requires gpr.mission1_camera_target_preflight.v1",
        )
    target = preflight.get("target")
    if not isinstance(target, dict) or target.get("role") != "camera":
        return (
            "camera_role_target_preflight_wrong_role",
            "camera-role Mission 1 closure requires target preflight target.role='camera'",
        )
    inputs = preflight.get("inputs")
    if not isinstance(inputs, dict):
        return (
            "camera_role_target_preflight_missing_inputs",
            "camera-role Mission 1 closure requires target preflight inputs",
        )
    receipt_source_kind = inputs.get("raw_source_kind")
    if receipt_source_kind not in CAMERA_RAW_SOURCE_KINDS:
        return (
            "camera_role_target_preflight_wrong_raw_source_kind",
            "camera-role Mission 1 closure requires target preflight raw_source_kind "
            f"in {sorted(CAMERA_RAW_SOURCE_KINDS)}, got {receipt_source_kind!r}",
        )
    if args.raw_source_kind != receipt_source_kind:
        return (
            "camera_role_raw_source_kind_mismatch",
            "camera-role Mission 1 closure raw source kind must match target preflight: "
            f"arg={args.raw_source_kind!r} receipt={receipt_source_kind!r}",
        )
    receipt_raw = inputs.get("raw")
    if str(args.raw) != str(receipt_raw):
        return (
            "camera_role_raw_path_mismatch",
            "camera-role Mission 1 closure raw path must match target preflight: "
            f"arg={args.raw!s} receipt={receipt_raw!r}",
        )
    verdict = preflight.get("verdict")
    if not isinstance(verdict, dict):
        return (
            "camera_role_target_preflight_missing_verdict",
            "camera-role Mission 1 closure requires target preflight verdict",
        )
    if verdict.get("target_preflight_ready") is not True:
        return (
            "camera_role_target_preflight_not_ready",
            "camera-role Mission 1 closure requires target_preflight_ready=true",
        )
    if verdict.get("camera_closure_possible") is not True:
        return (
            "camera_role_closure_not_possible",
            "camera-role Mission 1 closure requires camera_closure_possible=true",
        )
    blockers = preflight.get("blockers")
    if blockers:
        return (
            "camera_role_target_preflight_has_blockers",
            f"camera-role Mission 1 closure target preflight still has blockers: {blockers}",
        )
    return None


def maybe_flag(enabled: bool, flag: str) -> list[str]:
    return [flag] if enabled else []


def target_bench_cmd(args: argparse.Namespace, target_receipt: Path) -> list[str]:
    cmd = [
        sys.executable,
        str(ROOT / "tools/run_labs_target_bench.py"),
        "--output-dir",
        str(target_receipt.parent),
        "--frames",
        str(args.frames),
        "--target-fps",
        str(args.target_fps),
        "--source-width",
        str(args.source_width),
        "--source-height",
        str(args.source_height),
        "--capture-width",
        str(args.capture_width),
        "--capture-height",
        str(args.capture_height),
        "--quality",
        str(args.quality),
        "--wavelet-levels",
        str(args.wavelet_levels),
        "--pixel-format",
        str(args.pixel_format),
        "--storage-target-name",
        args.storage_target_name,
        "--storage-target-read-mbps",
        str(args.storage_target_read_mbps),
        "--storage-target-write-mbps",
        str(args.storage_target_write_mbps),
        "--storage-target-safety-margin",
        str(args.storage_target_safety_margin),
        "--storage-target-note",
        args.storage_target_note,
    ]
    if args.no_decimate:
        cmd.append("--no-decimate")
    else:
        cmd.extend(["--col-decimate", str(args.col_decimate), "--row-decimate", str(args.row_decimate)])
    if args.direct_gvid:
        cmd.append("--direct-gvid")
    if args.simulate_target_bench:
        cmd.append("--simulate")
    else:
        cmd.extend(["--bench", str(args.bench), "--raw", str(args.raw)])
    if args.source_provenance_root:
        cmd.extend(["--source-provenance-root", str(args.source_provenance_root)])
    if args.force_target_evidence:
        cmd.append("--target-evidence")
    return cmd


def handoff_cmd(args: argparse.Namespace, target_receipt: Path, output: Path) -> list[str]:
    cmd = [
        sys.executable,
        str(ROOT / "tools/labs_target_to_camera_handoff_receipt.py"),
        str(target_receipt),
        "--output",
        str(output),
        "--target-name",
        args.target_name,
        "--target-role",
        args.target_role,
        "--target-fps",
        str(args.target_fps),
        "--raw-source-kind",
        args.raw_source_kind,
        "--frame-source",
        args.frame_source,
        "--memory-ownership",
        args.memory_ownership,
        "--write-path",
        args.write_path,
        "--storage-medium",
        args.storage_medium,
        "--storage-ownership",
        args.storage_ownership,
        "--stride-bytes",
        str(args.stride_bytes),
        "--bit-depth",
        str(args.bit_depth),
        "--pixel-format",
        str(args.pixel_format),
        "--blocker-cause",
        args.handoff_blocker_cause,
    ]
    cmd += maybe_flag(args.sensor_dma_executed, "--sensor-dma-executed")
    cmd += maybe_flag(args.storage_handoff_executed, "--storage-handoff-executed")
    return cmd


def preview_decode_cmd(args: argparse.Namespace, target_receipt: Path, out_dir: Path) -> list[str]:
    target = read_json(target_receipt)
    gvid_path = target.get("gvid", {}).get("path")
    if not isinstance(gvid_path, str) or not gvid_path:
        raise ValueError("target bench receipt does not include gvid.path; provide --preview-receipt")
    return [
        sys.executable,
        str(ROOT / "tools/test/run_pi_gvid_decode_target_bench.py"),
        "--gvid",
        gvid_path,
        "--cli",
        str(args.fused_decode_cli),
        "--out-dir",
        str(out_dir),
        "--sensor-width",
        str(args.source_width),
        "--sensor-height",
        str(args.source_height),
        "--target",
        args.preview_target,
        "--limit",
        str(args.preview_limit),
    ]


def preview_ui_cmd(args: argparse.Namespace, target_receipt: Path, preview_receipt: Path, output: Path) -> list[str]:
    cmd = [
        sys.executable,
        str(ROOT / "tools/build_labs_preview_ui_receipt.py"),
        "--target-bench",
        str(target_receipt),
        "--preview-receipt",
        str(preview_receipt),
        "--output",
        str(output),
        "--target-name",
        args.target_name,
        "--target-role",
        args.target_role,
        "--display-surface",
        args.display_surface,
        "--presentation-path",
        args.presentation_path,
        "--buffer-ownership",
        args.preview_buffer_ownership,
        "--decode-path",
        args.preview_decode_path,
        "--color-pipeline",
        args.preview_color_pipeline,
        "--tone-pipeline",
        args.preview_tone_pipeline,
        "--blocker-cause",
        args.preview_blocker_cause,
    ]
    cmd += maybe_flag(args.ui_path_executed, "--ui-path-executed")
    cmd += maybe_flag(args.visual_checked, "--visual-checked")
    return cmd


def validator_cmd(tool: str, receipt: Path) -> list[str]:
    return [sys.executable, str(ROOT / tool), str(receipt)]


def write_failure_summary(summary_path: Path, steps: list[dict[str, Any]], reason: str) -> None:
    summary_path.write_text(
        json.dumps(
            {
                "schema": SCHEMA,
                "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "steps": steps,
                "verdict": {"production_ready": False, "reason": reason},
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--bench", type=Path, default=Path("build/source/app/bench_fused/bench_fused"))
    ap.add_argument("--raw", type=Path, default=Path("/mnt/ssd/mission1_native12/GP017602.raw"))
    ap.add_argument("--fused-decode-cli", type=Path, default=Path("build/source/app/fused_decode_cli"))
    ap.add_argument("--target-bench-receipt", type=Path, default=None)
    ap.add_argument("--target-preflight-receipt", type=Path, default=None)
    ap.add_argument("--camera-handoff-receipt", type=Path, default=None)
    ap.add_argument("--preview-receipt", type=Path, default=None)
    ap.add_argument("--preview-ui-receipt", type=Path, default=None)
    ap.add_argument("--simulate-target-bench", action="store_true")
    ap.add_argument("--force-target-evidence", action="store_true")
    ap.add_argument("--use-mission1-fll2-profile", action="store_true")
    ap.add_argument("--frames", type=int, default=1440)
    ap.add_argument("--target-fps", type=float, default=20.0)
    ap.add_argument("--source-width", type=int, default=4096)
    ap.add_argument("--source-height", type=int, default=3072)
    ap.add_argument("--capture-width", type=int, default=4096)
    ap.add_argument("--capture-height", type=int, default=3072)
    ap.add_argument("--quality", type=int, default=8)
    ap.add_argument("--wavelet-levels", type=int, default=1)
    ap.add_argument("--no-decimate", action="store_true", default=True)
    ap.add_argument("--col-decimate", type=int, default=1)
    ap.add_argument("--row-decimate", type=int, default=1)
    ap.add_argument("--pixel-format", type=int, default=1)
    ap.add_argument("--direct-gvid", action="store_true", default=True)
    ap.add_argument("--source-provenance-root", type=Path, default=None)
    ap.add_argument("--storage-target-name", default="Lexar Professional SILVER PLUS SDXC/microSDXC UHS-I (128GB-1TB)")
    ap.add_argument("--storage-target-read-mbps", type=float, default=205.0)
    ap.add_argument("--storage-target-write-mbps", type=float, default=150.0)
    ap.add_argument("--storage-target-safety-margin", type=float, default=0.90)
    ap.add_argument("--storage-target-note", default="Published 128GB-1TB SILVER PLUS profile: 205 MB/s read, 150 MB/s write; 64GB microSD is 205/100.")
    ap.add_argument("--target-name", default="Pi 5 stand-in")
    ap.add_argument("--target-role", choices=("stand-in", "camera"), default="stand-in")
    ap.add_argument("--raw-source-kind", choices=("file_standin", "sensor_dma_capture", "camera_ring_buffer"), default="file_standin")
    ap.add_argument("--frame-source", default="file-backed Bayer stand-in")
    ap.add_argument("--memory-ownership", default="synchronous submit; caller owns input through return")
    ap.add_argument("--write-path", default="bench_fused target-bench .gvid path")
    ap.add_argument("--sensor-dma-executed", action="store_true")
    ap.add_argument("--storage-handoff-executed", action="store_true")
    ap.add_argument("--storage-medium", default="target-bench filesystem stand-in")
    ap.add_argument("--storage-ownership", default="OS/page-cache writeback; not camera firmware DMA")
    ap.add_argument("--stride-bytes", type=int, default=8192)
    ap.add_argument("--bit-depth", type=int, default=14)
    ap.add_argument("--handoff-blocker-cause", default="camera sensor/DMA and camera storage handoff not executed")
    ap.add_argument("--preview-target", default=DEFAULT_TARGET)
    ap.add_argument("--preview-limit", type=int, default=0)
    ap.add_argument("--ui-path-executed", action="store_true")
    ap.add_argument("--visual-checked", action="store_true")
    ap.add_argument("--display-surface", default="stand-in raw preview receipt output")
    ap.add_argument("--presentation-path", default="off-camera preview decode receipt")
    ap.add_argument("--preview-buffer-ownership", default="process-owned RGB output buffer")
    ap.add_argument("--preview-decode-path", default="fused_decode_cli mission1_preview_4x_1024x768")
    ap.add_argument("--preview-color-pipeline", default="full-frame Bayer decode to RGB preview")
    ap.add_argument("--preview-tone-pipeline", default="preview tone path from fused decoder target")
    ap.add_argument("--preview-blocker-cause", default="Mission 1 camera UI/display path not executed")
    args = ap.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    target_receipt = args.target_bench_receipt or (args.output_dir / "labs_target_bench.json")
    target_preflight_receipt = args.output_dir / "target_preflight_receipt.json"
    handoff_receipt = args.output_dir / "camera_handoff_receipt.json"
    preview_dir = args.output_dir / "preview_decode_1024x768"
    preview_receipt = args.preview_receipt or (preview_dir / "receipt.json")
    preview_ui_receipt = args.output_dir / "preview_ui_receipt.json"
    summary_path = args.output_dir / "mission1_camera_closure_run.json"

    steps: list[dict[str, Any]] = []
    env = os.environ.copy()
    if args.use_mission1_fll2_profile:
        env.update(PROFILE_ENV)

    if args.target_role == "camera" and args.simulate_target_bench:
        return fail_before_run(
            summary_path,
            "camera_role_cannot_use_simulated_target_bench",
            "camera-role Mission 1 closure cannot use --simulate-target-bench",
        )
    if args.target_role == "camera" and args.target_bench_receipt is not None and target_bench_is_simulated(args.target_bench_receipt):
        return fail_before_run(
            summary_path,
            "camera_role_cannot_use_simulated_target_bench_receipt",
            f"camera-role Mission 1 closure cannot consume simulated target receipt: {args.target_bench_receipt}",
        )
    if args.target_role == "camera":
        camera_preflight_failure = validate_camera_preflight(args, args.target_preflight_receipt)
        if camera_preflight_failure is not None:
            reason, detail = camera_preflight_failure
            return fail_before_run(summary_path, reason, detail)

    if args.target_bench_receipt is None:
        step = run(target_bench_cmd(args, target_receipt), env=env)
        steps.append({"name": "target_bench", **step})
        if step["returncode"] != 0:
            write_failure_summary(summary_path, steps, "target_bench_failed")
            return 1

    if args.target_preflight_receipt is not None:
        if args.target_preflight_receipt.resolve() != target_preflight_receipt.resolve():
            shutil.copy2(args.target_preflight_receipt, target_preflight_receipt)
        steps.append(
            {
                "name": "copy_target_preflight_receipt",
                "cmd": ["copy", str(args.target_preflight_receipt), str(target_preflight_receipt)],
                "returncode": 0,
                "elapsed_s": 0.0,
                "stdout_tail": [],
                "stderr_tail": [],
            }
        )

    if args.camera_handoff_receipt is not None:
        if args.camera_handoff_receipt.resolve() != handoff_receipt.resolve():
            shutil.copy2(args.camera_handoff_receipt, handoff_receipt)
        steps.append(
            {
                "name": "copy_camera_handoff_receipt",
                "cmd": ["copy", str(args.camera_handoff_receipt), str(handoff_receipt)],
                "returncode": 0,
                "elapsed_s": 0.0,
                "stdout_tail": [],
                "stderr_tail": [],
            }
        )
    else:
        step = run(handoff_cmd(args, target_receipt, handoff_receipt), env=env)
        steps.append({"name": "build_camera_handoff_receipt", **step})
        if step["returncode"] != 0:
            write_failure_summary(summary_path, steps, "build_camera_handoff_receipt_failed")
            return 1
    step = run(validator_cmd("tools/check_labs_camera_handoff_receipt.py", handoff_receipt), env=env)
    steps.append({"name": "validate_camera_handoff_receipt", **step})
    if step["returncode"] != 0:
        write_failure_summary(summary_path, steps, "validate_camera_handoff_receipt_failed")
        return 1
    handoff_role = target_role(handoff_receipt)
    if handoff_role != args.target_role:
        write_failure_summary(summary_path, steps, "camera_handoff_receipt_role_mismatch")
        print(
            f"camera handoff receipt target.role={handoff_role!r} does not match requested target-role={args.target_role!r}",
            file=sys.stderr,
        )
        return 1

    if args.preview_receipt is None:
        preview_dir.mkdir(parents=True, exist_ok=True)
        step = run(preview_decode_cmd(args, target_receipt, preview_dir), env=env)
        steps.append({"name": "preview_decode", **step})
        if step["returncode"] != 0:
            write_failure_summary(summary_path, steps, "preview_decode_failed")
            return 1

    if args.preview_ui_receipt is not None:
        if args.preview_ui_receipt.resolve() != preview_ui_receipt.resolve():
            shutil.copy2(args.preview_ui_receipt, preview_ui_receipt)
        steps.append(
            {
                "name": "copy_preview_ui_receipt",
                "cmd": ["copy", str(args.preview_ui_receipt), str(preview_ui_receipt)],
                "returncode": 0,
                "elapsed_s": 0.0,
                "stdout_tail": [],
                "stderr_tail": [],
            }
        )
    else:
        step = run(preview_ui_cmd(args, target_receipt, preview_receipt, preview_ui_receipt), env=env)
        steps.append({"name": "build_preview_ui_receipt", **step})
        if step["returncode"] != 0:
            write_failure_summary(summary_path, steps, "build_preview_ui_receipt_failed")
            return 1
    step = run(validator_cmd("tools/check_labs_preview_ui_receipt.py", preview_ui_receipt), env=env)
    steps.append({"name": "validate_preview_ui_receipt", **step})
    if step["returncode"] != 0:
        write_failure_summary(summary_path, steps, "validate_preview_ui_receipt_failed")
        return 1
    preview_ui_role = target_role(preview_ui_receipt)
    if preview_ui_role != args.target_role:
        write_failure_summary(summary_path, steps, "preview_ui_receipt_role_mismatch")
        print(
            f"preview UI receipt target.role={preview_ui_role!r} does not match requested target-role={args.target_role!r}",
            file=sys.stderr,
        )
        return 1

    handoff = read_json(handoff_receipt)
    preview_ui = read_json(preview_ui_receipt)
    target_bench = read_json(target_receipt)
    target_preflight: dict[str, Any] = {}
    if target_preflight_receipt.exists():
        target_preflight = read_json(target_preflight_receipt)
    target_preflight_verdict = target_preflight.get("verdict", {}) if target_preflight else {}
    target_preflight_ready = target_preflight_verdict.get("target_preflight_ready") is True
    camera_closure_possible = target_preflight_verdict.get("camera_closure_possible") is True
    consistency_ready, consistency_failures = aggregate_consistency(target_bench, handoff, preview_ui)
    production_ready = (
        args.target_role == "camera"
        and handoff.get("verdict", {}).get("firmware_ready") is True
        and preview_ui.get("verdict", {}).get("ui_ready") is True
        and target_preflight_ready
        and camera_closure_possible
        and consistency_ready
    )
    receipts = {
        "target_bench": str(target_receipt),
        "camera_handoff": str(handoff_receipt),
        "preview_decode": str(preview_receipt),
        "preview_ui": str(preview_ui_receipt),
    }
    if target_preflight:
        receipts["target_preflight"] = str(target_preflight_receipt)
    summary = {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "output_dir": str(args.output_dir),
        "receipts": receipts,
        "steps": steps,
        "verdict": {
            "production_ready": production_ready,
            "target_preflight_ready": target_preflight_verdict.get("target_preflight_ready"),
            "camera_closure_possible": target_preflight_verdict.get("camera_closure_possible"),
            "aggregate_consistency_ready": consistency_ready,
            "aggregate_consistency_failures": consistency_failures,
            "firmware_ready": handoff.get("verdict", {}).get("firmware_ready"),
            "ui_ready": preview_ui.get("verdict", {}).get("ui_ready"),
            "handoff_blocker": handoff.get("blocker", {}).get("cause"),
            "preview_blocker": preview_ui.get("blocker", {}).get("cause"),
        },
    }
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"summary": str(summary_path), "production_ready": production_ready}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
