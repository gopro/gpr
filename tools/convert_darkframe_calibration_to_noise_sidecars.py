#!/usr/bin/env python3
"""Convert legacy darkframe calibration output to v1 noise sidecars.

The legacy `darkframe_calibration.json` artifacts contain useful grouped
darkframe statistics, but they predate `gpr.camera_noise_calibration.v1`.
This converter rebuilds the source-frame manifest from discovery rows, hashes
the selected source frames, and emits one validated sidecar per camera/ISO
group.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr.camera_noise_calibration.v1"
SITE_TO_PLANE = {"R00": "r", "G01": "g1", "B11": "b", "G10": "g2"}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_json(path: Path, payload: dict[str, Any]) -> dict[str, str]:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return {"path": path.as_posix(), "sha256": sha256_file(path)}


def safe_slug(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("_")


def cfa_phase(row: dict[str, Any]) -> str:
    pattern = row.get("pattern")
    color_desc = str(row.get("color_desc") or "RGBG")
    if not (
        isinstance(pattern, list)
        and len(pattern) == 2
        and all(isinstance(r, list) and len(r) == 2 for r in pattern)
    ):
        raise ValueError(f"unsupported CFA pattern in {row.get('path')}: {pattern!r}")

    phase = ""
    for y in range(2):
        for x in range(2):
            idx = int(pattern[y][x])
            if idx < 0 or idx >= len(color_desc):
                raise ValueError(f"CFA index {idx} outside color_desc={color_desc!r}")
            color = color_desc[idx].upper()
            phase += "G" if color == "G" else color
    if phase not in {"RGGB", "GBRG", "GRBG", "BGGR"}:
        raise ValueError(f"unsupported normal Bayer phase {phase!r}")
    return phase


def group_rows(data: dict[str, Any], group: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for row in data.get("discovery_rows", []):
        if not row.get("dark_candidate"):
            continue
        if row.get("make") != group.get("make"):
            continue
        if row.get("model") != group.get("model"):
            continue
        if int(row.get("iso", -1)) != int(group.get("iso", -2)):
            continue
        if float(row.get("exposure_time", -1.0)) != float(group.get("exposure_time", -2.0)):
            continue
        rows.append(row)
    return rows[: int(group["frame_count"])]


def source_manifest(
    *,
    legacy_path: Path,
    group: dict[str, Any],
    rows: list[dict[str, Any]],
    out_path: Path,
) -> dict[str, str]:
    files = []
    for row in rows:
        path = Path(str(row["path"]))
        files.append({"path": path.as_posix(), "sha256": sha256_file(path)})
    payload = {
        "kind": "gpr.camera_noise_source_manifest.v1",
        "legacy_calibration": {"path": legacy_path.as_posix(), "sha256": sha256_file(legacy_path)},
        "selection_policy": "first matching dark_candidate discovery rows by make/model/ISO/exposure",
        "camera": {
            "make": group["make"],
            "model": group["model"],
            "iso": int(group["iso"]),
            "exposure_time": float(group["exposure_time"]),
        },
        "source_files": files,
    }
    return write_json(out_path, payload)


def build_sidecar(
    *,
    legacy_path: Path,
    data: dict[str, Any],
    group: dict[str, Any],
    rows: list[dict[str, Any]],
    out_dir: Path,
) -> Path:
    if not rows:
        raise ValueError(f"no matching source rows for {group.get('key')}")

    first = rows[0]
    phase = cfa_phase(first)
    height, width = [int(v) for v in group["raw_shape"]]
    white = float(first.get("white") or first.get("WhiteLevel") or 0.0)
    if white <= 0:
        raise ValueError(f"missing white level for {group.get('key')}")
    black_by_site = first.get("black_by_site")
    if not isinstance(black_by_site, dict):
        raise ValueError(f"missing black_by_site for {group.get('key')}")
    black_mean = sum(float(v) for v in black_by_site.values()) / max(len(black_by_site), 1)
    raw_range = max(white - black_mean, 1.0)

    slug = safe_slug(f"{group['make']}_{group['model']}_ISO{group['iso']}_exp{group['exposure_time']}")
    manifest_ref = source_manifest(
        legacy_path=legacy_path,
        group=group,
        rows=rows,
        out_path=out_dir / f"{slug}_source_manifest.json",
    )

    per_plane: dict[str, Any] = {}
    for site, plane in SITE_TO_PLANE.items():
        stats = group["per_site"][site]
        black = float(black_by_site.get(site, black_mean))
        sigma = float(stats["temporal_noise_rms_counts"])
        per_plane[plane] = {
            "noise_profile_scale": 0.0,
            "noise_profile_offset": (sigma / raw_range) ** 2,
            "mean_black": black + float(stats.get("mean_residual_counts", 0.0)),
            "sigma_black": sigma,
            "temporal_noise_p95_counts": float(stats.get("temporal_noise_p95_counts", 0.0)),
            "spatial_fpn_rms_counts": float(stats.get("spatial_fpn_rms_counts", 0.0)),
            "row_fpn_rms_counts": float(stats.get("row_fpn_rms_counts", 0.0)),
            "col_fpn_rms_counts": float(stats.get("col_fpn_rms_counts", 0.0)),
        }

    artifacts = {}
    for key, value in (group.get("artifacts") or {}).items():
        path = Path(str(value))
        if path.exists():
            artifacts[key] = {"path": path.as_posix(), "sha256": sha256_file(path)}

    payload = {
        "schema": SCHEMA,
        "camera": {
            "make": group["make"],
            "model": group["model"],
            "width": width,
            "height": height,
            "bit_depth": 16 if white > 16383 else 14,
            "cfa_phase": phase,
            "black_level": black_mean,
            "white_level": white,
        },
        "calibrations": [
            {
                "iso": int(group["iso"]),
                "calibration_method": "legacy_darkframe_group_temporal_noise_v1",
                "source_kind": "darkframes",
                "sample_count": len(rows),
                "source": manifest_ref,
                "per_plane": per_plane,
                "noise_signal_audit": {
                    "separates_noise_from_signal": True,
                    "method": "darkframe_stack_temporal_noise",
                    "evidence": "selected source files are dark_candidate frames from the legacy calibration scan and contain no scene signal",
                },
                "usable_for_training_targets": True,
                "exposure_time": float(group["exposure_time"]),
                "available_candidate_count": int(group.get("available_candidate_count", len(rows))),
                "calibration_stride": int(group.get("calibration_stride", 1)),
                "legacy_artifacts": artifacts,
            }
        ],
        "production_ready": True,
    }

    out_path = out_dir / f"{slug}_noise_calibration.json"
    write_json(out_path, payload)
    return out_path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--legacy-json", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--min-frames", type=int, default=4)
    args = ap.parse_args()

    data = json.loads(args.legacy_json.read_text(encoding="utf-8"))
    if data.get("kind") != "darkframe_calibration":
        print(f"{args.legacy_json}: expected kind=darkframe_calibration", file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    outputs = []
    try:
        for group in data.get("calibration_groups", []):
            rows = group_rows(data, group)
            if len(rows) < args.min_frames:
                raise ValueError(f"{group.get('key')} has only {len(rows)} matching rows")
            outputs.append(build_sidecar(legacy_path=args.legacy_json, data=data, group=group, rows=rows, out_dir=args.out_dir))
    except Exception as exc:
        print(f"convert_darkframe_calibration_to_noise_sidecars: {exc}", file=sys.stderr)
        return 2

    index = {
        "kind": "gpr.camera_noise_calibration_index.v1",
        "legacy_json": {"path": args.legacy_json.as_posix(), "sha256": sha256_file(args.legacy_json)},
        "sidecars": [{"path": p.as_posix(), "sha256": sha256_file(p)} for p in outputs],
    }
    index_path = args.out_dir / "camera_noise_calibration_index.json"
    index_path.write_text(json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(index_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
