#!/usr/bin/env python3
"""Summarize informational PREVIEW holdout runs.

This script does not run a pipeline and does not issue ship claims. It reads
existing `run_gate.py --test-set ...` receipts and reports breadth statistics:
worst, p95/p05 tails, median, per-image failures, and per-stratum breakdowns.
The frozen ship gate remains `test_set.json` plus `run_gate.py` default mode.
"""
from __future__ import annotations

import argparse
import html
import json
import statistics
from collections import defaultdict
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
RUNS = REPO / "tests/quality_gates/runs"
DEFAULT_MANIFEST = REPO / "pipelines/preview_holdout_set.json"
GATES = REPO / "tests/quality_gates/gates.json"
DASH = RUNS / "dashboard"


TAIL_HIGH_METRICS = ("lpips", "dE2000_mean")
TAIL_LOW_METRICS = ("ms_ssim", "y_psnr")
METRICS = TAIL_HIGH_METRICS + TAIL_LOW_METRICS


def load_json(path: Path) -> dict:
    return json.loads(path.read_text())


def resolve_run(arg: str) -> tuple[str, Path, dict]:
    p = Path(arg)
    if p.is_dir():
        p = p / "run.json"
    elif not p.exists():
        p = RUNS / arg / "run.json"
    if not p.exists():
        raise FileNotFoundError(f"missing run.json for {arg}")
    run = load_json(p)
    return p.parent.name, p, run


def percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    values = sorted(values)
    if len(values) == 1:
        return values[0]
    pos = (len(values) - 1) * (pct / 100.0)
    lo = int(pos)
    hi = min(lo + 1, len(values) - 1)
    frac = pos - lo
    return values[lo] * (1.0 - frac) + values[hi] * frac


def metric_fails(row: dict, thresholds: dict) -> list[str]:
    fails = []
    for key, rule in thresholds.items():
        if rule is None or key.startswith("$"):
            continue
        val = row.get(key)
        if val is None:
            fails.append(key)
        elif "max" in rule and val > rule["max"]:
            fails.append(key)
        elif "min" in rule and val < rule["min"]:
            fails.append(key)
    return fails


def fmt(val: object, digits: int = 4) -> str:
    if val is None:
        return "-"
    if isinstance(val, str):
        return html.escape(val)
    return f"{float(val):.{digits}f}"


def repo_rel(value: object) -> object:
    if not isinstance(value, str):
        return value
    try:
        path = Path(value)
    except TypeError:
        return value
    if not path.is_absolute():
        return value
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return value


def normalize_paths(obj: object) -> object:
    if isinstance(obj, dict):
        return {key: normalize_paths(value) for key, value in obj.items()}
    if isinstance(obj, list):
        return [normalize_paths(value) for value in obj]
    return repo_rel(obj)


def summarize_group(rows: list[dict], thresholds: dict) -> dict:
    out: dict[str, object] = {
        "count": len(rows),
        "pass_count": 0,
        "fail_count": 0,
    }
    if not rows:
        return out
    for row in rows:
        if metric_fails(row, thresholds):
            out["fail_count"] += 1
        else:
            out["pass_count"] += 1
    for metric in METRICS:
        vals = [float(r[metric]) for r in rows if r.get(metric) is not None]
        if not vals:
            continue
        out[f"{metric}_median"] = statistics.median(vals)
        if metric in TAIL_HIGH_METRICS:
            out[f"{metric}_worst"] = max(vals)
            out[f"{metric}_p95"] = percentile(vals, 95.0)
        else:
            out[f"{metric}_worst"] = min(vals)
            out[f"{metric}_p05"] = percentile(vals, 5.0)
    enc_vals = [float(r["enc_ms"]) for r in rows if r.get("enc_ms") is not None]
    if enc_vals:
        out["enc_ms_median"] = statistics.median(enc_vals)
        out["enc_ms_p95"] = percentile(enc_vals, 95.0)
    byte_vals = [float(r["enc_bytes"]) for r in rows if r.get("enc_bytes") is not None]
    if byte_vals:
        out["enc_bytes_median"] = statistics.median(byte_vals)
        out["enc_bytes_p95"] = percentile(byte_vals, 95.0)
    return out


def summarize_run(run_hash: str, run_path: Path, run: dict, manifest: dict,
                  thresholds: dict) -> dict:
    manifest_images = {im["id"]: im for im in manifest["images"]}
    images = run.get("images") or {}
    rows = []
    missing = []
    for image_id, meta in manifest_images.items():
        metric_row = images.get(image_id)
        if not metric_row:
            missing.append(image_id)
            continue
        row = dict(metric_row)
        row["id"] = image_id
        row["character"] = meta.get("character", "")
        row["strata"] = list(meta.get("strata") or [meta.get("character", "unclassified")])
        row["fails"] = metric_fails(metric_row, thresholds)
        rows.append(row)
    if not rows:
        raise ValueError(f"{run_hash} has no images from {manifest.get('$doc', 'manifest')}")

    strata_rows: dict[str, list[dict]] = defaultdict(list)
    for row in rows:
        for stratum in row["strata"]:
            strata_rows[stratum].append(row)

    worst_first = sorted(rows, key=lambda r: float(r.get("lpips", 0.0)), reverse=True)
    failures = [r for r in worst_first if r["fails"]]
    return {
        "run_hash": run_hash,
        "run_path": repo_rel(str(run_path.resolve())),
        "pipeline": run.get("pipeline"),
        "ship_class": run.get("ship_class"),
        "run_verdict": run.get("verdict"),
        "verdict_authority": run.get("verdict_authority", "unknown"),
        "is_ship_gate": bool(run.get("is_ship_gate", False)),
        "image_count": len(rows),
        "missing_manifest_images": missing,
        "summary": summarize_group(rows, thresholds),
        "worst_first": normalize_paths(worst_first),
        "failures": normalize_paths(failures),
        "strata": {
            stratum: summarize_group(group_rows, thresholds)
            for stratum, group_rows in sorted(strata_rows.items())
        },
    }


def build_html(result: dict) -> str:
    css = """
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif; margin: 24px; background: #f6f6f2; color: #222; }
h1 { font-size: 24px; margin: 0 0 6px; }
h2 { font-size: 18px; margin: 28px 0 10px; }
.sub { color: #555; max-width: 1120px; line-height: 1.45; }
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 12px; margin: 18px 0; }
.card { background: #fff; border: 1px solid #d7d7d0; border-radius: 6px; padding: 12px; }
.card h3 { margin: 0 0 8px; font-size: 14px; }
.small { font-size: 12px; color: #555; line-height: 1.35; }
.pass { color: #0a6f2a; font-weight: 650; }
.fail { color: #a31621; font-weight: 650; }
.warn { color: #9a5a00; font-weight: 650; }
table { border-collapse: collapse; width: 100%; background: #fff; font-size: 12px; }
th, td { border: 1px solid #d9d9d2; padding: 6px 7px; text-align: right; vertical-align: top; }
th { background: #e9e9e2; position: sticky; top: 0; }
td.left, th.left { text-align: left; }
code { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: 11px; }
"""
    parts = [
        "<!doctype html><html><head><meta charset='utf-8'>",
        "<title>PREVIEW holdout summary</title>",
        f"<style>{css}</style></head><body>",
        "<h1>PREVIEW Holdout Summary</h1>",
        "<div class='sub'>Informational breadth evaluation only. The frozen four-image ship gate remains the only source of ship PASS/FAIL. "
        "Use these stats to compare candidates by worst image, p95 tail, median, and content stratum; do not average away a frozen-gate failure.</div>",
        "<div class='cards'>",
    ]
    for run in result["runs"]:
        s = run["summary"]
        status_cls = "pass" if s.get("fail_count", 0) == 0 and not run["missing_manifest_images"] else "fail"
        parts.append(f"""
<div class="card">
  <h3>{html.escape(str(run['pipeline']))}</h3>
  <div class="small"><code>{html.escape(run['run_hash'])}</code> authority={html.escape(str(run['verdict_authority']))}</div>
  <p class="{status_cls}">{s.get('pass_count', 0)}/{s.get('count', 0)} images pass PREVIEW thresholds</p>
  <p class="small">LPIPS worst {fmt(s.get('lpips_worst'))}, p95 {fmt(s.get('lpips_p95'))}, median {fmt(s.get('lpips_median'))}</p>
  <p class="small">MS-SSIM worst {fmt(s.get('ms_ssim_worst'))}, p05 {fmt(s.get('ms_ssim_p05'))}, median {fmt(s.get('ms_ssim_median'))}</p>
  <p class="small">dE worst {fmt(s.get('dE2000_mean_worst'))}, p95 {fmt(s.get('dE2000_mean_p95'))}, median {fmt(s.get('dE2000_mean_median'))}</p>
  <p class="small">missing manifest images: {len(run['missing_manifest_images'])}</p>
</div>""")
    parts.append("</div>")

    parts.append("<h2>Run Summary</h2>")
    parts.append("<table><thead><tr><th class='left'>run</th><th class='left'>pipeline</th><th>images</th><th>fail</th><th>LPIPS worst</th><th>LPIPS p95</th><th>MS-SSIM worst</th><th>MS-SSIM p05</th><th>Y-PSNR worst</th><th>dE worst</th><th>enc ms p95</th><th>bytes p95</th></tr></thead><tbody>")
    for run in result["runs"]:
        s = run["summary"]
        parts.append(
            "<tr>"
            f"<td class='left'><code>{html.escape(run['run_hash'])}</code></td>"
            f"<td class='left'>{html.escape(str(run['pipeline']))}</td>"
            f"<td>{s.get('count', 0)}</td>"
            f"<td class='{'pass' if s.get('fail_count', 0) == 0 else 'fail'}'>{s.get('fail_count', 0)}</td>"
            f"<td>{fmt(s.get('lpips_worst'))}</td>"
            f"<td>{fmt(s.get('lpips_p95'))}</td>"
            f"<td>{fmt(s.get('ms_ssim_worst'))}</td>"
            f"<td>{fmt(s.get('ms_ssim_p05'))}</td>"
            f"<td>{fmt(s.get('y_psnr_worst'), 2)}</td>"
            f"<td>{fmt(s.get('dE2000_mean_worst'), 2)}</td>"
            f"<td>{fmt(s.get('enc_ms_p95'), 1)}</td>"
            f"<td>{fmt(s.get('enc_bytes_p95'), 0)}</td>"
            "</tr>"
        )
    parts.append("</tbody></table>")

    for run in result["runs"]:
        parts.append(f"<h2>Worst Images: <code>{html.escape(run['run_hash'])}</code></h2>")
        parts.append("<table><thead><tr><th class='left'>image</th><th class='left'>character</th><th class='left'>strata</th><th>LPIPS</th><th>MS-SSIM</th><th>Y-PSNR</th><th>dE</th><th class='left'>fails</th></tr></thead><tbody>")
        for row in run["worst_first"][:20]:
            fail_cls = "fail" if row["fails"] else "pass"
            parts.append(
                "<tr>"
                f"<td class='left'>{html.escape(row['id'])}</td>"
                f"<td class='left'>{html.escape(row['character'])}</td>"
                f"<td class='left'>{html.escape(', '.join(row['strata']))}</td>"
                f"<td>{fmt(row.get('lpips'))}</td>"
                f"<td>{fmt(row.get('ms_ssim'))}</td>"
                f"<td>{fmt(row.get('y_psnr'), 2)}</td>"
                f"<td>{fmt(row.get('dE2000_mean'), 2)}</td>"
                f"<td class='left {fail_cls}'>{html.escape(', '.join(row['fails']) or 'PASS')}</td>"
                "</tr>"
            )
        parts.append("</tbody></table>")

        parts.append(f"<h2>Per-Stratum: <code>{html.escape(run['run_hash'])}</code></h2>")
        parts.append("<table><thead><tr><th class='left'>stratum</th><th>images</th><th>fail</th><th>LPIPS worst</th><th>LPIPS p95</th><th>MS-SSIM worst</th><th>Y-PSNR worst</th><th>dE worst</th></tr></thead><tbody>")
        for stratum, s in run["strata"].items():
            parts.append(
                "<tr>"
                f"<td class='left'>{html.escape(stratum)}</td>"
                f"<td>{s.get('count', 0)}</td>"
                f"<td class='{'pass' if s.get('fail_count', 0) == 0 else 'fail'}'>{s.get('fail_count', 0)}</td>"
                f"<td>{fmt(s.get('lpips_worst'))}</td>"
                f"<td>{fmt(s.get('lpips_p95'))}</td>"
                f"<td>{fmt(s.get('ms_ssim_worst'))}</td>"
                f"<td>{fmt(s.get('y_psnr_worst'), 2)}</td>"
                f"<td>{fmt(s.get('dE2000_mean_worst'), 2)}</td>"
                "</tr>"
            )
        parts.append("</tbody></table>")

        if run["missing_manifest_images"]:
            missing = ", ".join(run["missing_manifest_images"])
            parts.append(f"<p class='warn'>Missing manifest images for {html.escape(run['run_hash'])}: {html.escape(missing)}</p>")

    parts.append("</body></html>")
    return "\n".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("runs", nargs="+", help="Run hash, run dir, or run.json path")
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--ship-class", default="PREVIEW")
    ap.add_argument("--output-json", type=Path,
                    default=DASH / "preview_holdout_summary.json")
    ap.add_argument("--output-html", type=Path,
                    default=DASH / "preview_holdout_summary.html")
    ap.add_argument("--require-complete", action="store_true",
                    help="Exit non-zero when a run is missing any manifest image")
    args = ap.parse_args()

    manifest = load_json(args.manifest)
    gates = load_json(GATES)
    thresholds = gates["ship_classes"][args.ship_class]["per_image"]

    summaries = []
    for run_arg in args.runs:
        run_hash, run_path, run = resolve_run(run_arg)
        summaries.append(summarize_run(run_hash, run_path, run, manifest, thresholds))

    result = {
        "manifest": repo_rel(str(args.manifest.resolve())),
        "ship_class": args.ship_class,
        "image_count": len(manifest["images"]),
        "runs": summaries,
    }

    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(result, indent=2))
    args.output_html.parent.mkdir(parents=True, exist_ok=True)
    args.output_html.write_text(build_html(result))

    print(f"preview holdout json: {args.output_json}")
    print(f"preview holdout html: {args.output_html}")
    for run in summaries:
        s = run["summary"]
        print(
            f"{run['run_hash']} images={s.get('count', 0)} "
            f"fail={s.get('fail_count', 0)} "
            f"lpips_worst={fmt(s.get('lpips_worst'))} "
            f"lpips_p95={fmt(s.get('lpips_p95'))}"
        )

    if args.require_complete and any(r["missing_manifest_images"] for r in summaries):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
