#!/usr/bin/env python3
"""Build the runtime policy for calibrated noise removal/addback.

The coverage audit says which camera families have validated darkframe
sidecars. This policy turns that audit into a renderer/trainer-facing contract:
nonzero denoised targets and synthetic/raw-noise addback are allowed only for
exact camera/ISO classes with production-ready sidecars. Everything else is
metadata-conditioning only.
"""
from __future__ import annotations

import argparse
import html
import json
import time
from pathlib import Path
from typing import Any

from build_camera_noise_coverage_audit import (
    DEFAULT_SIDECAR_ROOT,
    EXPECTED_CAMERAS,
    build_audit,
    load_sidecar,
    row_from_sidecar,
    sidecar_paths,
    synthetic_rows,
)


SCHEMA = "gpr.camera_noise_runtime_policy.v1"


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--sidecar-root", type=Path, action="append", default=[])
    ap.add_argument("--synthetic", action="store_true", help="Build a CI-safe synthetic policy.")
    return ap.parse_args()


def policy_for_camera(expected: dict[str, Any], rows: list[dict[str, Any]]) -> dict[str, Any]:
    ready_rows = [
        row
        for row in rows
        if row.get("production_ready") and row.get("usable_for_training_targets")
    ]
    ready_isos = sorted({int(row["iso"]) for row in ready_rows if row.get("iso") is not None})
    ready_sidecars = [
        {
            "path": row.get("path"),
            "iso": row.get("iso"),
            "width": row.get("width"),
            "height": row.get("height"),
            "bit_depth": row.get("bit_depth"),
            "cfa_phase": row.get("cfa_phase"),
            "source_kind": row.get("source_kind"),
            "sample_count": row.get("sample_count"),
        }
        for row in ready_rows
    ]
    ready = bool(ready_rows)
    blocked_reason = None
    if not ready:
        blocked_reason = (
            "no validated production_ready darkframe sidecar; preserve metadata "
            "and do not remove/add nonzero noise for this camera family"
        )
    return {
        "camera_key": expected["key"],
        "label": expected["label"],
        "allow_denoised_training_targets": ready,
        "allow_nonzero_noise_addback": ready,
        "allow_noise_conditioning_metadata": True,
        "iso_policy": "exact_sidecar_iso_only" if ready else "metadata_only",
        "ready_isos": ready_isos,
        "ready_sidecars": ready_sidecars,
        "missing_sidecar_blocker": blocked_reason,
        "runtime_fallback": {
            "mode": "calibrated_sidecar_required" if ready else "metadata_conditioning_only",
            "if_iso_missing": "metadata_conditioning_only_no_addback",
            "if_camera_missing": "metadata_conditioning_only_no_addback",
        },
    }


def build_policy(rows: list[dict[str, Any]], roots: list[Path], mode: str) -> dict[str, Any]:
    audit = build_audit(rows, roots, mode)
    rows_by_camera: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        rows_by_camera.setdefault(str(row.get("camera_key") or "unknown"), []).append(row)
    policies = [
        policy_for_camera(expected, rows_by_camera.get(str(expected["key"]), []))
        for expected in EXPECTED_CAMERAS
    ]
    allowed = [row["camera_key"] for row in policies if row["allow_nonzero_noise_addback"]]
    blocked = [row["camera_key"] for row in policies if not row["allow_nonzero_noise_addback"]]
    return {
        "schema": SCHEMA,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "mode": mode,
        "sidecar_roots": [root.as_posix() for root in roots],
        "source_coverage_schema": audit["schema"],
        "summary": {
            "policy_count": len(policies),
            "nonzero_noise_addback_camera_count": len(allowed),
            "metadata_only_camera_count": len(blocked),
            "nonzero_noise_addback_camera_keys": allowed,
            "metadata_only_camera_keys": blocked,
            "production_noise_policy_complete": not blocked,
        },
        "rules": [
            "Use calibrated noise sidecars only for exact camera family and ISO classes listed in ready_isos.",
            "If the camera or ISO is missing, preserve source metadata and keep nonzero denoised targets/noise addback disabled.",
            "DNG NoiseProfile or ISO metadata may condition a model, but it is not proof that signal can be removed from training targets.",
            "Noise addback must be generated from calibrated sidecar statistics, not from REF/source image residuals at render time.",
        ],
        "camera_policies": policies,
    }


def render_html(data: dict[str, Any]) -> str:
    summary = data["summary"]
    cards = [
        ("Mode", data["mode"]),
        ("Policies", summary["policy_count"]),
        ("Noise addback enabled", ", ".join(summary["nonzero_noise_addback_camera_keys"]) or "none"),
        ("Metadata-only", ", ".join(summary["metadata_only_camera_keys"]) or "none"),
    ]
    card_html = "\n".join(
        f'<section class="card"><div class="label">{html.escape(str(label))}</div><div class="value">{html.escape(str(value))}</div></section>'
        for label, value in cards
    )
    policy_rows = "\n".join(
        "<tr>"
        f"<td>{html.escape(str(row['label']))}</td>"
        f"<td>{'yes' if row['allow_denoised_training_targets'] else 'no'}</td>"
        f"<td>{'yes' if row['allow_nonzero_noise_addback'] else 'no'}</td>"
        f"<td>{html.escape(', '.join(str(v) for v in row['ready_isos']) or '')}</td>"
        f"<td>{html.escape(str(row['iso_policy']))}</td>"
        f"<td>{html.escape(str(row.get('missing_sidecar_blocker') or ''))}</td>"
        "</tr>"
        for row in data["camera_policies"]
    )
    rule_items = "\n".join(f"<li>{html.escape(rule)}</li>" for rule in data["rules"])
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Camera Noise Runtime Policy</title>
<style>
body {{ margin: 30px; font: 15px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; color: #111820; background: #f5f7f8; }}
main {{ max-width: 1220px; margin: 0 auto; }}
h1 {{ font-size: 34px; margin: 0 0 8px; }}
.sub {{ color: #596572; }}
.grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 12px; margin: 22px 0; }}
.card {{ background: white; border: 1px solid #dce2e7; border-radius: 8px; padding: 14px; }}
.label {{ color: #596572; font-size: 12px; text-transform: uppercase; }}
.value {{ font-size: 22px; font-weight: 760; }}
table {{ width: 100%; border-collapse: collapse; background: white; border: 1px solid #dce2e7; margin: 14px 0 26px; }}
th, td {{ border-bottom: 1px solid #e7ebef; padding: 8px; text-align: left; vertical-align: top; }}
th {{ background: #eef2f5; color: #4f5b67; font-size: 12px; text-transform: uppercase; }}
section.rules {{ background: white; border: 1px solid #dce2e7; border-radius: 8px; padding: 14px; }}
</style></head><body><main>
<h1>Camera Noise Runtime Policy</h1>
<p class="sub">Schema {html.escape(data["schema"])}. This is the runtime guardrail for denoised still/SR targets and calibrated noise addback.</p>
<div class="grid">{card_html}</div>
<section class="rules"><h2>Rules</h2><ul>{rule_items}</ul></section>
<h2>Camera Policies</h2>
<table><thead><tr><th>Camera family</th><th>Denoised targets</th><th>Noise addback</th><th>Ready ISOs</th><th>ISO policy</th><th>Blocker</th></tr></thead><tbody>{policy_rows}</tbody></table>
</main></body></html>
"""


def load_rows(roots: list[Path], synthetic: bool) -> tuple[list[dict[str, Any]], list[Path], str]:
    if synthetic:
        return synthetic_rows(), roots or [Path("synthetic")], "synthetic"
    search_roots = roots or [DEFAULT_SIDECAR_ROOT]
    rows: list[dict[str, Any]] = []
    for path in sidecar_paths(search_roots):
        sidecar = load_sidecar(path)
        if sidecar is not None:
            rows.append(row_from_sidecar(path, sidecar))
    return rows, search_roots, "real"


def main() -> int:
    args = parse_args()
    rows, roots, mode = load_rows(args.sidecar_root, args.synthetic)
    data = build_policy(rows, roots, mode)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.output_dir / "camera_noise_runtime_policy.json"
    html_path = args.output_dir / "index.html"
    json_path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    html_path.write_text(render_html(data), encoding="utf-8")
    print(html_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
