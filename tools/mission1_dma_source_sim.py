#!/usr/bin/env python3
"""Simulate a Mission 1 DMA/ring-buffer raw frame source.

This is deliberately not production camera evidence. It creates a process-level
producer and consumer around a FIFO so we can measure a deterministic stand-in
for camera-source timing: inter-frame cadence, producer backpressure, consumer
wait, and complete-frame delivery.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import sys
import tempfile
import time
from multiprocessing import Process
from pathlib import Path
from typing import Any


SCHEMA = "gpr.mission1_dma_source_sim.v1"


def now_ns() -> int:
    return time.perf_counter_ns()


def ms_from_ns(value: int) -> float:
    return value / 1_000_000.0


def percentile(values: list[float], frac: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, round((len(ordered) - 1) * frac)))
    return float(ordered[idx])


def summary(values: list[float]) -> dict[str, Any]:
    if not values:
        return {"n": 0, "min": 0.0, "median": 0.0, "mean": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0}
    return {
        "n": len(values),
        "min": min(values),
        "median": statistics.median(values),
        "mean": statistics.fmean(values),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values),
    }


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_frame(frame_bytes: int, index: int) -> bytes:
    chunk_size = min(frame_bytes, 64 * 1024)
    pattern = bytearray(chunk_size)
    seed = (index * 1315423911) & 0xFFFFFFFF
    for offset in range(0, chunk_size, 2):
        value = (offset // 2 + seed) & 0x3FFF
        pattern[offset] = value & 0xFF
        pattern[offset + 1] = (value >> 8) & 0xFF
    pattern[:4] = index.to_bytes(4, "little", signed=False)
    chunk = bytes(pattern)
    repeats, remainder = divmod(frame_bytes, len(chunk))
    return chunk * repeats + chunk[:remainder]


def read_exact(fd: int, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining > 0:
        chunk = os.read(fd, min(remaining, 1024 * 1024))
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def write_exact(fd: int, payload: bytes) -> None:
    view = memoryview(payload)
    offset = 0
    while offset < len(payload):
        written = os.write(fd, view[offset:])
        if written <= 0:
            raise OSError("FIFO write returned zero bytes")
        offset += written


def parse_delay_pattern(text: str) -> list[float]:
    if not text:
        return []
    values: list[float] = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        values.append(float(item))
    return values


def producer(
    fifo: str,
    frame_bytes: int,
    frames: int,
    interval_ms: float,
    delay_pattern_ms: list[float],
    ready_path: str,
    report_path: str,
) -> None:
    rows: list[dict[str, Any]] = []
    first_hash = ""
    last_hash = ""
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
            frame = make_frame(frame_bytes, index)
            if index == 0:
                first_hash = sha256_bytes(frame)
            if index == frames - 1:
                last_hash = sha256_bytes(frame)
            write_start_ns = now_ns()
            write_exact(fd, frame)
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
                "rows": rows,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def consumer(
    fifo: str,
    frame_bytes: int,
    frames: int,
    consumer_delay_ms: float,
    report_path: str,
) -> None:
    rows: list[dict[str, Any]] = []
    first_hash = ""
    last_hash = ""
    fd = os.open(fifo, os.O_RDONLY)
    try:
        base_ns = now_ns()
        for index in range(frames):
            read_start_ns = now_ns()
            payload = read_exact(fd, frame_bytes)
            read_end_ns = now_ns()
            complete = len(payload) == frame_bytes
            if index == 0 and payload:
                first_hash = sha256_bytes(payload)
            if complete:
                last_hash = sha256_bytes(payload)
            rows.append(
                {
                    "frame": index,
                    "bytes_read": len(payload),
                    "complete": complete,
                    "read_start_ms": ms_from_ns(read_start_ns - base_ns),
                    "read_ms": ms_from_ns(read_end_ns - read_start_ns),
                }
            )
            if not complete:
                break
            if consumer_delay_ms > 0:
                time.sleep(consumer_delay_ms / 1000.0)
    finally:
        os.close(fd)
    Path(report_path).write_text(
        json.dumps(
            {
                "frames_requested": frames,
                "frames_read": len(rows),
                "complete_frames": sum(1 for row in rows if row["complete"]),
                "first_frame_sha256": first_hash,
                "last_frame_sha256": last_hash,
                "rows": rows,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def deltas(rows: list[dict[str, Any]], key: str) -> list[float]:
    values = [float(row[key]) for row in rows if isinstance(row.get(key), (int, float))]
    return [values[i] - values[i - 1] for i in range(1, len(values))]


def load_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"{path} must contain a JSON object")
    return data


def build_report(args: argparse.Namespace, fifo: Path, producer_report: Path, consumer_report: Path, elapsed_ms: float) -> dict[str, Any]:
    prod = load_json(producer_report)
    cons = load_json(consumer_report)
    prod_rows = prod.get("rows") if isinstance(prod.get("rows"), list) else []
    cons_rows = cons.get("rows") if isinstance(cons.get("rows"), list) else []
    write_ms = [float(row["write_ms"]) for row in prod_rows if isinstance(row, dict) and isinstance(row.get("write_ms"), (int, float))]
    lateness_ms = [
        float(row["lateness_ms"]) for row in prod_rows if isinstance(row, dict) and isinstance(row.get("lateness_ms"), (int, float))
    ]
    read_ms = [float(row["read_ms"]) for row in cons_rows if isinstance(row, dict) and isinstance(row.get("read_ms"), (int, float))]
    frame_intervals_ms = deltas(cons_rows, "read_start_ms")
    complete_frames = int(cons.get("complete_frames") or 0)
    frames_written = int(prod.get("frames_written") or 0)
    dropped_or_incomplete = max(0, int(args.frames) - complete_frames)
    target_interval_ms = 1000.0 / float(args.target_fps)
    over_budget_intervals = sum(1 for value in frame_intervals_ms if value > target_interval_ms * float(args.interval_overrun_ratio))
    source_ready = (
        frames_written == int(args.frames)
        and complete_frames == int(args.frames)
        and prod.get("first_frame_sha256") == cons.get("first_frame_sha256")
        and prod.get("last_frame_sha256") == cons.get("last_frame_sha256")
    )
    return {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "target": {
            "name": args.target_name,
            "role": "stand-in",
            "simulates": args.simulates,
            "not_camera_evidence": True,
        },
        "source": {
            "raw_source_kind": args.raw_source_kind,
            "endpoint": str(fifo),
            "width": args.source_width,
            "height": args.source_height,
            "bytes_per_pixel": 2,
            "frame_bytes": args.source_width * args.source_height * 2,
            "frames": args.frames,
            "target_fps": args.target_fps,
            "target_interval_ms": target_interval_ms,
            "delay_pattern_ms": args.delay_pattern_ms,
            "consumer_delay_ms": args.consumer_delay_ms,
        },
        "producer": {
            "process": "separate",
            "frames_written": frames_written,
            "write_ms": summary(write_ms),
            "lateness_ms": summary(lateness_ms),
            "first_frame_sha256": prod.get("first_frame_sha256"),
            "last_frame_sha256": prod.get("last_frame_sha256"),
        },
        "consumer": {
            "process": "separate",
            "frames_read": int(cons.get("frames_read") or 0),
            "complete_frames": complete_frames,
            "read_ms": summary(read_ms),
            "frame_intervals_ms": summary(frame_intervals_ms),
            "over_budget_intervals": over_budget_intervals,
            "first_frame_sha256": cons.get("first_frame_sha256"),
            "last_frame_sha256": cons.get("last_frame_sha256"),
        },
        "timing": {
            "elapsed_ms": elapsed_ms,
            "effective_fps": (complete_frames * 1000.0 / elapsed_ms) if elapsed_ms > 0 else 0.0,
        },
        "verdict": {
            "source_ready": source_ready,
            "deterministic_simulation": True,
            "production_evidence": False,
            "no_incomplete_frames": dropped_or_incomplete == 0,
            "hashes_match": prod.get("first_frame_sha256") == cons.get("first_frame_sha256")
            and prod.get("last_frame_sha256") == cons.get("last_frame_sha256"),
            "over_budget_intervals": over_budget_intervals,
        },
        "blockers": [] if source_ready else ["simulated source did not deliver every complete frame"],
        "artifacts": {
            "producer_report": str(producer_report),
            "consumer_report": str(consumer_report),
        },
    }


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--work-dir", type=Path)
    ap.add_argument("--target-name", default="Pi 5 DMA-source simulator")
    ap.add_argument("--simulates", default="Mission 1 sensor DMA/ring-buffer cadence")
    ap.add_argument("--raw-source-kind", choices=("sensor_dma_capture", "camera_ring_buffer"), default="sensor_dma_capture")
    ap.add_argument("--source-width", type=int, default=4096)
    ap.add_argument("--source-height", type=int, default=3072)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--target-fps", type=float, default=20.0)
    ap.add_argument("--delay-pattern-ms", default="", help="comma-separated deterministic producer delay offsets")
    ap.add_argument("--consumer-delay-ms", type=float, default=0.0, help="optional deterministic consumer delay after each frame")
    ap.add_argument("--interval-overrun-ratio", type=float, default=1.25)
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
    args.delay_pattern_ms = parse_delay_pattern(args.delay_pattern_ms)

    base_dir = args.work_dir
    temp_ctx: tempfile.TemporaryDirectory[str] | None = None
    if base_dir is None:
        parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
        parent.mkdir(parents=True, exist_ok=True)
        temp_ctx = tempfile.TemporaryDirectory(prefix="mission1_dma_source_sim_", dir=parent)
        base_dir = Path(temp_ctx.name)
    base_dir.mkdir(parents=True, exist_ok=True)
    fifo = base_dir / "sensor_dma_ring.fifo"
    producer_ready = base_dir / "producer.ready"
    producer_report = base_dir / "producer_report.json"
    consumer_report = base_dir / "consumer_report.json"
    if fifo.exists():
        fifo.unlink()
    os.mkfifo(fifo)

    frame_bytes = args.source_width * args.source_height * 2
    interval_ms = 1000.0 / args.target_fps
    started_ns = now_ns()
    prod = Process(
        target=producer,
        args=(
            str(fifo),
            frame_bytes,
            args.frames,
            interval_ms,
            args.delay_pattern_ms,
            str(producer_ready),
            str(producer_report),
        ),
    )
    cons = Process(
        target=consumer,
        args=(str(fifo), frame_bytes, args.frames, args.consumer_delay_ms, str(consumer_report)),
    )
    prod.start()
    deadline = time.time() + 5.0
    while not producer_ready.exists() and time.time() < deadline:
        time.sleep(0.005)
    cons.start()
    prod.join(timeout=max(10.0, args.frames / args.target_fps + 10.0))
    cons.join(timeout=max(10.0, args.frames / args.target_fps + 10.0))
    elapsed_ms = ms_from_ns(now_ns() - started_ns)
    exit_code = 0
    if prod.is_alive():
        prod.terminate()
        exit_code = 1
    if cons.is_alive():
        cons.terminate()
        exit_code = 1
    if prod.exitcode not in (0, None) or cons.exitcode not in (0, None):
        exit_code = 1
    if exit_code == 0:
        report = build_report(args, fifo, producer_report, consumer_report, elapsed_ms)
    else:
        report = {
            "schema": SCHEMA,
            "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "verdict": {"source_ready": False, "deterministic_simulation": True, "production_evidence": False},
            "blockers": [f"producer_exit={prod.exitcode}", f"consumer_exit={cons.exitcode}"],
        }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if temp_ctx is not None and os.environ.get("GPR_KEEP_TEST_ARTIFACTS") != "1":
        temp_ctx.cleanup()
    print(json.dumps({"output": str(args.output), "source_ready": report.get("verdict", {}).get("source_ready")}, indent=2))
    return 0 if report.get("verdict", {}).get("source_ready") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
