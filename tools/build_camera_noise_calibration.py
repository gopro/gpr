#!/usr/bin/env python3
"""Build a camera-noise calibration receipt from raw Bayer darkframes.

This produces the `gpr.camera_noise_calibration.v1` sidecar used by the
product-pillar guard. It intentionally works in raw Bayer space and only marks
the receipt usable for training targets when the source is a darkframe-like
stack with enough frames to separate stochastic noise from scene signal.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr.camera_noise_calibration.v1"
PLANE_ORDER = ("r", "g1", "b", "g2")
PHASE_TO_GRID = {
    "RGGB": (("r", "g1"), ("g2", "b")),
    "GRBG": (("g1", "r"), ("b", "g2")),
    "GBRG": (("g1", "b"), ("r", "g2")),
    "BGGR": (("b", "g1"), ("g2", "r")),
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def stack_sha256(paths: list[Path], metadata: dict[str, Any]) -> str:
    h = hashlib.sha256()
    h.update(json.dumps(metadata, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    for path in paths:
        h.update(path.name.encode("utf-8"))
        h.update(bytes.fromhex(sha256_file(path)))
    return h.hexdigest()


def load_source_provenance_manifest(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    with path.open("r", encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"{path} did not contain a JSON object")
    return data


def source_provenance_index(manifest: dict[str, Any] | None) -> dict[str, dict[str, Any]]:
    if not manifest:
        return {}
    rows = manifest.get("frames") or manifest.get("rows") or []
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        for field in ("raw_path", "path", "source_path"):
            raw_path = row.get(field)
            if not raw_path:
                continue
            path = Path(str(raw_path))
            keys = {path.as_posix(), path.name}
            if path.is_absolute():
                keys.add(str(path.resolve()))
            for key in keys:
                result[key] = row
    return result


def provenance_for_raw(path: Path, index: dict[str, dict[str, Any]]) -> dict[str, Any] | None:
    keys = [path.as_posix(), path.name]
    if path.is_absolute():
        keys.append(str(path.resolve()))
    for key in keys:
        if key in index:
            return index[key]
    return None


def valid_sha256(text: Any) -> bool:
    sha = str(text or "")
    return len(sha) == 64 and all(ch in "0123456789abcdefABCDEF" for ch in sha)


def source_frame_rows(paths: list[Path], manifest: dict[str, Any] | None, require_provenance: bool) -> tuple[list[dict[str, Any]], bool]:
    index = source_provenance_index(manifest)
    frames: list[dict[str, Any]] = []
    all_ready = True
    for path in paths:
        raw_sha = sha256_file(path)
        row = provenance_for_raw(path, index)
        failure: str | None = None
        original_path = None
        original_sha = None
        extract_receipt = None
        capture_setup = None
        no_scene_signal = None
        if row is None:
            failure = "missing provenance row"
        else:
            original_path = row.get("original_path") or row.get("original_raw_path") or row.get("source_dng") or row.get("source_path")
            original_sha = row.get("original_sha256") or row.get("source_sha256")
            extract_receipt = row.get("extract_receipt") or row.get("extraction_receipt")
            capture_setup = row.get("capture_setup") or row.get("proof")
            no_scene_signal = row.get("no_scene_signal")
            manifest_raw_sha = row.get("raw_sha256") or row.get("sha256")
            if not valid_sha256(manifest_raw_sha):
                failure = "raw sha256 missing or invalid"
            elif str(manifest_raw_sha).lower() != raw_sha.lower():
                failure = "raw sha256 does not match file contents"
            elif no_scene_signal is not True:
                failure = "no_scene_signal is not true"
            elif not str(capture_setup or "").strip():
                failure = "capture_setup/proof is missing"
            elif not str(extract_receipt or "").strip():
                failure = "extract_receipt is missing"
            elif not str(original_path or "").strip():
                failure = "original raw path is missing"
            elif not valid_sha256(original_sha):
                failure = "original source sha256 missing or invalid"
        ready = failure is None
        all_ready = all_ready and ready
        frames.append(
            {
                "raw_path": path.as_posix(),
                "raw_sha256": raw_sha,
                "original_path": original_path,
                "original_sha256": original_sha,
                "extract_receipt": extract_receipt,
                "no_scene_signal": bool(no_scene_signal) if no_scene_signal is not None else False,
                "capture_setup": capture_setup,
                "source_provenance_ready": ready,
                "source_provenance_failure": failure,
            }
        )
    if require_provenance and not all_ready:
        failures = [f"{row['raw_path']}: {row['source_provenance_failure']}" for row in frames if not row["source_provenance_ready"]]
        raise ValueError("source provenance is required: " + "; ".join(failures))
    return frames, all_ready


class RunningStats:
    def __init__(self) -> None:
        self.count = 0
        self.total = 0.0
        self.total_sq = 0.0

    def update(self, values: array.array) -> None:
        for value in values:
            fv = float(value)
            self.count += 1
            self.total += fv
            self.total_sq += fv * fv

    @property
    def mean(self) -> float:
        return self.total / self.count if self.count else 0.0

    @property
    def sigma(self) -> float:
        if self.count <= 1:
            return 0.0
        variance = (self.total_sq - (self.total * self.total / self.count)) / (self.count - 1)
        return math.sqrt(max(variance, 0.0))


def read_raw_u16(path: Path, width: int, height: int) -> array.array:
    arr = array.array("H")
    arr.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        arr.byteswap()
    expected = width * height
    if len(arr) != expected:
        raise ValueError(f"{path} has {len(arr)} uint16 samples, expected {expected}")
    return arr


def update_plane_stats(stats: dict[str, RunningStats], frame: array.array, width: int, height: int, cfa_phase: str) -> None:
    grid = PHASE_TO_GRID[cfa_phase]
    for row in range(height):
        row_base = row * width
        row_grid = grid[row & 1]
        stats[row_grid[0]].update(frame[row_base : row_base + width : 2])
        stats[row_grid[1]].update(frame[row_base + 1 : row_base + width : 2])


def summarize_plane(stats: RunningStats, black_level: float, raw_range: float) -> dict[str, float]:
    sigma = stats.sigma
    mean = stats.mean
    offset = (sigma / max(raw_range, 1.0)) ** 2
    return {
        "noise_profile_scale": 0.0,
        "noise_profile_offset": offset,
        "mean_black": mean,
        "sigma_black": sigma,
        "mean_black_delta": mean - black_level,
    }


def build_receipt(args: argparse.Namespace) -> dict[str, Any]:
    paths = [Path(p) for p in args.raw]
    if not paths:
        raise ValueError("at least one --raw file is required")
    for path in paths:
        if not path.exists():
            raise FileNotFoundError(path)

    raw_range = float(args.white_level - args.black_level)
    if raw_range <= 0:
        raise ValueError("--white-level must be greater than --black-level")

    by_plane: dict[str, RunningStats] = {plane: RunningStats() for plane in PLANE_ORDER}
    for path in paths:
        update_plane_stats(by_plane, read_raw_u16(path, args.width, args.height), args.width, args.height, args.cfa_phase)

    source_manifest = load_source_provenance_manifest(args.source_provenance_manifest)
    source_frames, source_provenance_ready = source_frame_rows(paths, source_manifest, args.require_source_provenance)
    per_plane = {
        plane: summarize_plane(stats, args.black_level, raw_range)
        for plane, stats in by_plane.items()
    }

    source_kind = args.source_kind
    separates_noise = source_kind in {"darkframes", "flat_dark_pair"} and len(paths) >= 4 and (
        source_provenance_ready or not args.require_source_provenance
    )
    usable = separates_noise
    if separates_noise and args.require_source_provenance:
        audit_evidence = (
            "darkframes contain no scene signal, the stack has at least four frames, "
            "and strict source provenance passed"
        )
    elif separates_noise:
        audit_evidence = "darkframes contain no scene signal and the stack has at least four frames"
    else:
        audit_evidence = "not enough darkframe evidence to separate stochastic noise from scene signal"
    metadata = {
        "width": args.width,
        "height": args.height,
        "bit_depth": args.bit_depth,
        "cfa_phase": args.cfa_phase,
        "iso": args.iso,
        "source_kind": source_kind,
        "sample_count": len(paths),
    }
    source_hash = stack_sha256(paths, metadata)
    source_path = paths[0].as_posix() if len(paths) == 1 else "raw_stack:" + ",".join(p.name for p in paths)

    return {
        "schema": SCHEMA,
        "camera": {
            "make": args.make,
            "model": args.model,
            "width": args.width,
            "height": args.height,
            "bit_depth": args.bit_depth,
            "cfa_phase": args.cfa_phase,
            "black_level": float(args.black_level),
            "white_level": float(args.white_level),
        },
        "calibrations": [
            {
                "iso": args.iso,
                "calibration_method": "darkframe_stack_per_plane_sigma_v1",
                "source_kind": source_kind,
                "sample_count": len(paths),
                "source": {
                    "path": source_path,
                    "sha256": source_hash,
                    "frame_count": len(source_frames),
                    "frames": source_frames,
                    "source_provenance_manifest": args.source_provenance_manifest.as_posix()
                    if args.source_provenance_manifest
                    else None,
                },
                "per_plane": per_plane,
                "noise_signal_audit": {
                    "separates_noise_from_signal": separates_noise,
                    "method": "darkframe_stack_sigma" if separates_noise else "insufficient_darkframe_stack",
                    "evidence": audit_evidence,
                    "source_provenance_required": bool(args.require_source_provenance),
                    "source_provenance_ready": bool(source_provenance_ready),
                    "source_provenance_manifest_present": source_manifest is not None,
                },
                "usable_for_training_targets": usable,
            }
        ],
        "production_ready": usable,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--raw", action="append", required=True, help="little-endian uint16 raw Bayer darkframe")
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--bit-depth", type=int, required=True)
    ap.add_argument("--cfa-phase", choices=sorted(PHASE_TO_GRID), required=True)
    ap.add_argument("--iso", type=int, required=True)
    ap.add_argument("--make", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--black-level", type=float, required=True)
    ap.add_argument("--white-level", type=float, required=True)
    ap.add_argument("--source-kind", choices=("darkframes", "flat_dark_pair"), default="darkframes")
    ap.add_argument(
        "--source-provenance-manifest",
        type=Path,
        help="Optional JSON with per-frame raw_path/raw_sha256/original_path/original_sha256/extract_receipt/no_scene_signal/capture_setup.",
    )
    ap.add_argument(
        "--require-source-provenance",
        action="store_true",
        help="Refuse production-ready output unless every raw frame has verified no-scene-signal source provenance.",
    )
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    try:
        receipt = build_receipt(args)
    except Exception as exc:
        print(f"build_camera_noise_calibration: {exc}", file=sys.stderr)
        return 2

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
