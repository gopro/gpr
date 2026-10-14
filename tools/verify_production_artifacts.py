#!/usr/bin/env python3
"""Verify production artifacts referenced by pipelines/registry.json.

Checkpoint binaries and large training-pair datasets stay off main. Registry paths remain portable
(`models/name.pt`) and resolve from:

  1. the repo path itself, for local developer copies;
  2. GPR_MODEL_ROOT and GPR_CHECKPOINT_ROOT, os.pathsep-separated;
  3. GPR_EXTERNAL_ROOT/{models,checkpoints}, when explicitly configured.

Default mode inventories release-required runtime weights and routing sidecars,
reporting failures without requiring private files in CI. Use --strict for
release verification. --all includes historical/review CNNs; combine with
--include-training-data for the full checkpoint and training-pair inventory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
REGISTRY = REPO / "pipelines/registry.json"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def candidates(path_value: str) -> list[Path]:
    path = Path(path_value)
    if path.is_absolute():
        return [path]

    roots: list[tuple[Path, bool]] = [(REPO, False)]
    for key in ("GPR_MODEL_ROOT", "GPR_CHECKPOINT_ROOT"):
        for item in os.environ.get(key, "").split(os.pathsep):
            if item:
                roots.append((Path(item), True))
    if os.environ.get("GPR_EXTERNAL_ROOT"):
        external_root = Path(os.environ["GPR_EXTERNAL_ROOT"])
        roots.extend([
            (external_root, False),
            (external_root / "models", True),
            (external_root / "checkpoints", True),
        ])

    out: list[Path] = []
    seen = set()
    for root, prefer_stripped in roots:
        root_path = root / path
        stripped = None
        if path.parts and path.parts[0] in {"models", "checkpoints"}:
            stripped = root / Path(*path.parts[1:])
        ordered = ([stripped, root_path] if prefer_stripped and stripped is not None
                   else [root_path] + ([stripped] if stripped is not None else []))
        for candidate in ordered:
            key = str(candidate)
            if key not in seen:
                seen.add(key)
                out.append(candidate)
    return out


def release_required(pipeline: dict) -> bool:
    """Structured selection wins; legacy registries retain their release roots."""
    if "release_selection" in pipeline:
        return pipeline["release_selection"] in {"ship_root", "existing_offline", "production"}
    return (str(pipeline.get("$role", "")).startswith(("ship-", "UPRESABLE production chain"))
            or pipeline.get("production_scope") == "offline_production")


def selected_cnns(registry: dict, all_records: bool = False) -> dict:
    names = {p.get("cnn") for p in registry.get("pipelines", {}).values()
             if isinstance(p, dict) and release_required(p)}
    return {name: cnn for name, cnn in registry.get("cnns", {}).items()
            if isinstance(cnn, dict) and not name.startswith("$") and name != "none"
            and (all_records or name in names)}


def artifact_specs(cnn: dict, include_training_data: bool = False) -> list[tuple[str, str, str, str | None]]:
    specs = []
    if "ckpt_path" in cnn:
        specs.append(("ckpt_path", cnn["ckpt_path"], "ckpt_sha256", cnn.get("ckpt_sha256")))
    if include_training_data and "training_pairs_path" in cnn:
        specs.append((
            "training_pairs_path",
            cnn["training_pairs_path"],
            "training_pairs_sha256",
            cnn.get("training_pairs_sha256"),
        ))
    for suffix in ("y", "cb", "cr", "chroma", "detail", "rgb_detail"):
        path_key = f"ckpt_{suffix}"
        if path_key in cnn:
            specs.append((path_key, cnn[path_key], f"{path_key}_sha256", cnn.get(f"{path_key}_sha256")))
    if "luma_detail_refiner" in cnn:
        specs.append((
            "luma_detail_refiner",
            cnn["luma_detail_refiner"],
            "luma_detail_refiner_sha256",
            cnn.get("luma_detail_refiner_sha256"),
        ))
    if "router_sidecar_path" in cnn:
        specs.append(("router_sidecar_path", cnn["router_sidecar_path"],
                      "router_sidecar_sha256", cnn.get("router_sidecar_sha256")))
    for role, expert in (cnn.get("expert_checkpoints") or {}).items():
        specs.append((f"expert_checkpoints.{role}.path", expert["path"],
                      f"expert_checkpoints.{role}.sha256", expert.get("sha256")))
    return specs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--strict", action="store_true", help="exit nonzero on missing/hash mismatch")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of text")
    ap.add_argument("--registry", type=Path, default=REGISTRY)
    ap.add_argument("--all", action="store_true", help="include historical and review-only CNNs")
    ap.add_argument("--include-training-data", action="store_true", help="also verify training pairs for selected CNNs")
    args = ap.parse_args()

    reg = json.loads(args.registry.read_text())
    rows = []
    failures = 0
    for cnn_name, cnn in selected_cnns(reg, args.all).items():
        for path_field, path_value, sha_field, expected_sha in artifact_specs(cnn, args.include_training_data):
            found = next((p for p in candidates(path_value) if p.is_file()), None)
            status = "missing"
            actual_sha = None
            if found is not None:
                actual_sha = sha256_file(found)
                status = "ok" if actual_sha == expected_sha else "sha_mismatch"
            if status != "ok":
                failures += 1
            rows.append({
                "cnn": cnn_name,
                "path_field": path_field,
                "path": path_value,
                "resolved": str(found) if found else None,
                "sha_field": sha_field,
                "expected_sha256": expected_sha,
                "actual_sha256": actual_sha,
                "status": status,
                "searched": [str(p) for p in candidates(path_value)[:8]],
            })

    if args.json:
        print(json.dumps({"scope": "all" if args.all else "release_required",
                          "include_training_data": args.include_training_data,
                          "failures": failures, "artifacts": rows}, indent=2))
    else:
        print("=== production artifact verification ===")
        print(f"GPR_MODEL_ROOT={os.environ.get('GPR_MODEL_ROOT', '')}")
        print(f"GPR_CHECKPOINT_ROOT={os.environ.get('GPR_CHECKPOINT_ROOT', '')}")
        for row in rows:
            loc = row["resolved"] or row["path"]
            print(f"{row['status']:12s} {row['cnn']} {row['path_field']} {loc}")
        if failures:
            print(f"\n{failures} artifact(s) missing or mismatched")
            print("Use --strict for release gating; default mode is inventory-only.")

    return 1 if args.strict and failures else 0


if __name__ == "__main__":
    sys.exit(main())
