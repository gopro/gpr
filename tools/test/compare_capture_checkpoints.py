#!/usr/bin/env python3
"""Compare fresh Bayer-to-GVID builds, including final output fsync in wall time."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_labs_bundle import validate_gvid


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--target-fps", type=float, default=20)
    parser.add_argument("--environment", type=Path, required=True)
    parser.add_argument("--storage", type=Path, required=True, help="existing directory on the device to measure")
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    if not Path("/usr/bin/time").is_file() or sys.platform != "linux":
        parser.error("requires Linux and GNU time at /usr/bin/time (package: time)")
    if not math.isfinite(args.target_fps) or min(args.width, args.height, args.frames, args.target_fps) <= 0:
        parser.error("dimensions, frame count, and FPS must be positive")
    if args.raw.stat().st_size != args.width * args.height * 2:
        parser.error("raw must be one complete unpacked uint16 Bayer frame")
    overrides = json.loads(args.environment.read_text())
    if not isinstance(overrides, dict) or not all(
        isinstance(k, str) and k.startswith(("GPR_", "FUSED_")) and isinstance(v, str)
        for k, v in overrides.items()
    ):
        parser.error("environment must map GPR_/FUSED_ variable names to strings")
    env = {k: v for k, v in os.environ.items() if not k.startswith(("GPR_", "FUSED_"))}
    env.update(overrides)
    env["GPR_BENCH_GVID_FPS"] = str(args.target_fps)
    rows = {}
    with tempfile.TemporaryDirectory(prefix="gpr_capture_compare_", dir=args.storage) as directory:
        root = Path(directory)
        for label in ("baseline", "candidate"):
            binary = getattr(args, label).resolve()
            output, memory = root / "capture.gvid", root / "memory.txt"
            started = time.monotonic()
            result = subprocess.run(
                ["/usr/bin/time", "-v", "-o", str(memory), str(binary),
                 str(args.raw.resolve()), str(args.width), str(args.height), str(args.frames)],
                env=dict(env, GPR_BENCH_GVID=str(output)), capture_output=True, text=True,
                check=True, timeout=600,
            )
            encoded = time.monotonic()
            with output.open("rb+") as stream:
                os.fsync(stream.fileno())
            synced = time.monotonic()
            validation = validate_gvid(output)
            assert validation["frame_count"] == args.frames, validation
            assert (validation["width"], validation["height"]) == (args.width, args.height), validation
            rss_line = next(line for line in memory.read_text().splitlines()
                            if "Maximum resident set size" in line)
            rows[label] = {
                "binary_sha256": digest(binary), "gvid_sha256": digest(output),
                "bytes": output.stat().st_size, "validation": validation,
                "encode_write_seconds": encoded - started, "final_fsync_seconds": synced - encoded,
                "durable_wall_seconds": synced - started,
                "durable_wall_fps": args.frames / (synced - started),
                "maxrss_kib": int(rss_line.rsplit(":", 1)[1].strip()),
                "log": result.stderr,
            }
            output.unlink()
    equal = rows["baseline"]["gvid_sha256"] == rows["candidate"]["gvid_sha256"]
    receipt = {
        "schema": "gpr_capture_checkpoint_comparison.v1", "input": args.raw.name,
        "input_sha256": digest(args.raw), "unique_input_frames": 1,
        "policy": "Freshly encode the same Bayer input for each frame; include final fsync; not a sensor/DMA or long-duration test.",
        "storage": str(args.storage.resolve()), "frames": args.frames,
        "environment": overrides, "target_fps": args.target_fps, "rows": rows,
        "identical_gvid": equal,
        "both_meet_target": all(row["durable_wall_fps"] >= args.target_fps for row in rows.values()),
    }
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"identical_gvid": equal, "both_meet_target": receipt["both_meet_target"],
                      "fps": {k: v["durable_wall_fps"] for k, v in rows.items()}}, indent=2))
    return 0 if equal and receipt["both_meet_target"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
