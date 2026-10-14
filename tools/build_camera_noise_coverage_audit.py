#!/usr/bin/env python3
"""Build a camera-noise calibration coverage audit.

The noise sidecar contract proves whether a camera/ISO class can safely remove
noise from training targets and add only calibrated noise/texture back later.
This dashboard keeps the product claim honest by separating cameras with
validated sidecars from cameras that only have fixtures or metadata.
"""
from __future__ import annotations

import argparse
import html
import json
import os
import time
from collections import defaultdict
from pathlib import Path
from typing import Any


SCHEMA = "gpr.camera_noise_coverage_audit.v1"
SIDECAR_SCHEMA = "gpr.camera_noise_calibration.v1"
DEFAULT_SIDECAR_ROOT = Path(os.environ.get("GPR_ARTIFACT_ROOT", Path(os.environ.get("GPR_EXTERNAL_ROOT", Path(__file__).resolve().parents[1])) / "artifacts")) / "camera_noise_sidecars"
EXPECTED_CAMERAS = [
    {
        "key": "x2d",
        "label": "Hasselblad X2D 100C",
        "match": ["hasselblad", "x2d"],
        "fixture_status": "100MP real still fixture and darkframe stack",
    },
    {
        "key": "z8",
        "label": "Nikon Z 8",
        "match": ["nikon", "z 8"],
        "fixture_status": "50MP real still fixture and darkframe stack",
    },
    {
        "key": "mission1",
        "label": "GoPro Mission 1",
        "match": ["gopro", "mission"],
        "fixture_status": "12MP/50MP real Mission DNG/GPR fixtures; no validated darkframe sidecar",
    },
    {
        "key": "iphone",
        "label": "iPhone CFA",
        "match": ["iphone", "apple"],
        "fixture_status": "real iPhone CFA fixture; no validated darkframe sidecar",
    },
]


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--sidecar-root", type=Path, action="append", default=[])
    ap.add_argument("--synthetic", action="store_true", help="Build a CI-safe synthetic coverage audit.")
    return ap.parse_args()


def camera_text(sidecar: dict[str, Any]) -> str:
    camera = sidecar.get("camera") or {}
    return f"{camera.get('make', '')} {camera.get('model', '')}".lower()


def classify_camera(sidecar: dict[str, Any]) -> str:
    text = camera_text(sidecar)
    for expected in EXPECTED_CAMERAS:
        if all(part in text for part in expected["match"]):
            return str(expected["key"])
    return "unknown"


def sidecar_paths(roots: list[Path]) -> list[Path]:
    paths: list[Path] = []
    for root in roots:
        if root.is_file() and root.name.endswith("_noise_calibration.json"):
            paths.append(root)
        elif root.exists():
            paths.extend(sorted(root.rglob("*_noise_calibration.json")))
    return sorted(set(paths))


def load_sidecar(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    if data.get("schema") != SIDECAR_SCHEMA:
        return None
    return data


def row_from_sidecar(path: Path, sidecar: dict[str, Any]) -> dict[str, Any]:
    camera = sidecar.get("camera") or {}
    calibrations = sidecar.get("calibrations") or []
    cal = calibrations[0] if calibrations and isinstance(calibrations[0], dict) else {}
    return {
        "path": path.as_posix(),
        "camera_key": classify_camera(sidecar),
        "make": camera.get("make"),
        "model": camera.get("model"),
        "width": camera.get("width"),
        "height": camera.get("height"),
        "bit_depth": camera.get("bit_depth"),
        "cfa_phase": camera.get("cfa_phase"),
        "iso": cal.get("iso"),
        "sample_count": cal.get("sample_count"),
        "source_kind": cal.get("source_kind"),
        "production_ready": bool(sidecar.get("production_ready")),
        "usable_for_training_targets": bool(cal.get("usable_for_training_targets")),
        "method": cal.get("calibration_method"),
    }


def synthetic_rows() -> list[dict[str, Any]]:
    return [
        {
            "path": "synthetic/x2d_iso64_noise_calibration.json",
            "camera_key": "x2d",
            "make": "Hasselblad",
            "model": "X2D 100C",
            "width": 11664,
            "height": 8750,
            "bit_depth": 16,
            "cfa_phase": "RGGB",
            "iso": 64,
            "sample_count": 8,
            "source_kind": "darkframes",
            "production_ready": True,
            "usable_for_training_targets": True,
            "method": "synthetic_darkframe_stack",
        },
        {
            "path": "synthetic/z8_iso500_noise_calibration.json",
            "camera_key": "z8",
            "make": "NIKON CORPORATION",
            "model": "NIKON Z 8",
            "width": 8280,
            "height": 5520,
            "bit_depth": 14,
            "cfa_phase": "RGGB",
            "iso": 500,
            "sample_count": 8,
            "source_kind": "darkframes",
            "production_ready": True,
            "usable_for_training_targets": True,
            "method": "synthetic_darkframe_stack",
        },
    ]


def build_audit(rows: list[dict[str, Any]], roots: list[Path], mode: str) -> dict[str, Any]:
    by_camera: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        by_camera[str(row.get("camera_key") or "unknown")].append(row)

    coverage = []
    for expected in EXPECTED_CAMERAS:
        key = str(expected["key"])
        ready_rows = [
            row
            for row in by_camera.get(key, [])
            if row.get("production_ready") and row.get("usable_for_training_targets")
        ]
        isos = sorted({int(row["iso"]) for row in ready_rows if row.get("iso") is not None})
        coverage.append(
            {
                "key": key,
                "label": expected["label"],
                "fixture_status": expected["fixture_status"],
                "ready": bool(ready_rows),
                "ready_iso_count": len(isos),
                "ready_isos": isos,
                "sidecar_count": len(by_camera.get(key, [])),
                "blocker": None
                if ready_rows
                else "no validated production_ready darkframe sidecar for this camera family",
            }
        )

    ready_keys = [row["key"] for row in coverage if row["ready"]]
    missing_keys = [row["key"] for row in coverage if not row["ready"]]
    return {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "mode": mode,
        "sidecar_roots": [root.as_posix() for root in roots],
        "summary": {
            "sidecar_count": len(rows),
            "expected_camera_count": len(EXPECTED_CAMERAS),
            "ready_camera_count": len(ready_keys),
            "ready_camera_keys": ready_keys,
            "missing_camera_keys": missing_keys,
            "production_noise_coverage_ready": len(missing_keys) == 0,
        },
        "coverage": coverage,
        "sidecars": rows,
    }


def render_html(data: dict[str, Any]) -> str:
    summary = data["summary"]
    cards = [
        ("Mode", data["mode"]),
        ("Sidecars", summary["sidecar_count"]),
        ("Ready cameras", f"{summary['ready_camera_count']} / {summary['expected_camera_count']}"),
        ("Missing", ", ".join(summary["missing_camera_keys"]) or "none"),
    ]
    card_html = "\n".join(
        f'<section class="card"><div class="label">{html.escape(str(label))}</div><div class="value">{html.escape(str(value))}</div></section>'
        for label, value in cards
    )
    coverage_rows = "\n".join(
        "<tr>"
        f"<td>{html.escape(str(row['label']))}</td>"
        f"<td>{'ready' if row['ready'] else 'missing'}</td>"
        f"<td>{html.escape(', '.join(str(v) for v in row['ready_isos']) or '')}</td>"
        f"<td>{row['sidecar_count']}</td>"
        f"<td>{html.escape(str(row['fixture_status']))}</td>"
        f"<td>{html.escape(str(row.get('blocker') or ''))}</td>"
        "</tr>"
        for row in data["coverage"]
    )
    sidecar_rows = "\n".join(
        "<tr>"
        f"<td>{html.escape(str(row.get('camera_key') or ''))}</td>"
        f"<td>{html.escape(str(row.get('make') or ''))} {html.escape(str(row.get('model') or ''))}</td>"
        f"<td>{html.escape(str(row.get('iso') or ''))}</td>"
        f"<td>{html.escape(str(row.get('sample_count') or ''))}</td>"
        f"<td>{html.escape(str(row.get('cfa_phase') or ''))}</td>"
        f"<td>{html.escape(str(row.get('production_ready')))}</td>"
        f"<td><code>{html.escape(str(row.get('path') or ''))}</code></td>"
        "</tr>"
        for row in data["sidecars"]
    )
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Camera Noise Coverage Audit</title>
<style>
body {{ margin: 30px; font: 15px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; color: #111820; background: #f5f7f8; }}
main {{ max-width: 1220px; margin: 0 auto; }}
h1 {{ font-size: 34px; margin: 0 0 8px; }}
.sub {{ color: #596572; }}
.grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 12px; margin: 22px 0; }}
.card {{ background: white; border: 1px solid #dce2e7; border-radius: 8px; padding: 14px; }}
.label {{ color: #596572; font-size: 12px; text-transform: uppercase; }}
.value {{ font-size: 24px; font-weight: 760; }}
table {{ width: 100%; border-collapse: collapse; background: white; border: 1px solid #dce2e7; margin: 14px 0 26px; }}
th, td {{ border-bottom: 1px solid #e7ebef; padding: 8px; text-align: left; vertical-align: top; }}
th {{ background: #eef2f5; color: #4f5b67; font-size: 12px; text-transform: uppercase; }}
code {{ font-size: 12px; word-break: break-all; }}
</style></head><body><main>
<h1>Camera Noise Coverage Audit</h1>
<p class="sub">Schema {html.escape(data["schema"])}. Production noise removal/addback is allowed only for camera families with validated darkframe sidecars.</p>
<div class="grid">{card_html}</div>
<h2>Product Coverage</h2>
<table><thead><tr><th>Camera family</th><th>Status</th><th>Ready ISOs</th><th>Sidecars</th><th>Fixture status</th><th>Blocker</th></tr></thead><tbody>{coverage_rows}</tbody></table>
<h2>Validated Sidecars</h2>
<table><thead><tr><th>Key</th><th>Camera</th><th>ISO</th><th>Frames</th><th>CFA</th><th>Ready</th><th>Path</th></tr></thead><tbody>{sidecar_rows}</tbody></table>
</main></body></html>
"""


def main() -> int:
    args = parse_args()
    roots = args.sidecar_root or [DEFAULT_SIDECAR_ROOT]
    rows = synthetic_rows() if args.synthetic else [
        row_from_sidecar(path, sidecar)
        for path in sidecar_paths(roots)
        for sidecar in [load_sidecar(path)]
        if sidecar is not None
    ]
    data = build_audit(rows, roots, "synthetic" if args.synthetic else "real")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "noise_coverage.json").write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.output_dir / "index.html").write_text(render_html(data), encoding="utf-8")
    print(args.output_dir / "index.html")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
