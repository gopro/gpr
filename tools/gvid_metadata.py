#!/usr/bin/env python3
"""Build and validate companion source metadata for .gvid streams."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any

GVID_MAGIC = 0x44495647
FRAME_MAGIC = 0x004D5246
GVID_VERSION = 1
SCHEMA = "gvid_source_metadata.v1"
PAYLOAD_KINDS = {"fused_gpr", "camera_gpr"}


def read_gvid_frames(path: Path) -> list[dict[str, int]]:
    """Index a v1 stream with header-only reads and O(frame-count) memory."""
    frames: list[dict[str, int]] = []
    # Unbuffered reads avoid prefetching payload bytes when reading headers.
    with path.open("rb", buffering=0) as stream:
        stream.seek(0, 2)
        file_size = stream.tell()
        stream.seek(0)
        data = stream.read(32)
        if len(data) != 32:
            raise ValueError(f"{path} is too small to be a .gvid stream")
        header = struct.unpack("<IBBHHHIIIII", data)
        magic, version, flags, pixel_format, quality, reserved2 = header[:6]
        width, height, fps_x1000, target_kbps, frame_count_hint = header[6:]
        if magic != GVID_MAGIC:
            raise ValueError(f"{path} does not have GVID magic")
        if version != GVID_VERSION:
            raise ValueError(f"{path} has unsupported GVID version {version}")
        if flags & ~0x03:
            raise ValueError(f"{path} has unknown .gvid flag bits")
        if pixel_format > 5 or quality > 11:
            raise ValueError(f"{path} has unsupported pixel_format or quality")
        if reserved2 != 0:
            raise ValueError(f"{path} has nonzero reserved2")
        if width == 0 or height == 0 or fps_x1000 == 0:
            raise ValueError(f"{path} has zero dimensions or fps")
        if bool(flags & 0x01) != bool(target_kbps):
            raise ValueError(f"{path} rate-control flag and target_kbps disagree")

        pos = 32
        last_tag: int | None = None
        while pos < file_size:
            stream.seek(pos)
            data = stream.read(16)
            if len(data) != 16:
                raise ValueError(f"{path} has a truncated frame header at byte {pos}")
            magic, payload_size, frame_tag = struct.unpack("<IIQ", data)
            if magic != FRAME_MAGIC:
                raise ValueError(f"{path} has bad frame magic at byte {pos}")
            pos += 16
            if payload_size == 0:
                raise ValueError(f"{path} has a zero-size frame payload at byte {pos}")
            if payload_size > file_size - pos:
                raise ValueError(f"{path} has a truncated frame payload at byte {pos}")
            if last_tag is not None and frame_tag <= last_tag:
                raise ValueError(f"{path} has a non-monotonic frame tag {frame_tag}")
            frames.append({
                "frame_index": len(frames),
                "frame_tag": int(frame_tag),
                "payload_offset": pos,
                "payload_size": int(payload_size),
            })
            last_tag = frame_tag
            pos += payload_size
    if not frames:
        raise ValueError(f"{path} has a zero-frame .gvid stream")
    if frame_count_hint not in (0, len(frames)):
        raise ValueError(f"{path} frame_count_hint={frame_count_hint} but stream has {len(frames)} frames")
    return frames


def read_gvid_frame_tags(path: Path) -> list[int]:
    frames = read_gvid_frames(path)
    tags = [frame["frame_tag"] for frame in frames]
    return tags


def sha256_gvid_payload(path: Path, frame: dict[str, int]) -> str:
    h = hashlib.sha256()
    remaining = int(frame["payload_size"])
    with path.open("rb") as f:
        f.seek(int(frame["payload_offset"]))
        while remaining > 0:
            chunk = f.read(min(1024 * 1024, remaining))
            if not chunk:
                raise ValueError(f"{path} ended while hashing frame tag {frame['frame_tag']}")
            h.update(chunk)
            remaining -= len(chunk)
    return h.hexdigest()


def validate_against_gvid(meta: dict[str, Any], gvid: Path) -> None:
    tags = read_gvid_frame_tags(gvid)
    meta_tags = [int(frame["frame_tag"]) for frame in meta["frames"]]
    if len(tags) != len(meta_tags):
        raise ValueError(f"{gvid} has {len(tags)} frames but metadata has {len(meta_tags)} frames")
    if tags != meta_tags:
        raise ValueError(f"{gvid} frame tags {tags} do not match metadata frame tags {meta_tags}")


def verify_camera_gpr_payloads(meta: dict[str, Any], gvid: Path) -> dict[str, Any]:
    validate_metadata(meta)
    if meta.get("payload_kind", "fused_gpr") != "camera_gpr":
        raise ValueError("payload hash verification is only defined for payload_kind=camera_gpr")
    stream_frames = read_gvid_frames(gvid)
    meta_by_tag = {int(frame["frame_tag"]): frame for frame in meta["frames"]}
    failures = []
    checked = []
    for stream_frame in stream_frames:
        tag = int(stream_frame["frame_tag"])
        meta_frame = meta_by_tag.get(tag)
        if not meta_frame:
            failures.append({"frame_tag": tag, "error": "missing metadata"})
            continue
        expected_size = int(meta_frame["payload_bytes"])
        actual_size = int(stream_frame["payload_size"])
        expected_sha = str(meta_frame["payload_sha256"])
        actual_sha = sha256_gvid_payload(gvid, stream_frame)
        ok = expected_size == actual_size and expected_sha == actual_sha
        checked.append({
            "frame_tag": tag,
            "source_id": meta_frame["source_id"],
            "payload_size": actual_size,
            "sha256": actual_sha,
            "ok": ok,
        })
        if not ok:
            failures.append({
                "frame_tag": tag,
                "expected_size": expected_size,
                "actual_size": actual_size,
                "expected_sha256": expected_sha,
                "actual_sha256": actual_sha,
            })
    if len(stream_frames) != len(meta["frames"]):
        failures.append({
            "error": "frame count mismatch",
            "stream_frames": len(stream_frames),
            "metadata_frames": len(meta["frames"]),
        })
    if failures:
        raise ValueError(f"camera_gpr payload verification failed: {failures}")
    return {
        "schema": "gvid_camera_gpr_payload_verify.v1",
        "gvid": str(gvid),
        "frame_count": len(checked),
        "payload_bytes": sum(int(frame["payload_size"]) for frame in stream_frames),
        "all_exact": True,
        "frames": checked,
    }


def build_runtime_dispatch(meta: dict[str, Any], gvid: Path) -> dict[str, Any]:
    validate_metadata(meta)
    payload_kind = meta.get("payload_kind", "fused_gpr")
    stream_frames = read_gvid_frames(gvid)
    if len(stream_frames) != len(meta["frames"]):
        raise ValueError(f"{gvid} has {len(stream_frames)} frames but metadata has {len(meta['frames'])} frames")
    stream_tags = [int(frame["frame_tag"]) for frame in stream_frames]
    if len(stream_tags) != len(set(stream_tags)):
        raise ValueError(f"{gvid} has duplicate frame tags {stream_tags}")
    meta_by_tag = {int(frame["frame_tag"]): frame for frame in meta["frames"]}
    if len(meta_by_tag) != len(meta["frames"]):
        raise ValueError("metadata has duplicate frame tags")
    missing = [frame["frame_tag"] for frame in stream_frames if frame["frame_tag"] not in meta_by_tag]
    extra = sorted(set(meta_by_tag) - {frame["frame_tag"] for frame in stream_frames})
    if missing or extra:
        raise ValueError(f"runtime dispatch frame-tag mismatch missing={missing} extra={extra}")

    frames = []
    accepted_tiles = 0
    total_tiles = 0
    for stream_frame in stream_frames:
        meta_frame = meta_by_tag[stream_frame["frame_tag"]]
        frame = {
            **stream_frame,
            "source_id": meta_frame["source_id"],
            "source_path": meta_frame["source_path"],
            "iso": meta_frame["iso"],
            "payload_kind": payload_kind,
        }
        if payload_kind == "camera_gpr":
            if int(stream_frame["payload_size"]) != int(meta_frame["payload_bytes"]):
                raise ValueError(
                    f"frame tag {stream_frame['frame_tag']} payload size "
                    f"{stream_frame['payload_size']} does not match metadata "
                    f"{meta_frame['payload_bytes']}"
                )
            frame["payload_sha256"] = meta_frame["payload_sha256"]
            frame["payload_bytes"] = meta_frame["payload_bytes"]
        else:
            tiles = []
            for tile in meta_frame["raw_clean_tiles"]:
                accepted = bool(tile["accepted"])
                accepted_tiles += int(accepted)
                total_tiles += 1
                tiles.append({
                    "crop": tile["crop"],
                    "source_xywh": tile["source_xywh"],
                    "accepted": accepted,
                    "policy": "accepted_only_raw_clean" if accepted else "all_targets_raw_clean",
                    "reject_reasons": tile["reject_reasons"],
                    "sigma_rms_counts": tile["sigma_rms_counts"],
                })
            frame["raw_clean_tiles"] = tiles
        frames.append(frame)
    return {
        "schema": "gvid_runtime_dispatch.v1",
        "gvid": str(gvid),
        "metadata": meta.get("gvid"),
        "payload_kind": payload_kind,
        "frame_count": len(frames),
        "tile_count": total_tiles,
        "accepted_tile_count": accepted_tiles,
        "frames": frames,
    }


def load_crop_xywh(npz_path: Path) -> list[int]:
    import numpy as np

    z = np.load(npz_path, allow_pickle=False)
    if "crop_xywh" not in z:
        raise ValueError(f"{npz_path} does not contain crop_xywh")
    return [int(v) for v in z["crop_xywh"].tolist()]


def build_from_raw_clean_targets(targets: Path, out: Path, *, gvid: Path | None) -> dict[str, Any]:
    data = json.loads(targets.read_text())
    rows_by_image: dict[str, list[dict[str, Any]]] = {}
    for row in data["rows"]:
        rows_by_image.setdefault(row["image_id"], []).append(row)

    frames = []
    for frame_index, image_id in enumerate(rows_by_image):
        rows = rows_by_image[image_id]
        iso_values = {int(row["iso"]) for row in rows}
        if len(iso_values) != 1:
            raise ValueError(f"{image_id} has inconsistent ISO values {sorted(iso_values)}")
        tiles = []
        for row in rows:
            tiles.append({
                "crop": row["crop"],
                "source_xywh": load_crop_xywh(Path(row["npz"])),
                "accepted": bool(row.get("accepted", True)),
                "reject_reasons": list(row.get("reject_reasons", [])),
                "sigma_rms_counts": float(row["sigma_rms_counts"]),
                "exact_residual_to_sigma_rms": float(row["exact_residual_to_sigma_rms"]),
                "lag_max_abs": float(row["lag_max_abs"]),
                "edge_removed_energy_ratio": float(row["edge_removed_energy_ratio"]),
            })
        frames.append({
            "frame_index": frame_index,
            "frame_tag": frame_index,
            "source_id": image_id,
            "source_path": rows[0]["path"],
            "iso": next(iter(iso_values)),
            "raw_clean_tiles": tiles,
        })

    meta = {
        "schema": SCHEMA,
        "payload_kind": "fused_gpr",
        "source": "raw_clean_ref_targets",
        "targets": str(targets),
        "gvid": str(gvid) if gvid else None,
        "frame_count": len(frames),
        "frames": frames,
    }
    validate_metadata(meta)
    if gvid:
        validate_against_gvid(meta, gvid)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(meta, indent=2))
    return meta


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def build_from_native_gpr_sequence(
    frame_dir: Path,
    out: Path,
    *,
    gvid: Path | None,
    width: int,
    height: int,
    fps: float,
    iso: int,
) -> dict[str, Any]:
    frames_in = sorted(p for p in frame_dir.iterdir() if p.is_file() and p.suffix.lower() == ".gpr")
    if not frames_in:
        raise FileNotFoundError(f"no .gpr/.GPR frames found in {frame_dir}")
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    if fps <= 0.0:
        raise ValueError("fps must be positive")
    if iso < 0:
        raise ValueError("iso must be non-negative")

    frames = []
    for frame_index, path in enumerate(frames_in):
        frames.append({
            "frame_index": frame_index,
            "frame_tag": frame_index,
            "source_id": path.stem,
            "source_path": str(path),
            "iso": iso,
            "payload_bytes": path.stat().st_size,
            "payload_sha256": sha256_file(path),
        })

    meta = {
        "schema": SCHEMA,
        "payload_kind": "camera_gpr",
        "source": "native_camera_gpr_sequence",
        "frame_dir": str(frame_dir),
        "gvid": str(gvid) if gvid else None,
        "width": width,
        "height": height,
        "fps": fps,
        "frame_count": len(frames),
        "frames": frames,
    }
    validate_metadata(meta)
    if gvid:
        validate_against_gvid(meta, gvid)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def validate_metadata(meta: dict[str, Any]) -> None:
    if meta.get("schema") != SCHEMA:
        raise ValueError(f"unsupported metadata schema {meta.get('schema')!r}")
    payload_kind = meta.get("payload_kind", "fused_gpr")
    if payload_kind not in PAYLOAD_KINDS:
        raise ValueError(f"unsupported payload_kind {payload_kind!r}")
    frames = meta.get("frames")
    if not isinstance(frames, list):
        raise ValueError("frames must be a list")
    if int(meta.get("frame_count", -1)) != len(frames):
        raise ValueError("frame_count does not match frames length")
    seen_indices: set[int] = set()
    seen_tags: set[int] = set()
    for frame in frames:
        frame_index = int(frame["frame_index"])
        frame_tag = int(frame["frame_tag"])
        if frame_index in seen_indices:
            raise ValueError(f"duplicate frame_index {frame_index}")
        if frame_tag in seen_tags:
            raise ValueError(f"duplicate frame_tag {frame_tag}")
        seen_indices.add(frame_index)
        seen_tags.add(frame_tag)
        if frame_index < 0 or frame_tag < 0:
            raise ValueError("frame_index/frame_tag must be non-negative")
        if payload_kind == "camera_gpr":
            payload_bytes = int(frame["payload_bytes"])
            if payload_bytes <= 0:
                raise ValueError(f"frame {frame_index} payload_bytes must be positive")
            digest = str(frame["payload_sha256"])
            if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                raise ValueError(f"frame {frame_index} payload_sha256 is invalid")
        else:
            tiles = frame.get("raw_clean_tiles")
            if not isinstance(tiles, list):
                raise ValueError(f"frame {frame_index} raw_clean_tiles must be a list")
            for tile in tiles:
                xywh = tile.get("source_xywh")
                if not (isinstance(xywh, list) and len(xywh) == 4 and all(int(v) >= 0 for v in xywh)):
                    raise ValueError(f"frame {frame_index} tile {tile.get('crop')} has invalid source_xywh")
                if any(int(v) % 2 for v in xywh):
                    raise ValueError(f"frame {frame_index} tile {tile.get('crop')} source_xywh is not CFA aligned")
                if not isinstance(tile.get("accepted"), bool):
                    raise ValueError(f"frame {frame_index} tile {tile.get('crop')} accepted must be bool")
                if not isinstance(tile.get("reject_reasons"), list):
                    raise ValueError(f"frame {frame_index} tile {tile.get('crop')} reject_reasons must be list")
                for key in (
                    "sigma_rms_counts",
                    "exact_residual_to_sigma_rms",
                    "lag_max_abs",
                    "edge_removed_energy_ratio",
                ):
                    value = float(tile[key])
                    if not math.isfinite(value):
                        raise ValueError(f"frame {frame_index} tile {tile.get('crop')} {key} is not finite")


def cmd_from_targets(args: argparse.Namespace) -> int:
    meta = build_from_raw_clean_targets(args.targets, args.output, gvid=args.gvid)
    accepted = sum(
        1
        for frame in meta["frames"]
        for tile in frame["raw_clean_tiles"]
        if tile["accepted"]
    )
    total = sum(len(frame["raw_clean_tiles"]) for frame in meta["frames"])
    print(args.output)
    print(f"frames={meta['frame_count']} tiles={total} accepted_tiles={accepted}")
    return 0


def cmd_from_native_gpr_sequence(args: argparse.Namespace) -> int:
    meta = build_from_native_gpr_sequence(
        args.frame_dir,
        args.output,
        gvid=args.gvid,
        width=args.width,
        height=args.height,
        fps=args.fps,
        iso=args.iso,
    )
    payload_bytes = sum(int(frame["payload_bytes"]) for frame in meta["frames"])
    print(args.output)
    print(f"frames={meta['frame_count']} payload_bytes={payload_bytes}")
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    meta = json.loads(args.metadata.read_text())
    validate_metadata(meta)
    if args.gvid:
        validate_against_gvid(meta, args.gvid)
    print(f"{args.metadata}: PASS")
    return 0


def cmd_runtime_dispatch(args: argparse.Namespace) -> int:
    meta = json.loads(args.metadata.read_text())
    dispatch = build_runtime_dispatch(meta, args.gvid)
    text = json.dumps(dispatch, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(args.output)
    else:
        print(text, end="")
    print(
        f"frames={dispatch['frame_count']} tiles={dispatch['tile_count']} "
        f"accepted_tiles={dispatch['accepted_tile_count']}",
        file=sys.stderr,
    )
    return 0


def cmd_verify_payloads(args: argparse.Namespace) -> int:
    meta = json.loads(args.metadata.read_text())
    receipt = verify_camera_gpr_payloads(meta, args.gvid)
    text = json.dumps(receipt, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(args.output)
    else:
        print(text, end="")
    print(f"frames={receipt['frame_count']} all_exact={receipt['all_exact']}", file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build and validate .gvid source metadata sidecars.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    build = sub.add_parser("from-raw-clean-targets")
    build.add_argument("targets", type=Path)
    build.add_argument("output", type=Path)
    build.add_argument("--gvid", type=Path)
    build.set_defaults(func=cmd_from_targets)

    native = sub.add_parser("from-native-gpr-sequence")
    native.add_argument("frame_dir", type=Path)
    native.add_argument("output", type=Path)
    native.add_argument("--gvid", type=Path)
    native.add_argument("--width", type=int, required=True)
    native.add_argument("--height", type=int, required=True)
    native.add_argument("--fps", type=float, default=24.0)
    native.add_argument("--iso", type=int, default=0)
    native.set_defaults(func=cmd_from_native_gpr_sequence)

    validate = sub.add_parser("validate")
    validate.add_argument("metadata", type=Path)
    validate.add_argument("--gvid", type=Path)
    validate.set_defaults(func=cmd_validate)

    runtime = sub.add_parser("runtime-dispatch")
    runtime.add_argument("metadata", type=Path)
    runtime.add_argument("--gvid", type=Path, required=True)
    runtime.add_argument("--output", type=Path)
    runtime.set_defaults(func=cmd_runtime_dispatch)

    verify_payloads = sub.add_parser("verify-payloads")
    verify_payloads.add_argument("metadata", type=Path)
    verify_payloads.add_argument("--gvid", type=Path, required=True)
    verify_payloads.add_argument("--output", type=Path)
    verify_payloads.set_defaults(func=cmd_verify_payloads)

    args = ap.parse_args(argv)
    try:
        return int(args.func(args))
    except Exception as exc:
        print(f"gvid_metadata: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
