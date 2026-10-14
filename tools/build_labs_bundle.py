#!/usr/bin/env python3
"""Build a portable Labs artifact bundle manifest.

The heavy files already live outside git. This helper makes the bundle
metadata reproducible: it records sizes and SHA-256 hashes for explicitly named
bundle-relative files, writes hashes/sha256sums.txt, and emits manifest.json in
the schema consumed by tools/verify_labs_bundle.py.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "gpr_labs_bundle.v1"
DEFAULT_MANIFEST = "manifest.json"
DEFAULT_HASHES = "hashes/sha256sums.txt"
DEFAULT_RELEASE_EVIDENCE = Path(__file__).resolve().parents[1] / "docs/release_evidence.json"
ALLOWED_KINDS = {"dashboard", "gvid", "json", "media", "receipt", "text"}


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


def parse_artifact(value: str) -> tuple[str, str]:
    if ":" not in value:
        raise argparse.ArgumentTypeError("artifact must be RELPATH:KIND")
    rel, kind = value.rsplit(":", 1)
    if not rel:
        raise argparse.ArgumentTypeError("artifact path must be non-empty")
    if kind not in ALLOWED_KINDS:
        allowed = ", ".join(sorted(ALLOWED_KINDS))
        raise argparse.ArgumentTypeError(f"artifact kind must be one of: {allowed}")
    return rel, kind


def artifact_row(root: Path, rel: str, kind: str) -> dict[str, Any]:
    path = safe_child(root, rel)
    if not path.is_file():
        raise FileNotFoundError(f"{rel}: missing file")
    data_size = path.stat().st_size
    return {
        "path": rel,
        "kind": kind,
        "size_bytes": data_size,
        "sha256": sha256_file(path),
    }


def write_hashes(root: Path, rows: list[dict[str, Any]], rel: str) -> dict[str, Any]:
    hashes_path = safe_child(root, rel)
    hashes_path.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for row in rows:
        if row["path"] == rel:
            continue
        lines.append(f"{row['sha256']}  {row['path']}")
    hashes_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return artifact_row(root, rel, "text")


def load_product_pillars(path: Path | None) -> list[dict[str, Any]] | None:
    if path is None:
        return None
    if not path.is_file():
        if path == DEFAULT_RELEASE_EVIDENCE:
            return None
        raise FileNotFoundError(f"product pillar source does not exist: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: release evidence must be an object")
    pillars = data.get("product_pillars")
    if pillars is None:
        return None
    if not isinstance(pillars, list) or not pillars:
        raise ValueError(f"{path}: product_pillars must be a non-empty list")
    rows: list[dict[str, Any]] = []
    for idx, row in enumerate(pillars):
        if not isinstance(row, dict):
            raise ValueError(f"{path}: product_pillars[{idx}] must be an object")
        slim = {
            "id": row.get("id"),
            "release_label": row.get("release_label"),
            "status": row.get("status"),
            "summary": row.get("summary"),
            "open_gate": row.get("open_gate"),
        }
        missing = [key for key, value in slim.items() if not isinstance(value, str) or not value]
        if missing:
            raise ValueError(f"{path}: product_pillars[{idx}] missing string fields: {', '.join(missing)}")
        rows.append(slim)
    return rows


def build_manifest(args: argparse.Namespace) -> dict[str, Any]:
    root = args.bundle_root.resolve()
    if not root.is_dir():
        raise FileNotFoundError(f"bundle root does not exist: {root}")

    seen: set[str] = set()
    rows: list[dict[str, Any]] = []
    for rel, kind in args.artifact:
        if rel in {args.manifest, args.hashes}:
            continue
        if rel in seen:
            raise ValueError(f"duplicate artifact path: {rel}")
        seen.add(rel)
        rows.append(artifact_row(root, rel, kind))

    hashes_row = write_hashes(root, rows, args.hashes)
    rows.append(hashes_row)

    manifest: dict[str, Any] = {
        "schema": SCHEMA,
        "repo_commit": args.repo_commit,
        "ci_run": args.ci_run,
        "target": {"name": args.target_name},
        "notes": args.note,
        "artifacts": rows,
    }
    if args.target_role:
        manifest["target"]["role"] = args.target_role
    product_pillars = load_product_pillars(args.product_pillars_from)
    if product_pillars is not None:
        manifest["product_pillars"] = product_pillars

    manifest_path = safe_child(root, args.manifest)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("bundle_root", type=Path, help="bundle directory to describe")
    ap.add_argument("--repo-commit", required=True, help="repo commit that produced or verified the bundle")
    ap.add_argument("--ci-run", required=True, help="GitHub Actions run URL for the commit")
    ap.add_argument("--target-name", required=True, help="target hardware or stand-in name")
    ap.add_argument("--target-role", default="", help="optional target role, e.g. stand-in or camera")
    ap.add_argument("--note", action="append", required=True, help="bundle note; repeat for multiple notes")
    ap.add_argument(
        "--product-pillars-from",
        type=Path,
        default=DEFAULT_RELEASE_EVIDENCE,
        help="optional release evidence index; copy product_pillars only when present (missing default index is allowed)",
    )
    ap.add_argument("--no-product-pillars", action="store_true", help="omit product_pillars from the bundle manifest")
    ap.add_argument(
        "--artifact",
        action="append",
        type=parse_artifact,
        required=True,
        help="bundle-relative artifact as RELPATH:KIND",
    )
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST, help="bundle-relative manifest output path")
    ap.add_argument("--hashes", default=DEFAULT_HASHES, help="bundle-relative sha256sum output path")
    args = ap.parse_args()
    if args.no_product_pillars:
        args.product_pillars_from = None

    try:
        manifest = build_manifest(args)
    except Exception as exc:
        print(f"build_labs_bundle: {exc}", file=sys.stderr)
        return 1

    print(json.dumps({
        "manifest": str((args.bundle_root / args.manifest).resolve()),
        "artifact_count": len(manifest["artifacts"]),
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
