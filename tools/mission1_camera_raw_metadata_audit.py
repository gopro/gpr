#!/usr/bin/env python3
"""Audit Mission 1 DNG/GPR metadata needed for raw-renderer compatibility."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


RENDER_REQUIRED = [
    "Make",
    "Model",
    "UniqueCameraModel",
    "LocalizedCameraModel",
    "DNGVersion",
    "DNGBackwardVersion",
    "PhotometricInterpretation",
    "CFARepeatPatternDim",
    "CFAPattern2",
    "CFAPlaneColor",
    "CFALayout",
    "BlackLevelRepeatDim",
    "BlackLevel",
    "WhiteLevel",
    "DefaultCropOrigin",
    "DefaultCropSize",
    "ActiveArea",
    "ColorMatrix1",
    "ColorMatrix2",
    "CalibrationIlluminant1",
    "CalibrationIlluminant2",
    "AnalogBalance",
    "AsShotNeutral",
    "BaselineExposure",
    "BaselineNoise",
    "BaselineSharpness",
    "LinearResponseLimit",
    "ProfileName",
    "ProfileEmbedPolicy",
    "NoiseProfile",
]

MISSION1_STRONGLY_RECOMMENDED = [
    "OpcodeList2",
    "RawDataUniqueID",
]

GOPRO_JPEG_LOOK_TAGS = [
    "ColorMode",
    "Sharpness",
    "WhiteBalance",
    "HDRSetting",
    "LensProjection",
    "FieldOfView",
]


def run_exiftool(paths: list[Path]) -> list[dict[str, Any]]:
    cmd = ["exiftool", "-a", "-G1", "-s", "-j", *[str(p) for p in paths]]
    result = subprocess.run(cmd, text=True, capture_output=True, check=False)
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stderr.strip() or "exiftool failed")
    if not result.stdout.strip():
        return []
    return json.loads(result.stdout)


def normalize_tags(raw: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    priority: dict[str, int] = {}
    for key, value in raw.items():
        if key in {"SourceFile", "ExifTool:Error", "Error"}:
            out[key] = value
            continue
        group, base = key.rsplit(":", 1) if ":" in key else ("", key)
        group_priority = {"GoPro": 5, "SubIFD": 4, "IFD0": 3, "ExifIFD": 2}.get(group, 1)
        if group_priority >= priority.get(base, 0):
            out[base] = value
            priority[base] = group_priority
    return out


def compare_to_reference(
    candidate: dict[str, Any], reference: dict[str, Any], tags: list[str]
) -> list[dict[str, Any]]:
    diffs: list[dict[str, Any]] = []
    for tag in tags:
        cval = candidate.get(tag)
        rval = reference.get(tag)
        if cval != rval:
            diffs.append({"tag": tag, "candidate": cval, "reference": rval})
    return diffs


def audit_candidate(candidate: dict[str, Any], reference: dict[str, Any]) -> dict[str, Any]:
    error = candidate.get("ExifTool:Error") or candidate.get("Error")
    if error:
        return {
            "source": candidate.get("SourceFile"),
            "readable_by_exiftool": False,
            "error": error,
            "missing_required": RENDER_REQUIRED,
            "missing_recommended": MISSION1_STRONGLY_RECOMMENDED,
            "diffs_from_reference": [],
        }

    missing_required = [tag for tag in RENDER_REQUIRED if tag not in candidate]
    missing_recommended = [
        tag for tag in MISSION1_STRONGLY_RECOMMENDED if tag not in candidate
    ]
    comparable = [
        "Make",
        "Model",
        "UniqueCameraModel",
        "LocalizedCameraModel",
        "ColorMatrix1",
        "ColorMatrix2",
        "AsShotNeutral",
        "BaselineExposure",
        "BlackLevel",
        "WhiteLevel",
        "DefaultCropSize",
        "ActiveArea",
        "OpcodeList2",
        "NoiseProfile",
    ]
    return {
        "source": candidate.get("SourceFile"),
        "readable_by_exiftool": True,
        "missing_required": missing_required,
        "missing_recommended": missing_recommended,
        "diffs_from_reference": compare_to_reference(candidate, reference, comparable),
    }


def write_markdown(report: dict[str, Any], path: Path) -> None:
    lines = [
        "# Mission 1 Camera Raw Metadata Audit",
        "",
        f"Reference DNG: `{report['reference_dng']}`",
        "",
        "## Reference Tags",
        "",
    ]
    ref = report["reference_tags"]
    for tag in RENDER_REQUIRED + MISSION1_STRONGLY_RECOMMENDED:
        if tag in ref:
            lines.append(f"- `{tag}`: `{ref[tag]}`")
    if report.get("camera_jpeg_tags"):
        lines += ["", "## GoPro JPEG Look Tags", ""]
        for tag, value in report["camera_jpeg_tags"].items():
            if tag in GOPRO_JPEG_LOOK_TAGS:
                lines.append(f"- `{tag}`: `{value}`")
    lines += ["", "## Candidates", ""]
    for item in report["candidates"]:
        lines.append(f"### `{item['source']}`")
        lines.append("")
        lines.append(f"- readable by ExifTool: `{item['readable_by_exiftool']}`")
        if item.get("error"):
            lines.append(f"- error: `{item['error']}`")
        lines.append(f"- missing required: `{', '.join(item['missing_required']) or 'none'}`")
        lines.append(
            f"- missing recommended: `{', '.join(item['missing_recommended']) or 'none'}`"
        )
        if item["diffs_from_reference"]:
            lines.append("- differing reference tags:")
            for diff in item["diffs_from_reference"]:
                candidate = diff["candidate"]
                reference = diff["reference"]
                lines.append(
                    f"  - `{diff['tag']}`: candidate=`{candidate}`, reference=`{reference}`"
                )
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-dng", required=True, type=Path)
    parser.add_argument("--camera-jpeg", type=Path)
    parser.add_argument("--candidate", action="append", type=Path, default=[])
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--md-out", type=Path)
    args = parser.parse_args()

    paths = [args.reference_dng]
    if args.camera_jpeg:
        paths.append(args.camera_jpeg)
    paths += args.candidate
    records = run_exiftool(paths)
    by_source = {Path(r["SourceFile"]).resolve(): normalize_tags(r) for r in records}

    reference = by_source[args.reference_dng.resolve()]
    camera_jpeg = by_source.get(args.camera_jpeg.resolve(), {}) if args.camera_jpeg else {}
    report = {
        "reference_dng": str(args.reference_dng),
        "reference_tags": {
            tag: reference.get(tag)
            for tag in RENDER_REQUIRED + MISSION1_STRONGLY_RECOMMENDED
            if tag in reference
        },
        "camera_jpeg": str(args.camera_jpeg) if args.camera_jpeg else None,
        "camera_jpeg_tags": {
            tag: camera_jpeg.get(tag)
            for tag in GOPRO_JPEG_LOOK_TAGS
            if tag in camera_jpeg
        },
        "candidates": [
            audit_candidate(by_source[c.resolve()], reference) for c in args.candidate
        ],
    }

    if args.json_out:
        args.json_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    else:
        print(json.dumps(report, indent=2))
    if args.md_out:
        write_markdown(report, args.md_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
