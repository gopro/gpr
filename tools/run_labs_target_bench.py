#!/usr/bin/env python3
"""Run or simulate a Labs target-style `.gvid` capture bench.

On target hardware this wraps `bench_fused` with `GPR_BENCH_WRITE_ALL`, packs
the emitted `.gpr` frames into a strict v1 `.gvid`, and writes a JSON receipt
with timing, storage, memory, drop, validation, and interruption-recovery
fields.

Use `--simulate` only for CI/schema smoke tests. Simulated receipts are not
target evidence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import resource
import shutil
import statistics
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[1]
GVID_CLIP_MAGIC = 0x44495647
GVID_FRAME_MAGIC = 0x004D5246
GVID_VERSION = 1
GVID_QUALITY_MAX = 11
GVID_PIXEL_FORMAT_MAX = 5
CLIP_HEADER_SIZE = 32
FRAME_HEADER_SIZE = 16
COPY_CHUNK_SIZE = 1024 * 1024
DEFAULT_GVID_HASH_LIMIT_BYTES = 1024 * 1024 * 1024
DEFAULT_STORAGE_TARGET_NAME = "Lexar Professional SILVER PLUS SDXC/microSDXC UHS-I (128GB-1TB)"
DEFAULT_STORAGE_TARGET_READ_MBPS = 205.0
DEFAULT_STORAGE_TARGET_WRITE_MBPS = 150.0
DEFAULT_STORAGE_TARGET_SAFETY_MARGIN = 0.90
DEFAULT_STORAGE_TARGET_NOTE = (
    "Published 128GB-1TB SILVER PLUS profile is 205 MB/s read and 150 MB/s "
    "write. The 64GB microSD SKU is 205/100 and must override the write target."
)
SOURCE_PROVENANCE_EXTS = {
    ".c",
    ".cc",
    ".cpp",
    ".h",
    ".hh",
    ".hpp",
    ".m",
    ".mm",
    ".py",
}
SOURCE_PROVENANCE_ROOTS = (
    "CMakeLists.txt",
    "source",
    "tools",
)
SOURCE_PROVENANCE_SKIP_DIRS = {
    ".git",
    "__pycache__",
    ".pytest_cache",
}
RELEVANT_BENCH_ENV = [
    "GPR_INCLUDE_LL",
    "FUSED_MULTI_LEVEL",
    "FUSED_WAVELET_LEVELS",
    "GPR_COL_DECIMATE",
    "GPR_ROW_DECIMATE",
    "GPR_DECIMATE_AA",
    "GPR_AA_LUMA_ONLY",
    "GPR_DROP_HIGHPASS",
    "FUSED_STRIPE_ROWS",
    "FUSED_DEFER_RANS",
    "FUSED_INLINE_TOKENIZE",
    "FUSED_THREADS",
    "FUSED_LL2_DIVISOR",
    "FUSED_QUALITY",
    "FUSED_RAW_LL",
    "FUSED_LL_PREDICT",
    "FUSED_LL_PREDICTOR",
    "FUSED_LL_RICE_K",
    "FUSED_LL_RICE_KS",
    "FUSED_LL_RICE_FAST",
    "FUSED_LL_ASSUME_U16",
    "FUSED_L1_PRESCALE",
    "FUSED_REFERENCE_HORIZONTAL",
    "FUSED_LOG_POLYNOMIAL",
    "FUSED_PIN",
    "FUSED_PIN_P2",
    "GPR_PIN_AFFINITY",
    "FUSED_FUSE_LP_OFF",
    "FUSED_LUMA_FUSED_OFF",
    "GPR_BENCH_DENOISE",
    "GPR_BENCH_NOISE_SCALE",
    "GPR_BENCH_NOISE_OFFSET",
    "GPR_BENCH_PIXEL_FORMAT",
    "GPR_BENCH_ASYNC_GVID",
    "GPR_BENCH_ASYNC_QUEUE",
    "GPR_BENCH_GVID_FPS",
    "GPR_BENCH_GVID_WRITEV",
    "GPR_BENCH_GVID_SCATTER",
    "GPR_BENCH_GVID_COALESCE_PREFIX",
    "GPR_BENCH_GVID_PINGPONG",
    "GPR_BENCH_GVID_WRITER_CORE",
    "GPR_BENCH_GVID_SYNC_RANGE",
    "GPR_INLINE_DENOISE_T",
    "GPR_INLINE_DENOISE_T_LH",
    "GPR_INLINE_DENOISE_T_HL",
    "GPR_INLINE_DENOISE_T_HH",
    "GPR_INLINE_DENOISE_T_CH0_LH",
    "GPR_INLINE_DENOISE_T_CH0_HL",
    "GPR_INLINE_DENOISE_T_CH0_HH",
    "GPR_INLINE_DENOISE_T_CH1_LH",
    "GPR_INLINE_DENOISE_T_CH1_HL",
    "GPR_INLINE_DENOISE_T_CH1_HH",
    "GPR_INLINE_DENOISE_T_CH2_LH",
    "GPR_INLINE_DENOISE_T_CH2_HL",
    "GPR_INLINE_DENOISE_T_CH2_HH",
    "GPR_INLINE_DENOISE_T_CH3_LH",
    "GPR_INLINE_DENOISE_T_CH3_HL",
    "GPR_INLINE_DENOISE_T_CH3_HH",
    "GPR_INLINE_DENOISE_MODE",
    "GPR_INLINE_DENOISE_HARD",
    "JANS_INLINE_PROFILE",
]
FUSED_STAGE_RE = re.compile(
    r"^\s*FUSED\s+(?P<label>.+?):\s+(?P<ms>[0-9]+(?:\.[0-9]+)?)ms(?:\s+\((?P<note>[^)]*)\))?\s*$"
)
FUSED_CHANNEL_RE = re.compile(r"^\s*ch(?P<channel>[0-9]+):\s+(?P<body>.+)$")
FUSED_KV_MS_RE = re.compile(r"(?P<key>[A-Za-z0-9_+.-]+)=(?P<ms>-?[0-9]+(?:\.[0-9]+)?)")
FUSED_PRODUCER_RE = re.compile(
    r"^\s*producer-unpack\[(?P<range>[0-9.]+)\]:\s+(?P<ms>[0-9]+(?:\.[0-9]+)?)ms"
)
FUSED_PRED_LL_RE = re.compile(
    r"^\s*FUSED\s+pred_ll\s+ch(?P<channel>[0-9]+):\s+(?P<ms>[0-9]+(?:\.[0-9]+)?)ms\s+size=(?P<size>-?[0-9]+)"
)
JANS_INLINE_PROFILE_RE = re.compile(r"^#\s+jans_inline_profile\s+(?P<body>.+)$")
BENCH_PHASE_RE = re.compile(r"^#\s+bench_phase_ms\s+(?P<name>[A-Za-z0-9_+.-]+)\s+(?P<body>.+)$")
BENCH_PHASE_KV_RE = re.compile(r"(?P<key>[A-Za-z0-9_+.-]+)=(?P<value>-?[0-9]+(?:\.[0-9]+)?)")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def gvid_sha256_receipt(path: Path) -> tuple[str | None, str]:
    """Hash small retained clips; avoid re-reading huge sustained payloads.

    Long target receipts validate the container structure and timing, then the
    wrapper deletes the payload. Hashing 100+ GB after validation can dominate
    the harness wall time without improving the retained evidence. Set
    GPR_TARGET_BENCH_HASH_GVID=1 to force a full hash when the payload itself
    is going to be preserved.
    """
    force = os.environ.get("GPR_TARGET_BENCH_HASH_GVID") == "1"
    skip = os.environ.get("GPR_TARGET_BENCH_HASH_GVID") == "0"
    limit = int(os.environ.get("GPR_TARGET_BENCH_HASH_LIMIT_BYTES", str(DEFAULT_GVID_HASH_LIMIT_BYTES)))
    size = path.stat().st_size
    if skip:
        return None, "skipped_by_GPR_TARGET_BENCH_HASH_GVID=0"
    if force or size <= limit:
        return sha256_file(path), "full_sha256"
    return None, f"skipped_large_gvid_gt_{limit}_bytes"


def git_commit() -> str | None:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return None


def git_text(root: Path, args: list[str]) -> str | None:
    try:
        return subprocess.check_output(
            ["git", *args],
            cwd=root,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return None


def source_provenance_file_allowed(path: Path) -> bool:
    return path.name == "CMakeLists.txt" or path.suffix.lower() in SOURCE_PROVENANCE_EXTS


def iter_source_provenance_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for rel in SOURCE_PROVENANCE_ROOTS:
        candidate = root / rel
        if candidate.is_file() and source_provenance_file_allowed(candidate):
            files.append(candidate)
            continue
        if not candidate.is_dir():
            continue
        for current, dirs, names in os.walk(candidate):
            dirs[:] = sorted(d for d in dirs if d not in SOURCE_PROVENANCE_SKIP_DIRS)
            current_path = Path(current)
            for name in sorted(names):
                path = current_path / name
                if path.is_file() and source_provenance_file_allowed(path):
                    files.append(path)
    return sorted(files, key=lambda p: p.relative_to(root).as_posix())


def source_tree_provenance(root: Path) -> dict[str, Any]:
    root = root.resolve()
    if not root.exists():
        return {
            "available": False,
            "policy": "source_tree_digest_v1",
            "root": str(root),
            "reason": "source_provenance_root_missing",
        }

    files = iter_source_provenance_files(root)
    if not files:
        return {
            "available": False,
            "policy": "source_tree_digest_v1",
            "root": str(root),
            "reason": "no_source_files_found",
        }

    h = hashlib.sha256()
    total_bytes = 0
    for path in files:
        rel = path.relative_to(root).as_posix()
        size = path.stat().st_size
        total_bytes += size
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        h.update(str(size).encode("ascii"))
        h.update(b"\0")
        with path.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        h.update(b"\0")

    git_root = git_text(root, ["rev-parse", "--show-toplevel"])
    git_status = git_text(root, ["status", "--short"])
    git_head = git_text(root, ["rev-parse", "HEAD"])
    return {
        "available": True,
        "policy": "source_tree_digest_v1",
        "root": str(root),
        "sha256": h.hexdigest(),
        "file_count": len(files),
        "total_bytes": total_bytes,
        "included_roots": list(SOURCE_PROVENANCE_ROOTS),
        "git": {
            "available": git_root is not None,
            "root": git_root,
            "head": git_head,
            "dirty": bool(git_status),
            "status_short": git_status.splitlines()[:200] if git_status else [],
        },
    }


def relevant_env(env: dict[str, str] | None = None) -> dict[str, str]:
    source = os.environ if env is None else env
    return {key: source[key] for key in RELEVANT_BENCH_ENV if key in source}


def find_cmake_build_root(binary: Path | None) -> Path | None:
    if not binary:
        return None
    for parent in [binary.parent, *binary.parents]:
        if (parent / "CMakeCache.txt").is_file():
            return parent
    return None


def first_matching_line(path: Path, prefix: str) -> str | None:
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith(prefix):
                    return line.strip()
    except OSError:
        return None
    return None


def bench_build_info(binary: Path | None) -> dict[str, Any]:
    if not binary:
        return {}
    info: dict[str, Any] = {
        "binary": str(binary),
        "binary_sha256": sha256_file(binary) if binary.is_file() else None,
    }
    build_root = find_cmake_build_root(binary)
    if not build_root:
        return info
    info["cmake_build_root"] = str(build_root)
    cache = build_root / "CMakeCache.txt"
    build_type = first_matching_line(cache, "CMAKE_BUILD_TYPE:")
    if build_type:
        info["cmake_build_type"] = build_type.split("=", 1)[-1]
    for label, rel in {
        "encoder_c_flags": "source/lib/vc5_encoder/CMakeFiles/vc5_encoder.dir/flags.make",
        "bench_c_flags": "source/app/bench_fused/CMakeFiles/bench_fused.dir/flags.make",
    }.items():
        flags = first_matching_line(build_root / rel, "C_FLAGS =")
        if flags:
            info[label] = flags.split("=", 1)[-1].strip()
    return info


def maxrss_kb() -> int:
    value = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    if sys.platform == "darwin":
        return value // 1024
    return value


def children_maxrss_kb() -> int:
    value = int(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    if sys.platform == "darwin":
        return value // 1024
    return value


def loadavg() -> list[float] | None:
    if not hasattr(os, "getloadavg"):
        return None
    try:
        return [float(v) for v in os.getloadavg()]
    except OSError:
        return None


def inferred_target_evidence() -> bool:
    system = platform.system().lower()
    machine = platform.machine().lower()
    return system == "linux" and machine in {"aarch64", "arm64", "armv7l", "armv8l"}


def percentile(sorted_values: list[float], frac: float) -> float:
    if not sorted_values:
        return 0.0
    idx = min(len(sorted_values) - 1, max(0, round((len(sorted_values) - 1) * frac)))
    return float(sorted_values[idx])


def summarize_ms(values: list[float]) -> dict[str, float | int]:
    vals = sorted(float(v) for v in values)
    if not vals:
        return {"n": 0}
    mean = statistics.mean(vals)
    median = statistics.median(vals)
    return {
        "n": len(vals),
        "mean_ms": mean,
        "median_ms": median,
        "min_ms": vals[0],
        "p25_ms": percentile(vals, 0.25),
        "p75_ms": percentile(vals, 0.75),
        "p95_ms": percentile(vals, 0.95),
        "p99_ms": percentile(vals, 0.99),
        "max_ms": vals[-1],
        "fps_mean": 1000.0 / mean if mean > 0 else 0.0,
        "fps_median": 1000.0 / median if median > 0 else 0.0,
    }


def normalize_timing_key(text: str) -> str:
    out = re.sub(r"\([^)]*\)", "", text.strip()).lower()
    out = out.replace("+", "_")
    out = re.sub(r"[^a-z0-9]+", "_", out)
    out = out.strip("_")
    return out or "unknown"


def append_ms(bucket: dict[str, list[float]], key: str, value: float) -> None:
    bucket.setdefault(key, []).append(float(value))


def summarize_ms_map(values: dict[str, list[float]]) -> dict[str, dict[str, float | int]]:
    return {key: summarize_ms(vals) for key, vals in sorted(values.items())}


def summarize_number_values(values: list[float]) -> dict[str, float | int]:
    vals = sorted(float(v) for v in values)
    if not vals:
        return {"n": 0}
    return {
        "n": len(vals),
        "mean": statistics.mean(vals),
        "median": statistics.median(vals),
        "min": vals[0],
        "p25": percentile(vals, 0.25),
        "p75": percentile(vals, 0.75),
        "p95": percentile(vals, 0.95),
        "p99": percentile(vals, 0.99),
        "max": vals[-1],
    }


def summarize_number_map(values: dict[str, list[float]]) -> dict[str, dict[str, float | int]]:
    return {key: summarize_number_values(vals) for key, vals in sorted(values.items())}


def dominant_mean_component(summaries: dict[str, dict[str, float | int]]) -> str | None:
    best_key: str | None = None
    best_mean = float("-inf")
    for key, summary in summaries.items():
        mean = summary.get("mean_ms")
        if isinstance(mean, (int, float)) and float(mean) > best_mean:
            best_key = key
            best_mean = float(mean)
    return best_key


def parse_fused_timing_stderr(stderr: str | None) -> dict[str, Any]:
    """Summarize FUSED_TIMING / FUSED_TIMING_DETAIL stderr into receipt JSON."""
    if not stderr:
        return {"available": False, "timing_line_count": 0}

    stage_ms: dict[str, list[float]] = {}
    channel_ms: dict[str, list[float]] = {}
    channel_by_id: dict[str, dict[str, list[float]]] = {}
    producer_ms: dict[str, list[float]] = {}
    pred_ll_ms: dict[str, list[float]] = {}
    pred_ll_size: dict[str, list[float]] = {}
    timing_line_count = 0

    for raw_line in stderr.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        stage = FUSED_STAGE_RE.match(line)
        if stage:
            timing_line_count += 1
            key = normalize_timing_key(stage.group("label"))
            append_ms(stage_ms, key, float(stage.group("ms")))
            continue

        channel = FUSED_CHANNEL_RE.match(line)
        if channel:
            parsed_any = False
            channel_id = channel.group("channel")
            per_channel = channel_by_id.setdefault(channel_id, {})
            for item in FUSED_KV_MS_RE.finditer(channel.group("body")):
                parsed_any = True
                key = normalize_timing_key(item.group("key"))
                value = float(item.group("ms"))
                append_ms(channel_ms, key, value)
                append_ms(per_channel, key, value)
            if parsed_any:
                timing_line_count += 1
            continue

        producer = FUSED_PRODUCER_RE.match(line)
        if producer:
            timing_line_count += 1
            append_ms(producer_ms, f"producer_unpack_{producer.group('range')}", float(producer.group("ms")))
            continue

        pred_ll = FUSED_PRED_LL_RE.match(line)
        if pred_ll:
            timing_line_count += 1
            ch = pred_ll.group("channel")
            append_ms(pred_ll_ms, ch, float(pred_ll.group("ms")))
            append_ms(pred_ll_size, ch, float(pred_ll.group("size")))

    stage_summary = summarize_ms_map(stage_ms)
    channel_summary = summarize_ms_map(channel_ms)
    producer_summary = summarize_ms_map(producer_ms)
    pred_ll_summary = summarize_ms_map(pred_ll_ms)
    pred_ll_size_summary = summarize_ms_map(pred_ll_size)
    by_channel_summary = {
        channel: summarize_ms_map(values)
        for channel, values in sorted(channel_by_id.items(), key=lambda item: int(item[0]))
    }
    return {
        "available": bool(stage_summary or channel_summary or producer_summary or pred_ll_summary),
        "timing_line_count": timing_line_count,
        "stage_ms": stage_summary,
        "channel_component_ms": channel_summary,
        "channel_component_by_channel_ms": by_channel_summary,
        "producer_ms": producer_summary,
        "pred_ll_ms": pred_ll_summary,
        "pred_ll_size_bytes": pred_ll_size_summary,
        "dominant_stage_by_mean_ms": dominant_mean_component(stage_summary),
        "dominant_channel_component_by_mean_ms": dominant_mean_component(channel_summary),
    }


def parse_bench_phase_timing_stderr(stderr: str | None) -> dict[str, Any]:
    """Parse production-safe bench_fused phase summaries.

    These lines are emitted by bench_fused itself and do not require compiling
    FUSED_TIMING/FUSED_TIMING_DETAIL into the encoder hot loop.
    """
    if not stderr:
        return {"available": False, "timing_line_count": 0}

    phases: dict[str, dict[str, float | int]] = {}
    timing_line_count = 0
    for raw_line in stderr.splitlines():
        line = raw_line.strip()
        m = BENCH_PHASE_RE.match(line)
        if not m:
            continue
        timing_line_count += 1
        name = normalize_timing_key(m.group("name"))
        values: dict[str, float | int] = {}
        for item in BENCH_PHASE_KV_RE.finditer(m.group("body")):
            key = normalize_timing_key(item.group("key"))
            if name != "payload_kib" and key in {"mean", "stddev", "min", "p25", "median", "p75", "p95", "p99", "max"}:
                key = f"{key}_ms"
            value = float(item.group("value"))
            values[key] = int(value) if key == "n" else value
        phases[name] = values

    timing_components = {
        key: value
        for key, value in phases.items()
        if key not in {"total", "payload_kib", "async_drain", "pingpong_drain"}
    }
    return {
        "available": bool(phases),
        "timing_line_count": timing_line_count,
        "phase_ms": phases,
        "dominant_phase_by_mean_ms": dominant_mean_component(timing_components),
    }


def writer_handoff_receipt(
    bench_phase_timing: dict[str, Any],
    frames_written: int,
    wall_s: float,
    target_fps: float,
) -> dict[str, Any]:
    """Summarize deferred writer work so async modes cannot hide drain time."""
    phases = bench_phase_timing.get("phase_ms") if isinstance(bench_phase_timing, dict) else {}
    if not isinstance(phases, dict):
        phases = {}
    drain_rows = {
        name: row
        for name, row in phases.items()
        if name in {"async_drain", "pingpong_drain"} and isinstance(row, dict)
    }
    drain_ms = 0.0
    for row in drain_rows.values():
        try:
            drain_ms += float(row.get("mean_ms", row.get("median_ms", 0.0)))
        except (TypeError, ValueError):
            pass
    loop_total = phases.get("total") if isinstance(phases.get("total"), dict) else {}
    try:
        loop_median_ms = float(loop_total.get("median_ms"))
    except (TypeError, ValueError):
        loop_median_ms = None
    loop_fps = 1000.0 / loop_median_ms if loop_median_ms and loop_median_ms > 0.0 else None
    wall_fps = (float(frames_written) / wall_s) if wall_s > 0.0 else 0.0
    wall_ms_per_frame = (wall_s * 1000.0 / float(frames_written)) if frames_written > 0 and wall_s > 0.0 else None
    target_frame_ms = 1000.0 / target_fps if target_fps > 0.0 else None
    loop_gap_ms = (
        loop_median_ms - target_frame_ms
        if loop_median_ms is not None and target_frame_ms is not None
        else None
    )
    wall_gap_ms = (
        wall_ms_per_frame - target_frame_ms
        if wall_ms_per_frame is not None and target_frame_ms is not None
        else None
    )
    bottleneck_gap_ms = None
    if loop_gap_ms is not None and wall_gap_ms is not None:
        bottleneck_gap_ms = max(loop_gap_ms, wall_gap_ms)
    elif loop_gap_ms is not None:
        bottleneck_gap_ms = loop_gap_ms
    elif wall_gap_ms is not None:
        bottleneck_gap_ms = wall_gap_ms
    return {
        "wall_includes_writer_drain": True,
        "deferred_writer_phase_names": sorted(drain_rows),
        "deferred_writer_drain_ms": drain_ms,
        "deferred_writer_work_present": drain_ms > 0.0,
        "loop_fps_median": loop_fps,
        "loop_median_ms": loop_median_ms,
        "wall_fps": wall_fps,
        "wall_ms_per_frame": wall_ms_per_frame,
        "target_fps": target_fps,
        "target_frame_ms": target_frame_ms,
        "loop_target_gap_ms": loop_gap_ms,
        "wall_target_gap_ms": wall_gap_ms,
        "bottleneck_target_gap_ms": bottleneck_gap_ms,
        "target_policy": "production target requires both loop median fps and whole-run wall fps",
        "fps_target_met": bool(loop_fps is not None and loop_fps >= target_fps and wall_fps >= target_fps),
    }


def parse_jans_inline_profile_stderr(stderr: str | None) -> dict[str, Any]:
    """Parse optional JANS_INLINE_PROFILE_RUNTIME stderr into receipt JSON."""
    if not stderr:
        return {"available": False, "profile_line_count": 0}

    by_label: dict[str, dict[str, list[float]]] = {}
    line_count = 0
    for raw_line in stderr.splitlines():
        line = raw_line.strip()
        m = JANS_INLINE_PROFILE_RE.match(line)
        if not m:
            continue
        fields: dict[str, str] = {}
        for item in m.group("body").split():
            if "=" not in item:
                continue
            key, value = item.split("=", 1)
            fields[key] = value
        label = fields.get("label", "unlabeled")
        bucket = by_label.setdefault(label, {})
        for key, value in fields.items():
            if key == "label":
                continue
            try:
                append_ms(bucket, normalize_timing_key(key), float(value))
            except ValueError:
                continue
        line_count += 1

    summary = {
        label: summarize_number_map(values)
        for label, values in sorted(by_label.items())
    }
    return {
        "available": bool(summary),
        "profile_line_count": line_count,
        "by_label": summary,
    }


def storage_target_receipt(total_frame_bytes: int, frames_written: int, target_fps: float, args: argparse.Namespace) -> dict[str, Any]:
    bytes_per_frame = (float(total_frame_bytes) / float(frames_written)) if frames_written > 0 else 0.0
    required_write_Bps = bytes_per_frame * float(target_fps)
    required_write_MBps = required_write_Bps / 1_000_000.0
    required_write_MiBps = required_write_Bps / (1024.0 * 1024.0)
    target_write_MBps = float(args.storage_target_write_mbps)
    target_read_MBps = float(args.storage_target_read_mbps)
    safety_margin = float(args.storage_target_safety_margin)
    budget_write_MBps = target_write_MBps * safety_margin
    budget_read_MBps = target_read_MBps * safety_margin
    budget_bytes_per_frame = (budget_write_MBps * 1_000_000.0 / float(target_fps)) if target_fps > 0 else 0.0
    return {
        "name": args.storage_target_name,
        "target_read_MBps": target_read_MBps,
        "target_write_MBps": target_write_MBps,
        "profile_note": getattr(args, "storage_target_note", DEFAULT_STORAGE_TARGET_NOTE),
        "safety_margin": safety_margin,
        "budget_read_MBps": budget_read_MBps,
        "budget_write_MBps": budget_write_MBps,
        "target_fps": float(target_fps),
        "bytes_per_frame": bytes_per_frame,
        "MiB_per_frame": bytes_per_frame / (1024.0 * 1024.0),
        "budget_bytes_per_frame": budget_bytes_per_frame,
        "budget_MiB_per_frame": budget_bytes_per_frame / (1024.0 * 1024.0),
        "required_write_MBps": required_write_MBps,
        "required_write_MiBps": required_write_MiBps,
        "fits_target": required_write_MBps <= budget_write_MBps,
        "policy": "payload_size_at_target_fps_vs_sustained_card_write_budget",
    }


def read_temp_c() -> float | None:
    try:
        out = subprocess.check_output(["vcgencmd", "measure_temp"], text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return None
    marker = "temp="
    if marker not in out:
        return None
    try:
        return float(out.split(marker, 1)[1].split("'C", 1)[0])
    except Exception:
        return None


def run_cmd(cmd: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)


def bench_times_from_stdout(stdout: str) -> list[float]:
    vals: list[float] = []
    for line in stdout.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            vals.append(float(line))
        except ValueError:
            continue
    return vals


def write_gvid(frame_paths: list[Path], out: Path, width: int, height: int, fps: float, quality: int, pixel_format: int) -> None:
    if width <= 0 or height <= 0:
        raise ValueError("bad .gvid dimensions")
    if fps <= 0.0:
        raise ValueError("bad .gvid fps")
    if not (0 <= pixel_format <= GVID_PIXEL_FORMAT_MAX):
        raise ValueError(f"bad .gvid pixel_format {pixel_format}; expected 0..{GVID_PIXEL_FORMAT_MAX}")
    if not (0 <= quality <= GVID_QUALITY_MAX):
        raise ValueError(f"bad .gvid quality {quality}; expected 0..{GVID_QUALITY_MAX}")
    fps_x1000 = int(round(fps * 1000.0))
    with out.open("wb") as f:
        f.write(struct.pack(
            "<IBBHHHIIIII",
            GVID_CLIP_MAGIC,
            GVID_VERSION,
            0,
            pixel_format,
            quality,
            0,
            width,
            height,
            fps_x1000,
            0,
            len(frame_paths),
        ))
        for tag, path in enumerate(frame_paths):
            payload = path.read_bytes()
            if not payload:
                raise ValueError(f"empty frame payload: {path}")
            f.write(struct.pack("<IIQ", GVID_FRAME_MAGIC, len(payload), tag))
            f.write(payload)


def validate_gvid(path: Path) -> dict[str, Any]:
    file_size = path.stat().st_size
    if file_size < CLIP_HEADER_SIZE:
        raise ValueError("too small for clip header")
    with path.open("rb") as f:
        header = f.read(CLIP_HEADER_SIZE)
        if len(header) != CLIP_HEADER_SIZE:
            raise ValueError("too small for clip header")
        clip = struct.unpack("<IBBHHHIIIII", header)
        magic, version, flags, pixel_format, quality, reserved2 = clip[:6]
        width, height, fps_x1000, target_kbps, frame_count_hint = clip[6:]
        if magic != GVID_CLIP_MAGIC or version != GVID_VERSION:
            raise ValueError("bad .gvid magic/version")
        if flags & ~0x03 or pixel_format > GVID_PIXEL_FORMAT_MAX or quality > GVID_QUALITY_MAX or reserved2:
            raise ValueError("bad .gvid clip fields")
        if width == 0 or height == 0 or fps_x1000 == 0:
            raise ValueError("bad .gvid dimensions/fps")
        if bool(flags & 0x01) != bool(target_kbps):
            raise ValueError("rate-control flag/target mismatch")
        pos = CLIP_HEADER_SIZE
        frame_count = 0
        last_tag: int | None = None
        payload_bytes = 0
        while pos < file_size:
            frame_header = f.read(FRAME_HEADER_SIZE)
            if len(frame_header) != FRAME_HEADER_SIZE:
                raise ValueError("truncated frame header")
            frame_magic, payload_size, tag = struct.unpack("<IIQ", frame_header)
            pos += FRAME_HEADER_SIZE
            if frame_magic != GVID_FRAME_MAGIC or payload_size == 0:
                raise ValueError("bad frame header")
            if payload_size > file_size - pos:
                raise ValueError("truncated payload")
            if last_tag is not None and tag <= last_tag:
                raise ValueError("non-monotonic frame tag")
            last_tag = tag
            frame_count += 1
            payload_bytes += payload_size
            f.seek(payload_size, os.SEEK_CUR)
            pos += payload_size
    if frame_count_hint and frame_count_hint != frame_count:
        raise ValueError("frame_count_hint mismatch")
    return {
        "valid": True,
        "width": width,
        "height": height,
        "fps_x1000": fps_x1000,
        "frame_count": frame_count,
        "payload_bytes": payload_bytes,
    }


def complete_frame_count(path: Path) -> int:
    file_size = path.stat().st_size
    if file_size < CLIP_HEADER_SIZE:
        return 0
    with path.open("rb") as f:
        if len(f.read(CLIP_HEADER_SIZE)) != CLIP_HEADER_SIZE:
            return 0
        pos = CLIP_HEADER_SIZE
        count = 0
        last_tag: int | None = None
        while pos < file_size:
            frame_header = f.read(FRAME_HEADER_SIZE)
            if len(frame_header) != FRAME_HEADER_SIZE:
                return count
            frame_magic, payload_size, tag = struct.unpack("<IIQ", frame_header)
            pos += FRAME_HEADER_SIZE
            if frame_magic != GVID_FRAME_MAGIC or payload_size == 0:
                return count
            if payload_size > file_size - pos:
                return count
            if last_tag is not None and tag <= last_tag:
                return count
            last_tag = tag
            count += 1
            f.seek(payload_size, os.SEEK_CUR)
            pos += payload_size
    return count


def write_sparse_gvid_without_last_byte(src: Path, dst: Path) -> None:
    """Create a one-byte-truncated .gvid test file without copying payloads.

    The validator and recovery counter read clip/frame headers and seek over
    payloads. For a tail-interruption proof, a sparse file with the same header
    layout exercises the same parser behavior while avoiding a full-size copy
    on long sustained receipts.
    """
    src_size = src.stat().st_size
    target_size = src_size - 1
    if target_size <= 0:
        dst.write_bytes(b"")
        return

    with src.open("rb") as inp, dst.open("wb") as out:
        clip_header = inp.read(CLIP_HEADER_SIZE)
        if len(clip_header) != CLIP_HEADER_SIZE:
            out.write(clip_header[:target_size])
            out.truncate(target_size)
            return

        out.write(clip_header)
        pos = CLIP_HEADER_SIZE
        while pos < src_size:
            inp.seek(pos)
            frame_header = inp.read(FRAME_HEADER_SIZE)
            if not frame_header:
                break
            writable = max(0, min(len(frame_header), target_size - pos))
            if writable:
                out.seek(pos)
                out.write(frame_header[:writable])
            if len(frame_header) != FRAME_HEADER_SIZE:
                break
            frame_magic, payload_size, _tag = struct.unpack("<IIQ", frame_header)
            if frame_magic != GVID_FRAME_MAGIC or payload_size == 0:
                break
            pos += FRAME_HEADER_SIZE + payload_size
        out.truncate(target_size)


def synth_frames(frame_dir: Path, frame_count: int) -> list[float]:
    frame_dir.mkdir(parents=True, exist_ok=True)
    times: list[float] = []
    for idx in range(frame_count):
        payload = (f"synthetic-frame-{idx:06d}\n".encode("ascii") * (idx % 7 + 1))
        t0 = time.perf_counter()
        (frame_dir / f"frame_{idx:04d}.gpr").write_bytes(payload)
        times.append((time.perf_counter() - t0) * 1000.0 + 1.0)
    return times


def run_bench(
    args: argparse.Namespace,
    frame_dir: Path,
    direct_gvid: Path,
) -> tuple[list[float], subprocess.CompletedProcess[str] | None, dict[str, str]]:
    if args.simulate:
        return synth_frames(frame_dir, args.frames), None, relevant_env()
    if not args.bench or not args.bench.is_file():
        raise RuntimeError("--bench is required and must point to bench_fused unless --simulate is set")
    if not args.raw or not (args.raw.is_file() or args.raw.is_dir()):
        raise RuntimeError("--raw is required and must point to a source Bayer raw file or directory unless --simulate is set")
    if (
        args.direct_gvid
        and args.wavelet_levels != 1
        and os.environ.get("GPR_BENCH_GVID_SCATTER") == "1"
    ):
        raise RuntimeError(
            "GPR_BENCH_GVID_SCATTER direct .gvid output is only supported "
            "with --wavelet-levels 1; multilevel scatter encodes no payload"
        )
    env = os.environ.copy()
    env.update({
        "GPR_INCLUDE_LL": "1",
        "FUSED_MULTI_LEVEL": "0" if args.wavelet_levels == 1 else "1",
        "FUSED_WAVELET_LEVELS": str(args.wavelet_levels),
        "FUSED_QUALITY": str(args.quality),
        "GPR_BENCH_PIXEL_FORMAT": str(args.pixel_format),
    })
    if args.no_decimate:
        env.pop("GPR_COL_DECIMATE", None)
        env.pop("GPR_ROW_DECIMATE", None)
    else:
        env["GPR_COL_DECIMATE"] = str(args.col_decimate)
        env["GPR_ROW_DECIMATE"] = str(args.row_decimate)
    if args.direct_gvid:
        env["GPR_BENCH_GVID"] = str(direct_gvid)
        env["GPR_BENCH_GVID_FPS"] = f"{args.target_fps:.6f}"
    else:
        env["GPR_BENCH_WRITE_ALL"] = str(frame_dir)
    cmd = [str(args.bench), str(args.raw), str(args.source_width), str(args.source_height), str(args.frames)]
    result = run_cmd(cmd, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"bench failed: {result.stderr[-2000:]}")
    return bench_times_from_stdout(result.stdout), result, relevant_env(env)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bench", type=Path, help="bench_fused binary on target")
    ap.add_argument("--raw", type=Path, help="source Bayer raw input on target")
    ap.add_argument("--output-dir", type=Path, default=Path(os.environ.get("GPR_ARTIFACT_ROOT", Path(os.environ.get("GPR_EXTERNAL_ROOT", Path(__file__).resolve().parents[1])) / "artifacts")) / "labs_target_bench")
    ap.add_argument("--frames", type=int, default=14400, help="frames to run; 14400 = 10 minutes at 24 fps")
    ap.add_argument("--target-fps", type=float, default=24.0)
    ap.add_argument("--source-width", type=int, default=8280)
    ap.add_argument("--source-height", type=int, default=5520)
    ap.add_argument("--capture-width", type=int, default=8280)
    ap.add_argument("--capture-height", type=int, default=5520)
    ap.add_argument("--quality", type=int, default=3)
    ap.add_argument("--wavelet-levels", type=int, default=2, choices=(1, 2))
    ap.add_argument("--col-decimate", type=int, default=2)
    ap.add_argument("--row-decimate", type=int, default=2)
    ap.add_argument("--no-decimate", action="store_true", help="omit GPR_COL_DECIMATE/GPR_ROW_DECIMATE for native-resolution sources")
    ap.add_argument("--pixel-format", type=int, default=4)
    ap.add_argument("--direct-gvid", action="store_true", help="measure bench_fused sequential .gvid output instead of staging per-frame .gpr files")
    ap.add_argument("--simulate", action="store_true", help="write a tiny synthetic receipt for CI/schema tests")
    ap.add_argument("--target-evidence", action="store_true", help="force target_evidence=true for an explicitly selected lab target host")
    ap.add_argument("--storage-target-name", default=DEFAULT_STORAGE_TARGET_NAME)
    ap.add_argument("--storage-target-read-mbps", type=float, default=DEFAULT_STORAGE_TARGET_READ_MBPS, help="decimal MB/s advertised/sustained read target recorded for playback/decode evidence")
    ap.add_argument("--storage-target-write-mbps", type=float, default=DEFAULT_STORAGE_TARGET_WRITE_MBPS, help="decimal MB/s sustained write target for payload-size gate")
    ap.add_argument("--storage-target-safety-margin", type=float, default=DEFAULT_STORAGE_TARGET_SAFETY_MARGIN, help="fraction of target write bandwidth allowed for encoded payload")
    ap.add_argument("--storage-target-note", default=DEFAULT_STORAGE_TARGET_NOTE, help="human-readable card profile/capacity note copied into the receipt")
    ap.add_argument(
        "--source-provenance-root",
        type=Path,
        default=Path(os.environ.get("GPR_SOURCE_PROVENANCE_ROOT", str(REPO))),
        help="source snapshot root to digest into the target receipt",
    )
    args = ap.parse_args()
    if args.no_decimate:
        args.capture_width = args.source_width
        args.capture_height = args.source_height

    args.output_dir.mkdir(parents=True, exist_ok=True)
    frame_dir = args.output_dir / "frames"
    if frame_dir.exists():
        shutil.rmtree(frame_dir)
    frame_dir.mkdir(parents=True)
    direct_gvid = args.output_dir / "capture.gvid"
    if direct_gvid.exists():
        direct_gvid.unlink()

    temp_start = read_temp_c()
    load_start = loadavg()
    wall_start = time.time()
    times_ms, bench_result, bench_env_overrides = run_bench(args, frame_dir, direct_gvid)
    wall_s = time.time() - wall_start
    temp_end = read_temp_c()
    load_end = loadavg()

    if args.direct_gvid and not args.simulate:
        gvid = direct_gvid
        gvid_info = validate_gvid(gvid)
        frames_written = int(gvid_info.get("frame_count", 0))
        missing = [f"frame_{idx:04d}.gpr" for idx in range(frames_written, args.frames)]
        total_frame_bytes = int(gvid_info.get("payload_bytes", 0))
    else:
        frame_paths = sorted(frame_dir.glob("frame_*.gpr"))
        expected_tags = {f"frame_{idx:04d}.gpr" for idx in range(args.frames)}
        actual_tags = {path.name for path in frame_paths}
        missing = sorted(expected_tags - actual_tags)
        total_frame_bytes = sum(path.stat().st_size for path in frame_paths)

        gvid = direct_gvid
        write_gvid(frame_paths, gvid, args.capture_width, args.capture_height, args.target_fps, args.quality, args.pixel_format)
        gvid_info = validate_gvid(gvid)

    truncated = args.output_dir / "capture_interrupted_tail.gvid"
    write_sparse_gvid_without_last_byte(gvid, truncated)
    interruption = {
        "truncated_path": str(truncated),
        "validator_rejects_truncated": False,
        "complete_frames_recovered": complete_frame_count(truncated),
    }
    try:
        validate_gvid(truncated)
    except Exception as exc:
        interruption["validator_rejects_truncated"] = True
        interruption["reject_reason"] = str(exc)

    timing = summarize_ms(times_ms)
    fused_timing = parse_fused_timing_stderr(bench_result.stderr if bench_result else None)
    bench_phase_timing = parse_bench_phase_timing_stderr(bench_result.stderr if bench_result else None)
    jans_inline_profile = parse_jans_inline_profile_stderr(bench_result.stderr if bench_result else None)
    gvid_sha256, gvid_sha256_policy = gvid_sha256_receipt(gvid)
    frames_written = int(gvid_info.get("frame_count", 0)) if args.direct_gvid and not args.simulate else args.frames - len(missing)
    writer_handoff = writer_handoff_receipt(bench_phase_timing, frames_written, wall_s, args.target_fps)
    storage_target = storage_target_receipt(total_frame_bytes, frames_written, args.target_fps, args)
    target_evidence = bool((not args.simulate) and (args.target_evidence or inferred_target_evidence()))
    wall_fps = (float(frames_written) / wall_s) if wall_s > 0 else 0.0
    median_fps_target_met = float(timing.get("fps_median", 0.0)) >= args.target_fps
    wall_fps_target_met = wall_fps >= args.target_fps
    source_provenance = source_tree_provenance(args.source_provenance_root)
    receipt = {
        "schema": "gpr_labs_target_bench.v1",
        "simulated": bool(args.simulate),
        "repo_commit": git_commit(),
        "source_provenance": source_provenance,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": {
            "node": platform.node(),
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "cpu_count": os.cpu_count(),
        },
        "target": {
            "name": "Pi 5 / Mission 1 stand-in" if target_evidence else ("synthetic smoke" if args.simulate else "local non-target bench host"),
            "fps": args.target_fps,
            "duration_target_s": args.frames / args.target_fps if args.target_fps else 0,
            "actual_wall_s": wall_s,
            "actual_wall_fps": wall_fps,
            "target_evidence_forced": bool(args.target_evidence),
        },
        "capture": {
            "source_width": args.source_width,
            "source_height": args.source_height,
            "capture_width": args.capture_width,
            "capture_height": args.capture_height,
            "quality": args.quality,
            "pixel_format": args.pixel_format,
            "frames_requested": args.frames,
            "frames_written": frames_written,
            "dropped_frames": len(missing),
            "missing_frames": missing[:100],
        },
        "timing": timing,
        "fused_timing": fused_timing,
        "bench_phase_timing": bench_phase_timing,
        "writer_handoff": writer_handoff,
        "jans_inline_profile": jans_inline_profile,
        "storage": {
            "frame_dir": str(frame_dir),
            "total_frame_bytes": total_frame_bytes,
            "gvid_bytes": gvid.stat().st_size,
            "write_MBps_wall": (total_frame_bytes / (1024 * 1024) / wall_s) if wall_s > 0 else 0.0,
            "fsync_policy": "bench_fused sequential .gvid fwrite" if args.direct_gvid and not args.simulate else "bench_fused fclose per frame; wrapper packs .gvid after run",
            "target": storage_target,
        },
        "memory": {
            "wrapper_maxrss_kb": maxrss_kb(),
            "bench_child_maxrss_kb": children_maxrss_kb(),
        },
        "cpu": {
            "loadavg_start": load_start,
            "loadavg_end": load_end,
            "utilization_policy": "load average receipt; per-core utilization requires target runner instrumentation",
        },
        "thermal": {
            "start_temp_c": temp_start,
            "end_temp_c": temp_end,
        },
        "gvid": {
            "path": str(gvid),
            "sha256": gvid_sha256,
            "sha256_policy": gvid_sha256_policy,
            "validation": gvid_info,
        },
        "interruption_recovery": interruption,
        "bench": {
            "cmd": [str(args.bench), str(args.raw), str(args.source_width), str(args.source_height), str(args.frames)] if not args.simulate else None,
            "env_overrides": bench_env_overrides,
            "build": bench_build_info(args.bench) if not args.simulate else {},
            "stdout_tail": bench_result.stdout[-2000:] if bench_result else None,
            "stderr_tail": bench_result.stderr[-2000:] if bench_result else None,
        },
        "verdict": {
            "fps_target_met": median_fps_target_met and wall_fps_target_met,
            "fps_median_target_met": median_fps_target_met,
            "fps_wall_target_met": wall_fps_target_met,
            "no_drops": len(missing) == 0,
            "gvid_valid": bool(gvid_info.get("valid")),
            "interruption_recovery_proven": bool(interruption["validator_rejects_truncated"]),
            "storage_target_met": bool(storage_target["fits_target"]),
            "target_evidence": target_evidence,
        },
    }
    out = args.output_dir / "labs_target_bench.json"
    out.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"receipt": str(out), "verdict": receipt["verdict"], "timing": timing}, indent=2))
    ok = receipt["verdict"]["gvid_valid"] and receipt["verdict"]["no_drops"] and receipt["verdict"]["storage_target_met"]
    if not args.simulate:
        ok = ok and receipt["verdict"]["fps_target_met"]
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
