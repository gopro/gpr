#!/usr/bin/env python3
"""Feed a deterministic Mission 1-shaped source stream into the Labs encoder.

This is the bridge between the DMA-source simulator and the real firmware-facing
encoder shim. It starts a separate producer process that writes raw Bayer frames
to a FIFO, then runs `labs_encoder_bench_cli` in streaming mode so each encoded
frame is read from that FIFO rather than from a preloaded file.

The resulting receipt is still stand-in evidence. It proves deterministic
source cadence plus encoder/container behavior, not real Mission 1 sensor/DMA,
storage, or display integration.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from multiprocessing import Process
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from mission1_dma_source_sim import (  # noqa: E402
    make_frame,
    now_ns,
    parse_delay_pattern,
    summary,
)
from mission1_native12_fll2_t2_profile import PROFILE_ID as MISSION1_FLL2_PROFILE_ID  # noqa: E402
from mission1_native12_fll2_t2_profile import profile_env  # noqa: E402
from run_labs_target_bench import validate_gvid  # noqa: E402


SCHEMA = "gpr.mission1_stream_source_encoder.v1"
LABS_STREAM_FRAME_RE = re.compile(
    r"^#\s+stream_frame\s+frame=(?P<frame>[0-9]+)\s+"
    r"source_read_ms=(?P<source>[0-9]+(?:\.[0-9]+)?)\s+"
    r"submit_ms=(?P<submit>[0-9]+(?:\.[0-9]+)?)\s*$"
)
BENCH_FUSED_STREAM_FRAME_RE = re.compile(
    r"^#\s+stream_frame\s+frame=(?P<frame>[0-9]+)\s+"
    r"source_read_ms=(?P<source>[0-9]+(?:\.[0-9]+)?)\s+"
    r"encode_write_ms=(?P<encode_write>[0-9]+(?:\.[0-9]+)?)\s*$"
)
LABS_STATS_RE = re.compile(r"^#\s+labs_encoder_stats\s+(?P<body>.+)$")
KV_RE = re.compile(r"(?P<key>[A-Za-z0-9_]+)=(?P<value>[0-9]+)")


def ms_from_ns(value: int) -> float:
    return value / 1_000_000.0


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"{path} must contain a JSON object")
    return data


def replay_paths(raw: Path | None) -> list[Path]:
    if raw is None:
        return []
    if raw.is_file():
        return [raw]
    if raw.is_dir():
        paths = sorted(p for p in raw.iterdir() if p.suffix.lower() == ".raw")
        if paths:
            return paths
    raise FileNotFoundError(f"--replay-raw must be a .raw file or directory containing .raw files: {raw}")


def load_replay_frames(paths: list[str], frame_bytes: int) -> list[bytes]:
    frames: list[bytes] = []
    for item in paths:
        path = Path(item)
        data = path.read_bytes()
        if len(data) != frame_bytes:
            raise ValueError(f"replay raw size mismatch for {path}: got {len(data)} expected {frame_bytes}")
        frames.append(data)
    return frames


def frame_for_index(frame_bytes: int, index: int, replay_frames: list[bytes]) -> bytes:
    if replay_frames:
        return replay_frames[index % len(replay_frames)]
    return make_frame(frame_bytes, index)


def parse_stdout(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m_labs = LABS_STREAM_FRAME_RE.match(line)
        if m_labs:
            rows.append(
                {
                    "frame": int(m_labs.group("frame")),
                    "source_read_ms": float(m_labs.group("source")),
                    "submit_ms": float(m_labs.group("submit")),
                }
            )
            continue
        m_bench = BENCH_FUSED_STREAM_FRAME_RE.match(line)
        if m_bench:
            rows.append(
                {
                    "frame": int(m_bench.group("frame")),
                    "source_read_ms": float(m_bench.group("source")),
                    "encode_write_ms": float(m_bench.group("encode_write")),
                }
            )
            continue
    return rows


def parse_stderr(path: Path) -> dict[str, Any]:
    stats: dict[str, Any] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = LABS_STATS_RE.match(line)
        if not m:
            continue
        for kv in KV_RE.finditer(m.group("body")):
            stats[kv.group("key")] = int(kv.group("value"))
    return stats


def mmap_ring_producer(
    ring_path: str,
    frame_bytes: int,
    frames: int,
    slots: int,
    interval_ms: float,
    delay_pattern_ms: list[float],
    replay_raw_paths: list[str],
    producer_mode: str,
    ready_path: str,
    report_path: str,
) -> None:
    slot_stride = 64 + frame_bytes
    rows: list[dict[str, Any]] = []
    first_hash = ""
    last_hash = ""
    replay_frames = load_replay_frames(replay_raw_paths, frame_bytes)
    ring = Path(ring_path)
    with ring.open("r+b") as fp:
        mm = mmap.mmap(fp.fileno(), slot_stride * slots)
        try:
            if producer_mode == "ready-only":
                if not replay_frames:
                    raise ValueError("ready-only producer mode requires --replay-raw")
                for slot in range(slots):
                    frame = replay_frames[slot % len(replay_frames)]
                    offset = slot * slot_stride
                    mm[offset + 64 : offset + 64 + frame_bytes] = frame
                    mm[offset + 8 : offset + 16] = struct.pack("<Q", frame_bytes)
            Path(ready_path).write_text("ready\n", encoding="utf-8")
            base_ns = now_ns()
            for index in range(frames):
                target_ns = base_ns + int(index * interval_ms * 1_000_000)
                if delay_pattern_ms:
                    target_ns += int(delay_pattern_ms[index % len(delay_pattern_ms)] * 1_000_000)
                before_sleep_ns = now_ns()
                if target_ns > before_sleep_ns:
                    time.sleep((target_ns - before_sleep_ns) / 1_000_000_000.0)
                frame = frame_for_index(frame_bytes, index, replay_frames)
                if index == 0:
                    first_hash = hashlib.sha256(frame).hexdigest()
                if index == frames - 1:
                    last_hash = hashlib.sha256(frame).hexdigest()
                slot = index % slots
                offset = slot * slot_stride
                if index >= slots:
                    want_consumed = index - slots + 1
                    while struct.unpack("<Q", mm[offset + 16 : offset + 24])[0] < want_consumed:
                        time.sleep(0.0001)
                write_start_ns = now_ns()
                if producer_mode != "ready-only":
                    mm[offset + 64 : offset + 64 + frame_bytes] = frame
                    mm[offset + 8 : offset + 16] = struct.pack("<Q", frame_bytes)
                mm[offset : offset + 8] = struct.pack("<Q", index + 1)
                write_end_ns = now_ns()
                rows.append(
                    {
                        "frame": index,
                        "slot": slot,
                        "scheduled_ms": ms_from_ns(target_ns - base_ns),
                        "write_start_ms": ms_from_ns(write_start_ns - base_ns),
                        "write_ms": ms_from_ns(write_end_ns - write_start_ns),
                        "lateness_ms": ms_from_ns(max(0, write_start_ns - target_ns)),
                    }
                )
        finally:
            mm.close()
    Path(report_path).write_text(
        json.dumps(
            {
                "frames_attempted": frames,
                "frames_written": len(rows),
                "first_frame_sha256": first_hash,
                "last_frame_sha256": last_hash,
                "replay_raw_paths": replay_raw_paths,
                "replay_frame_count": len(replay_frames),
                "producer_mode": producer_mode,
                "rows": rows,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def fifo_replay_producer(
    fifo: str,
    frame_bytes: int,
    frames: int,
    interval_ms: float,
    delay_pattern_ms: list[float],
    replay_raw_paths: list[str],
    ready_path: str,
    report_path: str,
) -> None:
    rows: list[dict[str, Any]] = []
    first_hash = ""
    last_hash = ""
    replay_frames = load_replay_frames(replay_raw_paths, frame_bytes)
    Path(ready_path).write_text("ready\n", encoding="utf-8")
    fd = os.open(fifo, os.O_WRONLY)
    try:
        base_ns = now_ns()
        for index in range(frames):
            target_ns = base_ns + int(index * interval_ms * 1_000_000)
            if delay_pattern_ms:
                target_ns += int(delay_pattern_ms[index % len(delay_pattern_ms)] * 1_000_000)
            before_sleep_ns = now_ns()
            if target_ns > before_sleep_ns:
                time.sleep((target_ns - before_sleep_ns) / 1_000_000_000.0)
            frame = frame_for_index(frame_bytes, index, replay_frames)
            if index == 0:
                first_hash = hashlib.sha256(frame).hexdigest()
            if index == frames - 1:
                last_hash = hashlib.sha256(frame).hexdigest()
            write_start_ns = now_ns()
            view = memoryview(frame)
            offset = 0
            while offset < len(view):
                written = os.write(fd, view[offset:])
                if written <= 0:
                    raise OSError("FIFO write returned zero bytes")
                offset += written
            write_end_ns = now_ns()
            rows.append(
                {
                    "frame": index,
                    "scheduled_ms": ms_from_ns(target_ns - base_ns),
                    "write_start_ms": ms_from_ns(write_start_ns - base_ns),
                    "write_ms": ms_from_ns(write_end_ns - write_start_ns),
                    "lateness_ms": ms_from_ns(max(0, write_start_ns - target_ns)),
                }
            )
    finally:
        os.close(fd)
    Path(report_path).write_text(
        json.dumps(
            {
                "frames_attempted": frames,
                "frames_written": len(rows),
                "first_frame_sha256": first_hash,
                "last_frame_sha256": last_hash,
                "replay_raw_paths": replay_raw_paths,
                "replay_frame_count": len(replay_frames),
                "rows": rows,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def run_encoder(args: argparse.Namespace, source_path: Path, gvid: Path, stdout_path: Path, stderr_path: Path) -> int:
    env = os.environ.copy()
    env.update(
        {
            "GPR_BENCH_GVID": str(gvid),
            "GPR_BENCH_GVID_FPS": str(args.target_fps),
            "GPR_BENCH_PIXEL_FORMAT": str(args.pixel_format),
            "FUSED_QUALITY": str(args.quality),
            "GPR_LABS_BIT_DEPTH": str(args.bit_depth),
            "GPR_LABS_STRIDE_BYTES": str(args.stride_bytes),
        }
    )
    if args.use_mission1_fll2_profile:
        env.update(profile_env())
    if args.encoder_kind == "labs":
        if args.source_mode == "fifo":
            env["GPR_LABS_STREAM_INPUT"] = "1"
        elif args.source_mode == "mmap-ring":
            env["GPR_LABS_MMAP_RING_INPUT"] = "1"
            env["GPR_LABS_MMAP_RING_SLOTS"] = str(args.ring_slots)
    elif args.source_mode == "fifo":
        env["GPR_BENCH_STREAM_INPUT"] = "1"
    elif args.source_mode == "mmap-ring":
        env["GPR_BENCH_MMAP_RING_INPUT"] = "1"
        env["GPR_BENCH_MMAP_RING_SLOTS"] = str(args.ring_slots)
    if args.encoder_count and args.encoder_kind == "labs":
        env["GPR_LABS_ENCODER_COUNT"] = str(args.encoder_count)
    if args.max_inflight and args.encoder_kind == "labs":
        env["GPR_LABS_MAX_INFLIGHT"] = str(args.max_inflight)

    cmd = [str(args.bench), str(source_path), str(args.source_width), str(args.source_height), str(args.frames)]
    with stdout_path.open("w", encoding="utf-8") as out, stderr_path.open("w", encoding="utf-8") as err:
        proc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=out, stderr=err, text=True)
    return int(proc.returncode)


def build_report(
    args: argparse.Namespace,
    source_path: Path,
    gvid: Path,
    producer_report: Path,
    stdout_path: Path,
    stderr_path: Path,
    elapsed_ms: float,
    encoder_returncode: int,
) -> dict[str, Any]:
    prod = load_json(producer_report) if producer_report.exists() else {}
    prod_rows = prod.get("rows") if isinstance(prod.get("rows"), list) else []
    stream_rows = parse_stdout(stdout_path) if stdout_path.exists() else []
    labs_stats = parse_stderr(stderr_path) if stderr_path.exists() else {}
    producer_write_ms = [
        float(row["write_ms"]) for row in prod_rows if isinstance(row, dict) and isinstance(row.get("write_ms"), (int, float))
    ]
    producer_lateness_ms = [
        float(row["lateness_ms"])
        for row in prod_rows
        if isinstance(row, dict) and isinstance(row.get("lateness_ms"), (int, float))
    ]
    source_read_ms = [float(row["source_read_ms"]) for row in stream_rows]
    submit_ms = [float(row["submit_ms"]) for row in stream_rows if isinstance(row.get("submit_ms"), (int, float))]
    encode_write_ms = [float(row["encode_write_ms"]) for row in stream_rows if isinstance(row.get("encode_write_ms"), (int, float))]
    frame_bytes = int(args.stride_bytes) * int(args.source_height)
    validation: dict[str, Any]
    gvid_sha256: str | None = None
    try:
        validation = validate_gvid(gvid)
        validation["valid"] = True
        gvid_sha256 = sha256_file(gvid)
    except Exception as exc:
        validation = {"valid": False, "error": str(exc)}

    frames_written = int(prod.get("frames_written") or 0)
    frames_encoded = int(labs_stats.get("written", 0)) if args.encoder_kind == "labs" else len(stream_rows)
    complete = (
        encoder_returncode == 0
        and frames_written == int(args.frames)
        and len(stream_rows) == int(args.frames)
        and frames_encoded == int(args.frames)
        and validation.get("valid") is True
        and int(validation.get("frame_count", -1)) == int(args.frames)
    )
    return {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "target": {
            "name": args.target_name,
            "role": "stand-in",
            "not_camera_evidence": True,
        },
        "source": {
            "mode": args.source_mode,
            "raw_source_kind": args.raw_source_kind,
            "endpoint": str(source_path),
            "width": args.source_width,
            "height": args.source_height,
            "stride_bytes": args.stride_bytes,
            "bit_depth": args.bit_depth,
            "pixel_format": args.pixel_format,
            "frame_bytes": frame_bytes,
            "frames": args.frames,
            "target_fps": args.target_fps,
            "delay_pattern_ms": args.delay_pattern_ms,
            "ring_slots": args.ring_slots if args.source_mode == "mmap-ring" else None,
            "replay_raw": [str(p) for p in args.replay_raw_paths],
            "producer_mode": args.producer_mode,
        },
        "producer": {
            "process": "separate",
            "frames_written": frames_written,
            "write_ms": summary(producer_write_ms),
            "lateness_ms": summary(producer_lateness_ms),
            "first_frame_sha256": prod.get("first_frame_sha256"),
            "last_frame_sha256": prod.get("last_frame_sha256"),
            "replay_frame_count": prod.get("replay_frame_count", 0),
        },
        "encoder": {
            "process": "separate",
            "kind": args.encoder_kind,
            "binary": str(args.bench),
            "returncode": encoder_returncode,
            "stream_frames": len(stream_rows),
            "source_read_ms": summary(source_read_ms),
            "submit_ms": summary(submit_ms),
            "encode_write_ms": summary(encode_write_ms),
            "mission1_fll2_profile": MISSION1_FLL2_PROFILE_ID if args.use_mission1_fll2_profile else None,
            "labs_encoder_stats": labs_stats,
        },
        "output": {
            "gvid": str(gvid),
            "sha256": gvid_sha256,
            "validation": validation,
        },
        "timing": {
            "elapsed_ms": elapsed_ms,
            "effective_fps": (float(args.frames) * 1000.0 / elapsed_ms) if elapsed_ms > 0 else 0.0,
        },
        "verdict": {
            "stream_encode_ready": complete,
            "deterministic_simulation": True,
            "production_evidence": False,
            "gvid_valid": validation.get("valid") is True,
            "no_drops": frames_written == int(args.frames) and len(stream_rows) == int(args.frames),
        },
        "blockers": [] if complete else ["stream source to encoder did not complete every requested frame"],
        "artifacts": {
            "producer_report": str(producer_report),
            "bench_stdout": str(stdout_path),
            "bench_stderr": str(stderr_path),
        },
    }


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bench", type=Path, required=True, help="path to labs_encoder_bench_cli or bench_fused")
    ap.add_argument("--encoder-kind", choices=("labs", "bench-fused"), default="labs")
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--work-dir", type=Path)
    ap.add_argument("--source-dir", type=Path, help="optional directory for FIFO or mmap-ring source files")
    ap.add_argument("--target-name", default="Pi 5 stream-source encoder stand-in")
    ap.add_argument("--raw-source-kind", choices=("sensor_dma_capture", "camera_ring_buffer"), default="sensor_dma_capture")
    ap.add_argument("--source-width", type=int, default=4096)
    ap.add_argument("--source-height", type=int, default=3072)
    ap.add_argument("--stride-bytes", type=int, default=0)
    ap.add_argument("--bit-depth", type=int, default=16)
    ap.add_argument("--pixel-format", type=int, default=1)
    ap.add_argument("--quality", type=int, default=8)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--target-fps", type=float, default=20.0)
    ap.add_argument("--delay-pattern-ms", default="", help="comma-separated deterministic producer delay offsets")
    ap.add_argument("--source-mode", choices=("fifo", "mmap-ring"), default="fifo")
    ap.add_argument("--ring-slots", type=int, default=3)
    ap.add_argument("--encoder-count", type=int, default=0)
    ap.add_argument("--max-inflight", type=int, default=0)
    ap.add_argument("--use-mission1-fll2-profile", action="store_true")
    ap.add_argument("--replay-raw", type=Path, help="optional .raw file or directory to replay as source content")
    ap.add_argument("--producer-mode", choices=("copy", "ready-only"), default="copy")
    return ap


def main() -> int:
    args = parser().parse_args()
    if args.frames <= 0:
        print("--frames must be positive", file=sys.stderr)
        return 1
    if args.source_width <= 0 or args.source_height <= 0:
        print("--source-width and --source-height must be positive", file=sys.stderr)
        return 1
    if args.target_fps <= 0:
        print("--target-fps must be positive", file=sys.stderr)
        return 1
    if args.stride_bytes == 0:
        args.stride_bytes = args.source_width * 2
    if args.stride_bytes < args.source_width * 2:
        print("--stride-bytes must be at least source_width*2", file=sys.stderr)
        return 1
    if args.encoder_kind == "bench-fused" and args.stride_bytes != args.source_width * 2:
        print("--encoder-kind bench-fused requires packed Bayer stride_bytes=source_width*2", file=sys.stderr)
        return 1
    if args.ring_slots <= 0:
        print("--ring-slots must be positive", file=sys.stderr)
        return 1
    if args.producer_mode == "ready-only" and args.source_mode != "mmap-ring":
        print("--producer-mode ready-only requires --source-mode mmap-ring", file=sys.stderr)
        return 1
    if not args.bench.is_file():
        print(f"--bench does not exist: {args.bench}", file=sys.stderr)
        return 1
    args.delay_pattern_ms = parse_delay_pattern(args.delay_pattern_ms)
    try:
        args.replay_raw_paths = replay_paths(args.replay_raw)
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1

    temp_ctx: tempfile.TemporaryDirectory[str] | None = None
    base_dir = args.work_dir
    if base_dir is None:
        parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
        parent.mkdir(parents=True, exist_ok=True)
        temp_ctx = tempfile.TemporaryDirectory(prefix="mission1_stream_source_encoder_", dir=parent)
        base_dir = Path(temp_ctx.name)
    base_dir.mkdir(parents=True, exist_ok=True)
    source_dir = args.source_dir or base_dir
    source_dir.mkdir(parents=True, exist_ok=True)
    source_path = source_dir / ("sensor_dma_ring.fifo" if args.source_mode == "fifo" else "sensor_dma_ring.mmap")
    producer_ready = base_dir / "producer.ready"
    producer_report = base_dir / "producer_report.json"
    stdout_path = base_dir / f"{args.encoder_kind}_stdout.txt"
    stderr_path = base_dir / f"{args.encoder_kind}_stderr.txt"
    gvid = base_dir / "stream_source_encoder.gvid"
    if source_path.exists():
        source_path.unlink()

    frame_bytes = args.stride_bytes * args.source_height
    interval_ms = 1000.0 / args.target_fps
    if args.source_mode == "fifo":
        os.mkfifo(source_path)
        prod = Process(
            target=fifo_replay_producer,
            args=(
                str(source_path),
                frame_bytes,
                args.frames,
                interval_ms,
                args.delay_pattern_ms,
                [str(p) for p in args.replay_raw_paths],
                str(producer_ready),
                str(producer_report),
            ),
        )
    else:
        slot_stride = 64 + frame_bytes
        with source_path.open("wb") as fp:
            fp.truncate(slot_stride * args.ring_slots)
        prod = Process(
            target=mmap_ring_producer,
            args=(
                str(source_path),
                frame_bytes,
                args.frames,
                args.ring_slots,
                interval_ms,
                args.delay_pattern_ms,
                [str(p) for p in args.replay_raw_paths],
                args.producer_mode,
                str(producer_ready),
                str(producer_report),
            ),
        )
    prod.start()
    deadline = time.time() + 5.0
    while not producer_ready.exists() and time.time() < deadline:
        time.sleep(0.005)

    started_ns = now_ns()
    encoder_returncode = run_encoder(args, source_path, gvid, stdout_path, stderr_path)
    prod.join(timeout=max(10.0, args.frames / args.target_fps + 10.0))
    if prod.is_alive():
        prod.terminate()
        encoder_returncode = encoder_returncode or 1
    if prod.exitcode not in (0, None):
        encoder_returncode = encoder_returncode or 1
    elapsed_ms = ms_from_ns(now_ns() - started_ns)

    report = build_report(args, source_path, gvid, producer_report, stdout_path, stderr_path, elapsed_ms, encoder_returncode)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "stream_encode_ready": report["verdict"]["stream_encode_ready"]}, indent=2))
    if temp_ctx is not None and os.environ.get("GPR_KEEP_TEST_ARTIFACTS") != "1":
        temp_ctx.cleanup()
    return 0 if report["verdict"]["stream_encode_ready"] is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
