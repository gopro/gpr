#!/usr/bin/env python3
"""Package retained Mission 1 8K SR Bayer raws into .gvid and ProRes receipts."""
from __future__ import annotations


import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any

import sys



sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import REPO, external_path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gvid_metadata import read_gvid_frames  # noqa: E402


SCHEMA = "mission1_8k_sr_sequence_packaging.v1"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def rel(path: Path, external_root: Path) -> str:
    try:
        return "artifacts/" + path.resolve().relative_to((external_root / "artifacts").resolve()).as_posix()
    except ValueError:
        return str(path)


def file_record(path: Path, external_root: Path) -> dict[str, Any]:
    return {
        "path": rel(path, external_root),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def run(cmd: list[str], log: Path) -> dict[str, Any]:
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
    elapsed = time.perf_counter() - t0
    text = proc.stdout + proc.stderr
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(text, encoding="utf-8")
    return {
        "command": cmd,
        "returncode": proc.returncode,
        "elapsed_s": elapsed,
        "stdout_tail": proc.stdout[-4000:],
        "stderr_tail": proc.stderr[-4000:],
    }


def tool_cmd(path: Path) -> list[str]:
    if path.suffix == ".py":
        return [sys.executable, str(path)]
    return [str(path)]


def require_ok(result: dict[str, Any], label: str) -> None:
    if int(result["returncode"]) != 0:
        tail = str(result.get("stderr_tail") or result.get("stdout_tail") or "")[-2000:]
        raise RuntimeError(f"{label} failed rc={result['returncode']}\n{tail}")


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    pos = (len(ordered) - 1) * pct / 100.0
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    frac = pos - lo
    return float(ordered[lo] * (1.0 - frac) + ordered[hi] * frac)


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "min": min(values) if values else 0.0,
        "median": percentile(values, 50),
        "p95": percentile(values, 95),
        "max": max(values) if values else 0.0,
        "mean": sum(values) / len(values) if values else 0.0,
    }


def ffprobe(path: Path) -> dict[str, Any]:
    out = subprocess.check_output([
        "ffprobe",
        "-v",
        "error",
        "-show_streams",
        "-show_format",
        "-of",
        "json",
        str(path),
    ])
    return json.loads(out)


def sr_raws(sr_raw_dir: Path, frame_count: int | None) -> list[Path]:
    raws = sorted(sr_raw_dir.glob("*_sr8k.raw"))
    if not raws:
        raws = sorted(sr_raw_dir.glob("*.raw"))
    if frame_count is not None:
        raws = raws[:frame_count]
    if not raws:
        raise FileNotFoundError(f"no SR raws found in {sr_raw_dir}")
    return raws


def build(args: argparse.Namespace) -> dict[str, Any]:
    args.out_dir.mkdir(parents=True, exist_ok=True)
    gpr_dir = args.out_dir / "gpr_frames"
    validation_dir = args.out_dir / "validation"
    log_dir = args.out_dir / "logs"
    for path in (gpr_dir, validation_dir, log_dir):
        path.mkdir(parents=True, exist_ok=True)

    raws = sr_raws(args.sr_raw_dir, args.frames)
    expected_bytes = args.width * args.height * 2
    frame_rows: list[dict[str, Any]] = []
    encode_times: list[float] = []
    decode_times: list[float] = []
    for idx, raw in enumerate(raws):
        if raw.stat().st_size != expected_bytes:
            raise ValueError(f"{raw} has {raw.stat().st_size} bytes, expected {expected_bytes}")
        gpr = gpr_dir / f"frame_{idx:06d}.gpr"
        encode = run([
            str(args.gpr_tools),
            "-i",
            str(raw),
            "-w",
            str(args.width),
            "-h",
            str(args.height),
            "-x",
            args.pixel_format,
            "-o",
            str(gpr),
            "-q",
            str(args.gpr_quality),
        ], log_dir / f"frame_{idx:06d}_raw_to_gpr.log")
        require_ok(encode, f"encode frame {idx}")
        encode_times.append(float(encode["elapsed_s"]))
        row = {
            "frame_index": idx,
            "input_raw": str(raw),
            "input_raw_bytes": raw.stat().st_size,
            "input_raw_sha256": sha256_file(raw),
            "gpr": file_record(gpr, args.external_root),
            "encode_elapsed_s": float(encode["elapsed_s"]),
            "encode_stdout_tail": encode["stdout_tail"],
            "encode_stderr_tail": encode["stderr_tail"],
        }
        if idx == 0 or args.validate_all_frames:
            decoded = validation_dir / f"frame_{idx:06d}_decoded.raw"
            decode = run([str(args.gpr_tools), "-i", str(gpr), "-o", str(decoded)], log_dir / f"frame_{idx:06d}_gpr_to_raw.log")
            require_ok(decode, f"decode frame {idx}")
            decode_times.append(float(decode["elapsed_s"]))
            row["decode_validation"] = {
                "decoded_raw_bytes": decoded.stat().st_size,
                "decoded_raw_sha256": sha256_file(decoded),
                "elapsed_s": float(decode["elapsed_s"]),
                "stdout_tail": decode["stdout_tail"],
                "stderr_tail": decode["stderr_tail"],
            }
            if not args.keep_validation_raw:
                decoded.unlink(missing_ok=True)
        frame_rows.append(row)

    gvid = args.out_dir / args.gvid_name
    gvid_pack = run([
        *tool_cmd(args.gvid_pack),
        str(gpr_dir),
        str(gvid),
        "--width",
        str(args.width),
        "--height",
        str(args.height),
        "--fps",
        str(args.fps),
        "--pixel-format",
        str(args.gvid_pixel_format),
        "--quality",
        str(args.gpr_quality),
    ], log_dir / "gvid_pack.log")
    require_ok(gvid_pack, "gvid pack")
    gvid_frames = read_gvid_frames(gvid)
    if len(gvid_frames) != len(raws):
        raise RuntimeError(f"packed .gvid has {len(gvid_frames)} frames, expected {len(raws)}")

    prores = args.out_dir / args.prores_name
    gpr2prores_cmd = [
        str(args.gpr2prores),
        "--meta-dng",
        str(args.meta_dng),
        "--no-cnn",
        "--demosaic",
        args.demosaic,
        "--out-resolution",
        args.out_resolution,
        "--fps",
        str(args.fps),
        "--timing",
        str(gvid),
        str(prores),
    ]
    prores_run = run(gpr2prores_cmd, log_dir / "gvid_to_prores.log")
    require_ok(prores_run, "gvid to prores")

    receipt = {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source_sr_receipt": str(args.source_sr_receipt) if args.source_sr_receipt else None,
        "source_sr_raw_dir": str(args.sr_raw_dir),
        "out_dir": str(args.out_dir),
        "width": args.width,
        "height": args.height,
        "quality": args.gpr_quality,
        "fps": args.fps,
        "frame_count": len(raws),
        "frames": frame_rows,
        "summary": {
            "encode_elapsed_s": summarize(encode_times),
            "decode_validation_elapsed_s": summarize(decode_times),
            "gpr_bytes_median": percentile([float(row["gpr"]["bytes"]) for row in frame_rows], 50),
            "gpr_bytes_total": sum(int(row["gpr"]["bytes"]) for row in frame_rows),
            "gvid_frame_count": len(gvid_frames),
        },
        "gvid_packaging": {
            **file_record(gvid, args.external_root),
            "frame_count": len(gvid_frames),
            "payload_bytes": sum(int(frame["payload_size"]) for frame in gvid_frames),
            "pack_log_tail": (gvid_pack["stdout_tail"] + gvid_pack["stderr_tail"])[-2000:],
        },
        "prores_review": {
            **file_record(prores, args.external_root),
            "ffprobe": ffprobe(prores),
            "render_elapsed_s": float(prores_run["elapsed_s"]),
            "render_stdout_tail": prores_run["stdout_tail"],
            "render_stderr_tail": prores_run["stderr_tail"],
        },
        "commands": {
            "gvid_pack": gvid_pack["command"],
            "gvid_to_prores": gpr2prores_cmd,
        },
    }
    if not args.keep_validation_raw:
        shutil.rmtree(validation_dir, ignore_errors=True)
    return receipt


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sr-raw-dir", type=Path, required=True)
    ap.add_argument("--source-sr-receipt", type=Path)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--external-root", type=Path, default=external_path(''))
    ap.add_argument("--gpr-tools", type=Path, default=REPO / "build-local/source/app/gpr_tools/gpr_tools")
    ap.add_argument("--gvid-pack", type=Path, default=REPO / "tools/gvid_pack.py")
    ap.add_argument("--gpr2prores", type=Path, default=REPO / "tools/gpr2prores/gpr2prores")
    ap.add_argument("--meta-dng", type=Path, required=True)
    ap.add_argument("--width", type=int, default=8192)
    ap.add_argument("--height", type=int, default=6144)
    ap.add_argument("--pixel-format", default="rggb14")
    ap.add_argument("--gvid-pixel-format", type=int, default=1)
    ap.add_argument("--gpr-quality", type=int, default=3)
    ap.add_argument("--fps", type=float, default=20.0)
    ap.add_argument("--frames", type=int)
    ap.add_argument("--validate-all-frames", action="store_true")
    ap.add_argument("--keep-validation-raw", action="store_true")
    ap.add_argument("--demosaic", default="core-image")
    ap.add_argument("--out-resolution", default="8k")
    ap.add_argument("--gvid-name", default="capture_8k_sr_q3.gvid")
    ap.add_argument("--prores-name", default="mission1_8k_sr_gvid_20p_prores.mov")
    args = ap.parse_args()
    receipt = build(args)
    out = args.out_dir / "receipt.json"
    out.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "receipt": str(out),
        "frame_count": receipt["frame_count"],
        "gvid": receipt["gvid_packaging"]["path"],
        "prores": receipt["prores_review"]["path"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
