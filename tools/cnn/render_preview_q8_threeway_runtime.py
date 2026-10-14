#!/usr/bin/env python3
"""Render the q8 three-way PREVIEW offline/review path.

This is the product-facing wrapper for the integrated no-REF PREVIEW renderer.
It delegates to ``evaluate_preview_q8_threeway_runtime_fullframe.py`` because
that script already owns the frozen route/render contract and timing receipt.
REF, when present in the child evaluator, is scoring-only; route and render
inputs are source RGB, source-derived features, checkpoint sidecars, and
normalized coordinates.
"""
from __future__ import annotations


import argparse
import os
import subprocess
import sys
from pathlib import Path




sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_paths import external_path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_EXTERNAL_ROOT = external_path('')
DEFAULT_ARTIFACT_ROOT = DEFAULT_EXTERNAL_ROOT / "artifacts"


def default_external_root() -> Path:
    return external_path()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-name", default="q8_threeway_preview_render", help="subdirectory under artifact root")
    ap.add_argument("--artifact-root", type=Path, default=None)
    ap.add_argument("--tmp-dir", type=Path, default=None)
    ap.add_argument("--external-root", type=Path, default=None)
    ap.add_argument("--image-id", action="append", default=[])
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--save-fullframe", action="store_true")
    ap.add_argument("--output-dir", type=Path, default=None)
    args = ap.parse_args()

    external_root = args.external_root or default_external_root()
    artifact_root = args.artifact_root or Path(os.environ.get("GPR_ARTIFACT_ROOT", external_root / "artifacts"))
    tmp_dir = args.tmp_dir or Path(os.environ.get("GATE_TMPDIR", external_root / "tmp"))
    output_dir = args.output_dir or (artifact_root / "preview_runtime_policy_20260613" / args.run_name)
    output_json = output_dir / "preview_q8_threeway_runtime_fullframe.json"
    output_html = output_dir / "preview_q8_threeway_runtime_fullframe.html"
    output_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable,
        str(REPO / "tools/cnn/evaluate_preview_q8_threeway_runtime_fullframe.py"),
        "--output-dir",
        str(output_dir),
        "--output-json",
        str(output_json),
        "--output-html",
        str(output_html),
        "--tmp-dir",
        str(tmp_dir),
        "--external-root",
        str(external_root),
    ]
    if args.limit:
        cmd.extend(["--limit", str(args.limit)])
    if args.save_fullframe:
        cmd.append("--save-fullframe")
    for image_id in args.image_id:
        cmd.extend(["--image-id", image_id])

    env = dict(os.environ)
    env["TMPDIR"] = str(tmp_dir)
    env["GATE_TMPDIR"] = str(tmp_dir)
    env["GPR_EXTERNAL_ROOT"] = str(external_root)
    print("[preview-render] " + " ".join(cmd), flush=True)
    return subprocess.call(cmd, cwd=str(REPO), env=env)


if __name__ == "__main__":
    raise SystemExit(main())
