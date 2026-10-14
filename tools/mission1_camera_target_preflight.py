#!/usr/bin/env python3
"""Preflight a Mission 1/Pi target before running camera closure.

This writes a compact receipt that answers a narrower question than the
camera-handoff receipt: is the target host prepared to run the closure command,
and are the remaining camera-only assertions explicit?  It does not turn Pi
stand-in evidence into camera evidence.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


SCHEMA = "gpr.mission1_camera_target_preflight.v1"
STANDIN_TOKENS = (
    "stand-in",
    "file-backed",
    "bench_fused",
    "page-cache",
    "filesystem",
    "off-camera",
    "pi 5",
    "pi5",
)
REMOTE_SCRIPT = r'''
import base64
import json
import os
import shutil
import stat
import sys
from pathlib import Path

config = json.loads(base64.b64decode(CONFIG_B64).decode("utf-8"))

def path_info(path_text):
    path = Path(path_text)
    row = {
        "path": str(path),
        "exists": path.exists(),
        "is_file": path.is_file(),
        "is_dir": path.is_dir(),
    }
    if path.exists():
        st = path.stat()
        row["size_bytes"] = st.st_size
        row["executable"] = bool(st.st_mode & stat.S_IXUSR)
        row["is_char_device"] = stat.S_ISCHR(st.st_mode)
        row["is_fifo"] = stat.S_ISFIFO(st.st_mode)
        row["is_socket"] = stat.S_ISSOCK(st.st_mode)
    return row

def write_probe(path_text):
    path = Path(path_text)
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / ".gpr_mission1_preflight_write_test"
        probe.write_text("ok\n", encoding="utf-8")
        probe.unlink()
        return {"path": str(path), "writable": True}
    except Exception as exc:
        return {"path": str(path), "writable": False, "error": str(exc)}

def disk_row(path_text):
    path = Path(path_text)
    probe = path if path.exists() else path.parent
    try:
        usage = shutil.disk_usage(probe)
        return {
            "path": str(path),
            "probe_path": str(probe),
            "total_bytes": usage.total,
            "used_bytes": usage.used,
            "free_bytes": usage.free,
        }
    except Exception as exc:
        return {"path": str(path), "error": str(exc)}

repo = Path(config["repo_root"])
payload = {
    "host": os.uname().nodename,
    "machine": os.uname().machine,
    "tools": {name: shutil.which(name) for name in config["required_tools"]},
    "paths": {
        "repo_root": path_info(config["repo_root"]),
        "raw": path_info(config["raw"]),
        "output_dir": path_info(config["output_dir"]),
        "scratch_dir": path_info(config["scratch_dir"]),
    },
    "executables": {
        name: [path_info(str(repo / candidate)) for candidate in candidates]
        for name, candidates in config["executable_candidates"].items()
    },
    "write_probe": {
        "output_dir": write_probe(config["output_dir"]),
        "scratch_dir": write_probe(config["scratch_dir"]),
    },
    "disk": {
        "output_dir": disk_row(config["output_dir"]),
        "scratch_dir": disk_row(config["scratch_dir"]),
    },
}
print(json.dumps(payload))
'''


def path_info(path: Path) -> dict[str, Any]:
    row: dict[str, Any] = {
        "path": str(path),
        "exists": path.exists(),
        "is_file": path.is_file(),
        "is_dir": path.is_dir(),
    }
    if path.exists():
        st = path.stat()
        row["size_bytes"] = st.st_size
        row["executable"] = bool(st.st_mode & stat.S_IXUSR)
        row["is_char_device"] = stat.S_ISCHR(st.st_mode)
        row["is_fifo"] = stat.S_ISFIFO(st.st_mode)
        row["is_socket"] = stat.S_ISSOCK(st.st_mode)
    return row


def write_probe(path: Path) -> dict[str, Any]:
    try:
        path.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            prefix=".gpr_mission1_preflight_",
            dir=path,
            delete=False,
        ) as f:
            f.write("ok\n")
            probe = Path(f.name)
        probe.unlink()
        return {"path": str(path), "writable": True}
    except Exception as exc:
        return {"path": str(path), "writable": False, "error": str(exc)}


def disk_row(path: Path) -> dict[str, Any]:
    probe = path if path.exists() else path.parent
    try:
        usage = shutil.disk_usage(probe)
        return {
            "path": str(path),
            "probe_path": str(probe),
            "total_bytes": usage.total,
            "used_bytes": usage.used,
            "free_bytes": usage.free,
        }
    except Exception as exc:
        return {"path": str(path), "error": str(exc)}


def executable_candidates(args: argparse.Namespace) -> dict[str, list[str]]:
    return {
        "bench_fused": [
            args.bench,
            "build-closure/source/app/bench_fused/bench_fused",
            "build/source/app/bench_fused/bench_fused",
            "build/bin/bench_fused",
        ],
        "labs_encoder_bench_cli": [
            args.labs_encoder_bench_cli,
            "build-closure/bin/labs_encoder_bench_cli",
            "build/bin/labs_encoder_bench_cli",
            "build/source/app/labs_encoder_bench_cli",
        ],
        "fused_decode_cli": [
            args.fused_decode_cli,
            "build-closure/bin/fused_decode_cli",
            "build/bin/fused_decode_cli",
            "build/source/app/fused_decode_cli",
        ],
        "gvid_preview_rgb_cli": [
            args.preview_cli,
            "build-closure/bin/gvid_preview_rgb_cli",
            "build/bin/gvid_preview_rgb_cli",
            "build/source/app/gvid_preview_rgb_cli",
        ],
    }


def config(args: argparse.Namespace) -> dict[str, Any]:
    return {
        "repo_root": str(args.repo_root),
        "raw": str(args.raw),
        "output_dir": str(args.output_dir),
        "scratch_dir": str(args.scratch_dir),
        "required_tools": ["python3", "cmake", "git"],
        "executable_candidates": executable_candidates(args),
    }


def probe_local(args: argparse.Namespace) -> dict[str, Any]:
    cfg = config(args)
    repo = args.repo_root
    return {
        "host": os.uname().nodename,
        "machine": os.uname().machine,
        "tools": {name: shutil.which(name) for name in cfg["required_tools"]},
        "paths": {
            "repo_root": path_info(args.repo_root),
            "raw": path_info(args.raw),
            "output_dir": path_info(args.output_dir),
            "scratch_dir": path_info(args.scratch_dir),
        },
        "executables": {
            name: [path_info(repo / candidate) for candidate in candidates]
            for name, candidates in executable_candidates(args).items()
        },
        "write_probe": {
            "output_dir": write_probe(args.output_dir),
            "scratch_dir": write_probe(args.scratch_dir),
        },
        "disk": {
            "output_dir": disk_row(args.output_dir),
            "scratch_dir": disk_row(args.scratch_dir),
        },
    }


def probe_remote(args: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    started = time.time()
    config_b64 = base64.b64encode(json.dumps(config(args)).encode("utf-8")).decode("ascii")
    script = f"CONFIG_B64 = {config_b64!r}\n" + REMOTE_SCRIPT
    proc = subprocess.run(
        [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            f"ConnectTimeout={args.ssh_timeout_s}",
            args.target_host,
            "python3",
            "-",
        ],
        input=script,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    step = {
        "cmd": ["ssh", args.target_host, "python3", "-", "<mission1_preflight_probe>"],
        "returncode": proc.returncode,
        "elapsed_s": round(time.time() - started, 3),
        "stderr_tail": proc.stderr.splitlines()[-20:],
    }
    if proc.returncode != 0:
        return {}, step
    try:
        return json.loads(proc.stdout), step
    except json.JSONDecodeError as exc:
        step["returncode"] = 1
        step["stderr_tail"].append(f"invalid remote JSON: {exc}")
        return {}, step


def first_executable(rows: list[dict[str, Any]]) -> dict[str, Any] | None:
    for row in rows:
        if row.get("exists") is True and row.get("is_file") is True and row.get("executable") is True:
            return row
    return None


def add_check(checks: list[dict[str, Any]], name: str, passed: bool, detail: str) -> None:
    checks.append({"name": name, "passed": passed, "detail": detail})


def contains_standin_token(value: str) -> str | None:
    lowered = value.lower()
    for token in STANDIN_TOKENS:
        if token in lowered:
            return token
    return None


def validate_camera_label(
    checks: list[dict[str, Any]],
    blockers: list[str],
    *,
    assertion_name: str,
    label_name: str,
    value: str,
    required: bool,
) -> None:
    if not required:
        add_check(checks, label_name, True, "not asserted")
        return
    text = value.strip()
    if not text:
        add_check(checks, label_name, False, "missing")
        blockers.append(f"{assertion_name} label is missing")
        return
    token = contains_standin_token(text)
    if token:
        add_check(checks, label_name, False, f"contains stand-in token {token!r}: {text}")
        blockers.append(f"{assertion_name} label contains stand-in token {token!r}")
        return
    add_check(checks, label_name, True, text)


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    target_probe: dict[str, Any]
    target_step: dict[str, Any] | None = None
    if args.target_host:
        target_probe, target_step = probe_remote(args)
    else:
        target_probe = probe_local(args)

    checks: list[dict[str, Any]] = []
    blockers: list[str] = []

    if target_step is not None:
        ok = target_step["returncode"] == 0
        add_check(checks, "ssh target probe", ok, f"returncode={target_step['returncode']}")
        if not ok:
            blockers.append("target host is not reachable over noninteractive SSH")

    paths = target_probe.get("paths", {})
    repo = paths.get("repo_root", {})
    add_check(checks, "repo root exists", repo.get("is_dir") is True, repo.get("path", ""))
    if repo.get("is_dir") is not True:
        blockers.append("repo root is missing on target")

    raw = paths.get("raw", {})
    expected_raw_bytes = args.source_width * args.source_height * 2
    if args.raw_source_kind == "file_standin":
        raw_ok = raw.get("is_file") is True
        size = raw.get("size_bytes")
        size_ok = isinstance(size, int) and size >= expected_raw_bytes
        add_check(checks, "raw source exists", raw_ok, raw.get("path", ""))
        add_check(checks, "raw source has unpacked Bayer size", size_ok, f"size={size} expected_min={expected_raw_bytes}")
        if not raw_ok:
            blockers.append("raw Bayer source is missing on target")
        elif not size_ok:
            blockers.append("raw Bayer source is smaller than expected unpacked 4096x3072 u16 frame")
    else:
        endpoint_exists = raw.get("exists") is True
        endpoint_device_like = (
            endpoint_exists
            and raw.get("is_file") is not True
            and raw.get("is_dir") is not True
            and (
                raw.get("is_char_device") is True
                or raw.get("is_fifo") is True
                or raw.get("is_socket") is True
            )
        )
        add_check(checks, "camera raw source endpoint exists", endpoint_exists, raw.get("path", ""))
        add_check(
            checks,
            "camera raw source endpoint is device-like",
            endpoint_device_like,
            "is_file={} is_dir={} is_char_device={} is_fifo={} is_socket={}".format(
                raw.get("is_file"),
                raw.get("is_dir"),
                raw.get("is_char_device"),
                raw.get("is_fifo"),
                raw.get("is_socket"),
            ),
        )
        if not endpoint_exists:
            blockers.append("camera raw source endpoint is missing on target")
        elif not endpoint_device_like:
            blockers.append("camera raw source endpoint is not a device-like stream")

    for tool, path in target_probe.get("tools", {}).items():
        ok = isinstance(path, str) and bool(path)
        add_check(checks, f"tool available: {tool}", ok, str(path))
        if not ok:
            blockers.append(f"required tool missing on target: {tool}")

    for name, rows in target_probe.get("executables", {}).items():
        match = first_executable(rows if isinstance(rows, list) else [])
        add_check(checks, f"executable available: {name}", match is not None, match.get("path", "") if match else "missing")
        if match is None:
            blockers.append(f"target binary is missing or not executable: {name}")

    for label in ("output_dir", "scratch_dir"):
        row = target_probe.get("write_probe", {}).get(label, {})
        ok = row.get("writable") is True
        add_check(checks, f"{label} writable", ok, row.get("path", ""))
        if not ok:
            blockers.append(f"{label} is not writable on target")

    camera_assertions = {
        "camera frame source ready": args.camera_frame_source_ready,
        "camera storage path ready": args.camera_storage_path_ready,
        "camera display path ready": args.camera_display_path_ready,
    }
    if args.target_role == "camera":
        for label, value in camera_assertions.items():
            add_check(checks, label, bool(value), "operator assertion")
            if not value:
                blockers.append(label)
        validate_camera_label(
            checks,
            blockers,
            assertion_name="camera frame source ready",
            label_name="camera frame source label",
            value=args.frame_source,
            required=args.camera_frame_source_ready,
        )
        validate_camera_label(
            checks,
            blockers,
            assertion_name="camera storage path ready",
            label_name="camera storage path label",
            value="; ".join(part for part in (args.write_path, args.storage_medium) if part.strip()),
            required=args.camera_storage_path_ready,
        )
        validate_camera_label(
            checks,
            blockers,
            assertion_name="camera display path ready",
            label_name="camera display path label",
            value="; ".join(part for part in (args.display_surface, args.presentation_path) if part.strip()),
            required=args.camera_display_path_ready,
        )

    ready = all(check["passed"] for check in checks)
    report = {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "target": {
            "host": args.target_host or "local",
            "role": args.target_role,
            "name": args.target_name,
        },
        "inputs": {
            "repo_root": str(args.repo_root),
            "raw": str(args.raw),
            "output_dir": str(args.output_dir),
            "scratch_dir": str(args.scratch_dir),
            "raw_source_kind": args.raw_source_kind,
            "source_width": args.source_width,
            "source_height": args.source_height,
            "frame_source": args.frame_source,
            "write_path": args.write_path,
            "storage_medium": args.storage_medium,
            "display_surface": args.display_surface,
            "presentation_path": args.presentation_path,
        },
        "target_probe": target_probe,
        "target_step": target_step,
        "checks": checks,
        "blockers": blockers,
        "verdict": {
            "target_preflight_ready": ready,
            "camera_closure_possible": ready and args.target_role == "camera",
            "remaining_blocker_count": len(blockers),
        },
    }
    return report


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target-host", default="")
    ap.add_argument("--ssh-timeout-s", type=int, default=5)
    ap.add_argument("--target-name", default="Mission 1")
    ap.add_argument("--target-role", choices=("stand-in", "camera"), default="camera")
    ap.add_argument("--repo-root", type=Path, default=Path("/mnt/ssd/gpr_work/worktrees/current_goal_sync_20260625_codex_followup"))
    ap.add_argument("--raw", type=Path, default=Path("/mnt/ssd/mission1_native12/GP017602.raw"))
    ap.add_argument("--raw-source-kind", choices=("file_standin", "sensor_dma_capture", "camera_ring_buffer"), default="file_standin")
    ap.add_argument("--output-dir", type=Path, default=Path("/mnt/ssd/gpr_work/artifacts/mission1_camera_closure_run_20260625/current_camera"))
    ap.add_argument("--scratch-dir", type=Path, default=Path("/mnt/ssd/gpr_work/tmp"))
    ap.add_argument("--bench", default="build-closure/source/app/bench_fused/bench_fused")
    ap.add_argument("--labs-encoder-bench-cli", default="build-closure/bin/labs_encoder_bench_cli")
    ap.add_argument("--fused-decode-cli", default="build-closure/bin/fused_decode_cli")
    ap.add_argument("--preview-cli", default="build-closure/bin/gvid_preview_rgb_cli")
    ap.add_argument("--source-width", type=int, default=4096)
    ap.add_argument("--source-height", type=int, default=3072)
    ap.add_argument("--camera-frame-source-ready", action="store_true")
    ap.add_argument("--camera-storage-path-ready", action="store_true")
    ap.add_argument("--camera-display-path-ready", action="store_true")
    ap.add_argument("--frame-source", default="")
    ap.add_argument("--write-path", default="")
    ap.add_argument("--storage-medium", default="")
    ap.add_argument("--display-surface", default="")
    ap.add_argument("--presentation-path", default="")
    ap.add_argument("--output-json", type=Path)
    ap.add_argument("--require-ready", action="store_true")
    return ap


def main() -> int:
    args = parser().parse_args()
    report = build_report(args)
    text = json.dumps(report, indent=2) + "\n"
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    if args.require_ready and report["verdict"]["target_preflight_ready"] is not True:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
