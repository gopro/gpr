#!/usr/bin/env python3
"""Repack a Bayer raw with source-camera metadata and audit the result."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import time
from pathlib import Path
from typing import Any


SCHEMA = "gpr.source_metadata_repack_receipt.v1"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: list[str], log: Path) -> str:
    proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
    text = proc.stdout + proc.stderr
    log.write_text(text, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"command failed ({proc.returncode}): {' '.join(cmd)}\n{text[-2000:]}")
    return proc.stdout


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


def candidate_passed(
    row: dict[str, Any],
    *,
    allowed_missing_recommended: set[str],
    allowed_diff_tags: set[str],
) -> bool:
    missing_required = row.get("missing_required") if isinstance(row.get("missing_required"), list) else []
    missing_recommended = row.get("missing_recommended") if isinstance(row.get("missing_recommended"), list) else []
    diffs = row.get("diffs_from_reference") if isinstance(row.get("diffs_from_reference"), list) else []
    diff_tags = {
        str(item.get("tag"))
        for item in diffs
        if isinstance(item, dict) and isinstance(item.get("tag"), str)
    }
    return (
        row.get("readable_by_exiftool") is True
        and missing_required == []
        and {str(item) for item in missing_recommended} <= allowed_missing_recommended
        and diff_tags <= allowed_diff_tags
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-dng", type=Path, required=True)
    ap.add_argument("--candidate-raw", type=Path, required=True)
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--external-root", type=Path, default=Path(os.environ.get("GPR_EXTERNAL_ROOT", Path(__file__).resolve().parents[1])))
    ap.add_argument("--gpr-tools", type=Path, default=Path("build-local/source/app/gpr_tools/gpr_tools"))
    ap.add_argument("--metadata-audit-tool", type=Path, default=Path("tools/mission1_camera_raw_metadata_audit.py"))
    ap.add_argument("--camera-jpeg", type=Path)
    ap.add_argument("--stem", default="frame_000000_sr8k_source_meta")
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--pitch", type=int)
    ap.add_argument("--pixel-format", default="rggb14")
    ap.add_argument("--allowed-missing-recommended", action="append", default=["OpcodeList2", "RawDataUniqueID"])
    ap.add_argument("--allowed-diff-tag", action="append", default=["AsShotNeutral", "ActiveArea", "NoiseProfile"])
    args = ap.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    pitch = args.pitch if args.pitch is not None else args.width * 2
    params = args.output_dir / "source_metadata_params.json"
    dng = args.output_dir / f"{args.stem}.dng"
    gpr = args.output_dir / f"{args.stem}.gpr"
    audit_json = args.output_dir / "metadata_transplant_audit.json"
    audit_md = args.output_dir / "metadata_transplant_audit.md"

    params.write_text(
        run([str(args.gpr_tools), "-i", str(args.source_dng), "-d", "1"], args.output_dir / "source_metadata_dump.log"),
        encoding="utf-8",
    )
    common = [
        str(args.gpr_tools),
        "-i",
        str(args.candidate_raw),
        "-w",
        str(args.width),
        "-h",
        str(args.height),
        "-p",
        str(pitch),
        "-x",
        args.pixel_format,
        "-a",
        str(params),
    ]
    run([*common, "-o", str(dng)], args.output_dir / "raw_to_source_meta_dng.log")
    run([*common, "-o", str(gpr)], args.output_dir / "raw_to_source_meta_gpr.log")

    audit_cmd = [
        "python3",
        str(args.metadata_audit_tool),
        "--reference-dng",
        str(args.source_dng),
    ]
    if args.camera_jpeg:
        audit_cmd += ["--camera-jpeg", str(args.camera_jpeg)]
    audit_cmd += [
        "--candidate",
        str(dng),
        "--candidate",
        str(gpr),
        "--json-out",
        str(audit_json),
        "--md-out",
        str(audit_md),
    ]
    run(audit_cmd, args.output_dir / "metadata_audit.log")

    audit = json.loads(audit_json.read_text(encoding="utf-8"))
    allowed_missing = {str(item) for item in args.allowed_missing_recommended}
    allowed_diffs = {str(item) for item in args.allowed_diff_tag}
    passed = bool(audit.get("candidates")) and all(
        candidate_passed(
            row,
            allowed_missing_recommended=allowed_missing,
            allowed_diff_tags=allowed_diffs,
        )
        for row in audit.get("candidates", [])
        if isinstance(row, dict)
    )
    receipt = {
        "schema": SCHEMA,
        "created_unix": int(time.time()),
        "production_ready": passed,
        "source_dng": file_record(args.source_dng, args.external_root),
        "candidate_raw": file_record(args.candidate_raw, args.external_root),
        "candidate_dng": file_record(dng, args.external_root),
        "candidate_gpr": file_record(gpr, args.external_root),
        "metadata_audit": file_record(audit_json, args.external_root),
        "metadata_audit_md": file_record(audit_md, args.external_root),
        "width": args.width,
        "height": args.height,
        "pitch": pitch,
        "pixel_format": args.pixel_format,
        "allowed_missing_recommended": sorted(allowed_missing),
        "allowed_diff_tags": sorted(allowed_diffs),
    }
    receipt_path = args.output_dir / "metadata_repack_receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"receipt": str(receipt_path), "candidate_dng": str(dng), "passed": passed}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
