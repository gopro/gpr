#!/usr/bin/env python3
"""Verify a portable Labs artifact bundle manifest.

The bundle itself lives outside git. This verifier is intentionally dependency
light: it checks file presence, byte sizes, SHA-256 hashes, JSON readability,
and the `.gvid` v1 stream contract without importing NumPy or project-private
media tooling.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr_labs_bundle.v1"
REQUIRED_PRODUCT_PILLARS = {
    "raw_stills": "RAW stills",
    "raw_video_mvp": "RAW video MVP",
    "premium_still_sr": "Premium still/SR",
    "raw_video_reconstruction": "RAW video reconstruction",
}
GVID_CLIP_MAGIC = 0x44495647
GVID_FRAME_MAGIC = 0x004D5246
GVID_VERSION = 1
GVID_FLAG_MASK = 0x03
GVID_QUALITY_MAX = 11
GVID_PIXEL_FORMAT_MAX = 5
CLIP_HEADER_SIZE = 32
FRAME_HEADER_SIZE = 16


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def safe_child(root: Path, value: str) -> Path:
    rel = Path(value)
    if rel.is_absolute() or any(part in {"", ".", ".."} for part in rel.parts):
        raise ValueError(f"unsafe bundle path: {value}")
    return root / rel


def validate_gvid(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < CLIP_HEADER_SIZE:
        raise ValueError("too small for .gvid clip header")

    clip = struct.unpack("<IBBHHHIIIII", data[:CLIP_HEADER_SIZE])
    magic, version, flags, pixel_format, quality, reserved2 = clip[:6]
    width, height, fps_x1000, target_kbps, frame_count_hint = clip[6:]
    if magic != GVID_CLIP_MAGIC:
        raise ValueError("bad .gvid clip magic")
    if version != GVID_VERSION:
        raise ValueError(f"unsupported .gvid version {version}")
    if flags & ~GVID_FLAG_MASK:
        raise ValueError("unknown .gvid flag bits set")
    if pixel_format > GVID_PIXEL_FORMAT_MAX or quality > GVID_QUALITY_MAX:
        raise ValueError("unsupported .gvid pixel_format or quality")
    if reserved2 != 0:
        raise ValueError("nonzero .gvid reserved2")
    if width == 0 or height == 0 or fps_x1000 == 0:
        raise ValueError("zero .gvid dimensions or fps")
    if bool(flags & 0x01) != bool(target_kbps):
        raise ValueError(".gvid rate-control flag and target_kbps disagree")

    pos = CLIP_HEADER_SIZE
    frame_count = 0
    last_tag: int | None = None
    payload_bytes = 0
    while pos < len(data):
        if len(data) - pos < FRAME_HEADER_SIZE:
            raise ValueError("truncated .gvid frame header")
        frame_magic, payload_size, frame_tag = struct.unpack("<IIQ", data[pos:pos + FRAME_HEADER_SIZE])
        pos += FRAME_HEADER_SIZE
        if frame_magic != GVID_FRAME_MAGIC:
            raise ValueError("bad .gvid frame magic")
        if payload_size == 0:
            raise ValueError("zero-size .gvid frame payload")
        if payload_size > len(data) - pos:
            raise ValueError("truncated .gvid frame payload")
        if last_tag is not None and frame_tag <= last_tag:
            raise ValueError("non-monotonic .gvid frame tag")
        last_tag = frame_tag
        frame_count += 1
        payload_bytes += payload_size
        pos += payload_size

    if frame_count == 0:
        raise ValueError("zero-frame .gvid stream")

    if frame_count_hint and frame_count_hint != frame_count:
        raise ValueError(".gvid frame_count_hint mismatch")

    return {
        "width": width,
        "height": height,
        "fps_x1000": fps_x1000,
        "frame_count": frame_count,
        "payload_bytes": payload_bytes,
    }


def require_string(obj: dict[str, Any], key: str, failures: list[str]) -> None:
    if not isinstance(obj.get(key), str) or not obj.get(key):
        failures.append(f"manifest missing string field {key}")


def validate_manifest_metadata(manifest: dict[str, Any], failures: list[str]) -> None:
    target = manifest.get("target")
    if (
        not isinstance(target, dict)
        or not isinstance(target.get("name"), str)
        or not target.get("name")
    ):
        failures.append("manifest target must be an object with non-empty name")

    notes = manifest.get("notes")
    if isinstance(notes, str):
        if not notes:
            failures.append("manifest notes must be a non-empty string or string list")
    elif isinstance(notes, list):
        if not notes or not all(isinstance(item, str) and item for item in notes):
            failures.append("manifest notes must be a non-empty string or string list")
    else:
        failures.append("manifest notes must be a non-empty string or string list")

    ci_run = manifest.get("ci_run")
    ci_run_ok = (
        isinstance(ci_run, str)
        and ci_run.startswith("https://github.com/")
        and "/actions/runs/" in ci_run
    )
    if not ci_run_ok and isinstance(ci_run, str) and ci_run:
        failures.append("manifest ci_run must be a GitHub Actions URL")

    pillars = manifest.get("product_pillars")
    if pillars is None:
        return
    if not isinstance(pillars, list):
        failures.append("manifest product_pillars must be a list when present")
        return
    by_id: dict[str, Any] = {}
    for idx, row in enumerate(pillars):
        if not isinstance(row, dict):
            failures.append(f"manifest product_pillars[{idx}] must be an object")
            continue
        pillar_id = row.get("id")
        if not isinstance(pillar_id, str) or not pillar_id:
            failures.append(f"manifest product_pillars[{idx}] missing id")
            continue
        by_id[pillar_id] = row
        for key in ("release_label", "status", "summary", "open_gate"):
            if not isinstance(row.get(key), str) or not row.get(key):
                failures.append(f"manifest product_pillars.{pillar_id} missing string field {key}")
    missing = set(REQUIRED_PRODUCT_PILLARS) - set(by_id)
    extra = set(by_id) - set(REQUIRED_PRODUCT_PILLARS)
    if missing:
        failures.append("manifest product_pillars missing ids: " + ", ".join(sorted(missing)))
    if extra:
        failures.append("manifest product_pillars has unexpected ids: " + ", ".join(sorted(extra)))
    for pillar_id, label in REQUIRED_PRODUCT_PILLARS.items():
        row = by_id.get(pillar_id)
        if isinstance(row, dict) and row.get("release_label") != label:
            failures.append(f"manifest product_pillars.{pillar_id} release_label must be {label!r}")


def verify_manifest(manifest_path: Path) -> tuple[int, dict[str, Any]]:
    root = manifest_path.parent
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    failures: list[str] = []
    rows: list[dict[str, Any]] = []

    if manifest.get("schema") != SCHEMA:
        failures.append(f"manifest schema must be {SCHEMA}")
    for key in ("repo_commit", "ci_run"):
        require_string(manifest, key, failures)
    validate_manifest_metadata(manifest, failures)

    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        failures.append("manifest artifacts must be a non-empty list")
        artifacts = []

    seen_gvid_sample = False
    seen_sample = False
    seen_receipt = False
    seen_review = False
    seen_hashes = False

    for idx, artifact in enumerate(artifacts):
        if not isinstance(artifact, dict):
            failures.append(f"artifact {idx} must be an object")
            continue
        rel = artifact.get("path")
        kind = artifact.get("kind")
        expected_sha = artifact.get("sha256")
        expected_size = artifact.get("size_bytes")
        if not isinstance(rel, str) or not rel:
            failures.append(f"artifact {idx} missing path")
            continue
        try:
            path = safe_child(root, rel)
        except ValueError as exc:
            failures.append(str(exc))
            continue
        if rel.startswith("samples/"):
            seen_sample = True
        if rel.startswith("receipts/"):
            seen_receipt = True
        if rel.startswith("review/") or kind == "dashboard":
            seen_review = True
        if rel == "hashes/sha256sums.txt":
            seen_hashes = True
        if kind == "gvid" and rel.startswith("samples/"):
            seen_gvid_sample = True
        row: dict[str, Any] = {"path": rel, "kind": kind, "status": "missing"}
        rows.append(row)
        if not path.is_file():
            failures.append(f"{rel}: missing file")
            continue

        size = path.stat().st_size
        actual_sha = sha256_file(path)
        row.update({"status": "ok", "size_bytes": size, "sha256": actual_sha})
        if not isinstance(expected_size, int) or expected_size != size:
            failures.append(f"{rel}: size_bytes mismatch")
        if not isinstance(expected_sha, str) or expected_sha != actual_sha:
            failures.append(f"{rel}: sha256 mismatch")

        if kind == "json":
            try:
                json.loads(path.read_text(encoding="utf-8"))
            except Exception as exc:
                failures.append(f"{rel}: invalid JSON: {exc}")
        elif kind == "gvid":
            try:
                row["gvid"] = validate_gvid(path)
            except Exception as exc:
                failures.append(f"{rel}: invalid .gvid: {exc}")
        elif kind not in {"text", "media", "dashboard", "receipt"}:
            failures.append(f"{rel}: unknown artifact kind {kind!r}")

    if not seen_gvid_sample:
        failures.append("manifest artifacts must include at least one samples/*.gvid artifact")
    if not seen_sample:
        failures.append("manifest artifacts must include at least one samples/ artifact")
    if not seen_receipt:
        failures.append("manifest artifacts must include at least one receipts/ artifact")
    if not seen_review:
        failures.append("manifest artifacts must include at least one review/ or dashboard artifact")
    if not seen_hashes:
        failures.append("manifest artifacts must include hashes/sha256sums.txt")

    report = {
        "manifest": str(manifest_path),
        "root": str(root),
        "failures": failures,
        "artifacts": rows,
    }
    return (1 if failures else 0), report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("manifest", type=Path, help="path to gpr_labs_bundle.v1 manifest.json")
    ap.add_argument("--json", action="store_true", help="emit JSON report")
    args = ap.parse_args()

    rc, report = verify_manifest(args.manifest)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print("=== Labs bundle verification ===")
        print(f"manifest={report['manifest']}")
        for row in report["artifacts"]:
            print(f"{row['status']:8s} {row['kind']:9s} {row['path']}")
        if report["failures"]:
            print("\nFailures:", file=sys.stderr)
            for failure in report["failures"]:
                print(f"  {failure}", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
