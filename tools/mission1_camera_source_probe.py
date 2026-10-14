#!/usr/bin/env python3
"""Probe a Mission 1 camera raw source endpoint.

This is intentionally narrower than the full target preflight. It answers
whether a proposed raw source matches its declared source policy:

- `file_standin` must be a regular file large enough for one unpacked frame.
- `sensor_dma_capture` and `camera_ring_buffer` must be a device-like stream
  endpoint, not a regular fixture file.

The probe never reads from a camera device. It only checks the endpoint shape so
firmware integration can fail early without blocking on frame production.
"""
from __future__ import annotations

import argparse
import base64
import glob
import json
import os
import stat
import subprocess
import time
from pathlib import Path
from typing import Any


SCHEMA = "gpr.mission1_camera_source_probe.v1"
CAMERA_SOURCE_KINDS = {"sensor_dma_capture", "camera_ring_buffer"}
MAX_DISCOVERY_ROWS = 200
SOURCE_DISCOVERY_GLOBS = (
    "/dev/mission1/*",
    "/dev/*mission*",
    "/dev/*gopro*",
    "/dev/*raw*",
    "/dev/video*",
    "/dev/media*",
    "/dev/v4l/by-id/*",
    "/dev/v4l/by-path/*",
    "/run/mission1/*",
    "/tmp/mission1/*",
    "/mnt/ssd/mission1/*",
)
DISPLAY_DISCOVERY_GLOBS = (
    "/dev/fb*",
    "/dev/dri/*",
    "/sys/class/graphics/*",
    "/sys/class/drm/*",
)
REMOTE_SCRIPT = r'''
import base64
import glob
import json
import os
import stat
from pathlib import Path

config = json.loads(base64.b64decode(CONFIG_B64).decode("utf-8"))

def path_info(path):
    row = {
        "path": str(path),
        "exists": path.exists(),
        "is_file": path.is_file(),
        "is_dir": path.is_dir(),
    }
    if path.exists():
        try:
            st = path.stat()
            row["size_bytes"] = st.st_size
            row["mode"] = stat.S_IMODE(st.st_mode)
            row["is_char_device"] = stat.S_ISCHR(st.st_mode)
            row["is_block_device"] = stat.S_ISBLK(st.st_mode)
            row["is_fifo"] = stat.S_ISFIFO(st.st_mode)
            row["is_socket"] = stat.S_ISSOCK(st.st_mode)
            row["resolved_path"] = str(path.resolve())
        except OSError as exc:
            row["error"] = str(exc)
        try:
            name = path.resolve().name
            if name.startswith("video"):
                sys_name = Path("/sys/class/video4linux") / name / "name"
                if sys_name.exists():
                    row["sysfs_name"] = sys_name.read_text(encoding="utf-8", errors="replace").strip()
            if name.startswith("media"):
                sys_model = Path("/sys/class/media") / name / "model"
                if sys_model.exists():
                    row["sysfs_model"] = sys_model.read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            pass
    return row

def device_like(row):
    return (
        row.get("exists") is True
        and row.get("is_file") is not True
        and row.get("is_dir") is not True
        and (
            row.get("is_char_device") is True
            or row.get("is_fifo") is True
            or row.get("is_socket") is True
        )
    )

def unique_paths(patterns, raw_text, include_raw_parent):
    out = []
    seen = set()
    raw = Path(raw_text)
    candidates = [str(raw)]
    if include_raw_parent:
        parent = raw.parent
        for stem in ("*mission*", "*gopro*", "*raw*", "video*", "media*"):
            candidates.append(str(parent / stem))
    candidates.extend(patterns)
    for pattern in candidates:
        for value in glob.glob(pattern):
            if value not in seen:
                seen.add(value)
                out.append(value)
        if len(out) >= 200:
            break
    return out[:200]

def read_mounts():
    rows = []
    try:
        with open("/proc/mounts", "r", encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 3:
                    continue
                mount = parts[1].replace("\\040", " ")
                if mount.startswith(("/mnt", "/media", "/run/media", "/Volumes")):
                    rows.append({"device": parts[0], "mount": mount, "fstype": parts[2]})
    except OSError as exc:
        rows.append({"error": str(exc)})
    return rows[:100]

def discover(raw_text):
    source_patterns = [
        "/dev/mission1/*",
        "/dev/*mission*",
        "/dev/*gopro*",
        "/dev/*raw*",
        "/dev/video*",
        "/dev/media*",
        "/dev/v4l/by-id/*",
        "/dev/v4l/by-path/*",
        "/run/mission1/*",
        "/tmp/mission1/*",
        "/mnt/ssd/mission1/*",
    ]
    display_patterns = [
        "/dev/fb*",
        "/dev/dri/*",
        "/sys/class/graphics/*",
        "/sys/class/drm/*",
    ]
    raw_candidates = [path_info(Path(path)) for path in unique_paths(source_patterns, raw_text, True)]
    display_candidates = [path_info(Path(path)) for path in unique_paths(display_patterns, "/dev/fb0", False)]
    return {
        "raw_candidates": raw_candidates,
        "device_like_raw_candidates": [row for row in raw_candidates if device_like(row)],
        "display_candidates": display_candidates,
        "storage_mounts": read_mounts(),
    }

print(json.dumps({
    "host": os.uname().nodename,
    "machine": os.uname().machine,
    "path": path_info(Path(config["raw"])),
    "discovery": discover(config["raw"]),
}))
'''


def path_info(path: Path) -> dict[str, Any]:
    row: dict[str, Any] = {
        "path": str(path),
        "exists": path.exists(),
        "is_file": path.is_file(),
        "is_dir": path.is_dir(),
    }
    if path.exists():
        try:
            st = path.stat()
            row["size_bytes"] = st.st_size
            row["mode"] = stat.S_IMODE(st.st_mode)
            row["is_char_device"] = stat.S_ISCHR(st.st_mode)
            row["is_block_device"] = stat.S_ISBLK(st.st_mode)
            row["is_fifo"] = stat.S_ISFIFO(st.st_mode)
            row["is_socket"] = stat.S_ISSOCK(st.st_mode)
            row["resolved_path"] = str(path.resolve())
        except OSError as exc:
            row["error"] = str(exc)
        try:
            name = path.resolve().name
            if name.startswith("video"):
                sys_name = Path("/sys/class/video4linux") / name / "name"
                if sys_name.exists():
                    row["sysfs_name"] = sys_name.read_text(encoding="utf-8", errors="replace").strip()
            if name.startswith("media"):
                sys_model = Path("/sys/class/media") / name / "model"
                if sys_model.exists():
                    row["sysfs_model"] = sys_model.read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            pass
    return row


def is_device_like(row: dict[str, Any]) -> bool:
    return (
        row.get("exists") is True
        and row.get("is_file") is not True
        and row.get("is_dir") is not True
        and (
            row.get("is_char_device") is True
            or row.get("is_fifo") is True
            or row.get("is_socket") is True
        )
    )


def unique_discovery_paths(patterns: tuple[str, ...], raw: Path, *, include_raw_parent: bool) -> list[Path]:
    candidates = [str(raw)]
    if include_raw_parent:
        for stem in ("*mission*", "*gopro*", "*raw*", "video*", "media*"):
            candidates.append(str(raw.parent / stem))
    candidates.extend(patterns)
    out: list[Path] = []
    seen: set[str] = set()
    for pattern in candidates:
        for value in glob.glob(pattern):
            if value in seen:
                continue
            seen.add(value)
            out.append(Path(value))
        if len(out) >= MAX_DISCOVERY_ROWS:
            break
    return out[:MAX_DISCOVERY_ROWS]


def storage_mounts() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    try:
        with Path("/proc/mounts").open("r", encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 3:
                    continue
                mount = parts[1].replace("\\040", " ")
                if mount.startswith(("/mnt", "/media", "/run/media", "/Volumes")):
                    rows.append({"device": parts[0], "mount": mount, "fstype": parts[2]})
    except OSError as exc:
        rows.append({"error": str(exc)})
    return rows[:100]


def discovery(raw: Path) -> dict[str, Any]:
    raw_candidates = [
        path_info(path)
        for path in unique_discovery_paths(SOURCE_DISCOVERY_GLOBS, raw, include_raw_parent=True)
    ]
    display_candidates = [
        path_info(path)
        for path in unique_discovery_paths(DISPLAY_DISCOVERY_GLOBS, Path("/dev/fb0"), include_raw_parent=False)
    ]
    return {
        "raw_candidates": raw_candidates,
        "device_like_raw_candidates": [row for row in raw_candidates if is_device_like(row)],
        "display_candidates": display_candidates,
        "storage_mounts": storage_mounts(),
    }


def probe_remote(args: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    started = time.time()
    config_b64 = base64.b64encode(json.dumps({"raw": str(args.raw)}).encode("utf-8")).decode("ascii")
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
        "cmd": ["ssh", args.target_host, "python3", "-", "<mission1_camera_source_probe>"],
        "returncode": proc.returncode,
        "elapsed_s": round(time.time() - started, 3),
        "stderr_tail": proc.stderr.splitlines()[-20:],
    }
    if proc.returncode != 0:
        return {"host": args.target_host, "machine": None, "path": {"path": str(args.raw), "exists": False}}, step
    try:
        payload = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        step["returncode"] = 1
        step["stderr_tail"].append(f"invalid remote JSON: {exc}")
        return {"host": args.target_host, "machine": None, "path": {"path": str(args.raw), "exists": False}}, step
    return payload, step


def add_check(checks: list[dict[str, Any]], name: str, passed: bool, detail: str) -> None:
    checks.append({"name": name, "passed": passed, "detail": detail})


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    if args.target_host:
        probe, target_step = probe_remote(args)
    else:
        probe = {
            "host": os.uname().nodename,
            "machine": os.uname().machine,
            "path": path_info(args.raw),
            "discovery": discovery(args.raw),
        }
        target_step = None

    checks: list[dict[str, Any]] = []
    blockers: list[str] = []
    if target_step is not None:
        ok = target_step["returncode"] == 0
        add_check(checks, "ssh target probe", ok, f"returncode={target_step['returncode']}")
        if not ok:
            blockers.append("target host is not reachable over noninteractive SSH")

    raw = probe.get("path", {})
    raw_source_kind = args.raw_source_kind
    expected_raw_bytes = int(args.source_width) * int(args.source_height) * 2

    if raw_source_kind == "file_standin":
        exists = raw.get("is_file") is True
        size = raw.get("size_bytes")
        size_ok = isinstance(size, int) and size >= expected_raw_bytes
        add_check(checks, "raw source file exists", exists, raw.get("path", ""))
        add_check(checks, "raw source file has unpacked Bayer size", size_ok, f"size={size} expected_min={expected_raw_bytes}")
        if not exists:
            blockers.append("raw source file is missing")
        elif not size_ok:
            blockers.append("raw source file is smaller than one unpacked Bayer frame")
    else:
        exists = raw.get("exists") is True
        device_like = (
            exists
            and raw.get("is_file") is not True
            and raw.get("is_dir") is not True
            and (
                raw.get("is_char_device") is True
                or raw.get("is_fifo") is True
                or raw.get("is_socket") is True
            )
        )
        add_check(checks, "camera raw source endpoint exists", exists, raw.get("path", ""))
        add_check(
            checks,
            "camera raw source endpoint is device-like",
            device_like,
            "is_file={} is_dir={} is_char_device={} is_fifo={} is_socket={}".format(
                raw.get("is_file"),
                raw.get("is_dir"),
                raw.get("is_char_device"),
                raw.get("is_fifo"),
                raw.get("is_socket"),
            ),
        )
        if not exists:
            blockers.append("camera raw source endpoint is missing on target")
        elif not device_like:
            blockers.append("camera raw source endpoint is not a device-like stream")

    found_candidates = len(probe.get("discovery", {}).get("device_like_raw_candidates", []))
    add_check(checks, "camera source discovery completed", True, f"device_like_raw_candidates={found_candidates}")

    source_ready = not blockers and all(check.get("passed") is True for check in checks)
    return {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "target": {
            "host": args.target_host or "local",
            "name": args.target_name,
            "role": args.target_role,
        },
        "inputs": {
            "raw": str(args.raw),
            "raw_source_kind": raw_source_kind,
            "source_width": args.source_width,
            "source_height": args.source_height,
            "stride_bytes": args.stride_bytes,
            "bit_depth": args.bit_depth,
            "pixel_format": args.pixel_format,
        },
        "probe": probe,
        "target_step": target_step,
        "checks": checks,
        "blockers": blockers,
        "verdict": {
            "source_ready": source_ready,
            "remaining_blocker_count": len(blockers),
        },
    }


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target-host", default="")
    ap.add_argument("--ssh-timeout-s", type=int, default=5)
    ap.add_argument("--target-name", default="Mission 1")
    ap.add_argument("--target-role", choices=("stand-in", "camera"), default="camera")
    ap.add_argument("--raw", type=Path, default=Path("/dev/mission1/sensor_dma_ring"))
    ap.add_argument("--raw-source-kind", choices=("file_standin", "sensor_dma_capture", "camera_ring_buffer"), default="sensor_dma_capture")
    ap.add_argument("--source-width", type=int, default=4096)
    ap.add_argument("--source-height", type=int, default=3072)
    ap.add_argument("--stride-bytes", type=int, default=8192)
    ap.add_argument("--bit-depth", type=int, default=14)
    ap.add_argument("--pixel-format", type=int, default=1)
    ap.add_argument("--output-json", type=Path)
    ap.add_argument("--require-ready", action="store_true")
    return ap


def main() -> int:
    args = parser().parse_args()
    report = build_report(args)
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    else:
        print(json.dumps(report, indent=2))
    if args.require_ready and report["verdict"]["source_ready"] is not True:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
